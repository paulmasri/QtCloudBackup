#include "localbackend.h"
#include "backupvalidation.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtConcurrent>

// Platform backends provide their own createPlatformBackend().
// The fallback is used only when no platform backend is compiled in.
#if !defined(Q_OS_WIN) && !defined(Q_OS_APPLE)
std::unique_ptr<CloudBackupBackend> createPlatformBackend()
{
    return std::make_unique<LocalBackend>();
}
#endif

static constexpr qint64 MaxMetaFileSize = 1024 * 1024; // 1 MB

QString LocalBackend::backupDir() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
           + QStringLiteral("/Backups");
}

void LocalBackend::initialise()
{
    QString dir = backupDir();
    QPointer<LocalBackend> self(this);
    (void)QtConcurrent::run([self, dir] {
        QDir d(dir);
        bool ok = d.exists() || d.mkpath(QStringLiteral("."));

        auto status = ok ? QtCloudBackup::StorageStatus::LocalFallback
                         : QtCloudBackup::StorageStatus::Unavailable;
        auto detail = ok ? LocalBackend::tr("Using local storage")
                         : LocalBackend::tr("Failed to create backup directory");

        QMetaObject::invokeMethod(qApp, [self, status, detail] {
            if (!self) return;
            self->m_status = status;
            self->m_statusDetail = detail;
            emit self->statusChanged(status, detail);
        }, Qt::QueuedConnection);
    });
}

QtCloudBackup::StorageStatus LocalBackend::storageStatus() const
{
    return m_status;
}

QString LocalBackend::statusDetail() const
{
    return m_statusDetail;
}

QtCloudBackup::StorageType LocalBackend::storageType() const
{
    return QtCloudBackup::StorageType::LocalDirectory;
}

void LocalBackend::writeBackup(const QString &filename, const QByteArray &data,
                                const QJsonObject &meta)
{
    QString dir = backupDir();
    QPointer<LocalBackend> self(this);
    (void)QtConcurrent::run([self, dir, filename, data, meta] {
        QString bakPath = dir + QLatin1Char('/') + filename;
        QString metaPath = dir + QLatin1Char('/') + backupStem(filename) + QStringLiteral(".meta");

        // Write .bak file atomically
        {
            QSaveFile file(bakPath);
            if (!file.open(QIODevice::WriteOnly)) {
                QMetaObject::invokeMethod(qApp, [self, filename,
                        msg = LocalBackend::tr("Failed to open backup file for writing")] {
                    if (!self) return;
                    emit self->writeCompleted(filename,
                        int(QtCloudBackup::BackupError::IOError), msg);
                }, Qt::QueuedConnection);
                return;
            }
            file.write(data);
            if (!file.commit()) {
                QMetaObject::invokeMethod(qApp, [self, filename,
                        msg = LocalBackend::tr("Failed to write backup file")] {
                    if (!self) return;
                    emit self->writeCompleted(filename,
                        int(QtCloudBackup::BackupError::IOError), msg);
                }, Qt::QueuedConnection);
                return;
            }
        }

        // Write .meta file atomically. On failure, roll back the .bak so the
        // writer-local invariant (complete backup ⇔ both files exist) holds.
        {
            QSaveFile file(metaPath);
            if (!file.open(QIODevice::WriteOnly)) {
                QFile::remove(bakPath);
                QMetaObject::invokeMethod(qApp, [self, filename,
                        msg = LocalBackend::tr("Failed to open metadata file for writing")] {
                    if (!self) return;
                    emit self->writeCompleted(filename,
                        int(QtCloudBackup::BackupError::MetadataIOError), msg);
                }, Qt::QueuedConnection);
                return;
            }
            file.write(QJsonDocument(meta).toJson(QJsonDocument::Compact));
            if (!file.commit()) {
                QFile::remove(bakPath);
                QMetaObject::invokeMethod(qApp, [self, filename,
                        msg = LocalBackend::tr("Failed to write metadata file")] {
                    if (!self) return;
                    emit self->writeCompleted(filename,
                        int(QtCloudBackup::BackupError::MetadataIOError), msg);
                }, Qt::QueuedConnection);
                return;
            }
        }

        QMetaObject::invokeMethod(qApp, [self, filename] {
            if (!self) return;
            emit self->writeCompleted(filename,
                int(QtCloudBackup::BackupError::NoError), QString());
        }, Qt::QueuedConnection);
    });
}

void LocalBackend::readBackup(const QString &filename)
{
    QString dir = backupDir();
    QPointer<LocalBackend> self(this);
    (void)QtConcurrent::run([self, dir, filename] {
        QString bakPath = dir + QLatin1Char('/') + filename;
        QString metaPath = dir + QLatin1Char('/') + backupStem(filename) + QStringLiteral(".meta");

        QFile bakFile(bakPath);
        if (!bakFile.open(QIODevice::ReadOnly)) {
            QMetaObject::invokeMethod(qApp, [self, filename,
                    msg = LocalBackend::tr("Failed to open backup file for reading")] {
                if (!self) return;
                emit self->readCompleted(filename, {}, {},
                    int(QtCloudBackup::BackupError::IOError), msg);
            }, Qt::QueuedConnection);
            return;
        }
        QByteArray data = bakFile.readAll();

        QJsonObject meta;
        QFile metaFile(metaPath);
        if (metaFile.open(QIODevice::ReadOnly)) {
            meta = QJsonDocument::fromJson(metaFile.read(MaxMetaFileSize)).object();
        }

        QMetaObject::invokeMethod(qApp, [self, filename, data, meta] {
            if (!self) return;
            emit self->readCompleted(filename, data, meta,
                int(QtCloudBackup::BackupError::NoError), QString());
        }, Qt::QueuedConnection);
    });
}

void LocalBackend::deleteBackup(const QString &filename)
{
    QString dir = backupDir();
    QPointer<LocalBackend> self(this);
    (void)QtConcurrent::run([self, dir, filename] {
        QString bakPath = dir + QLatin1Char('/') + filename;
        QString metaPath = dir + QLatin1Char('/') + backupStem(filename) + QStringLiteral(".meta");

        // Delete .meta first (the completion marker), then .bak. Mirrors the
        // write protocol: .bak written first, .meta last. If interrupted after
        // .meta removal but before .bak removal, the .bak is left as an orphan
        // which the scanner surfaces with metadataAvailable=false rather than
        // an invisible orphan .meta.
        if (!QFile::remove(metaPath) && QFile::exists(metaPath))
            qWarning("Failed to remove metadata sidecar: %s", qPrintable(metaPath));
        bool ok = QFile::remove(bakPath);

        int err = ok ? int(QtCloudBackup::BackupError::NoError)
                     : int(QtCloudBackup::BackupError::IOError);
        QString msg = ok ? QString() : LocalBackend::tr("Failed to delete backup file");
        QMetaObject::invokeMethod(qApp, [self, filename, err, msg] {
            if (!self) return;
            emit self->deleteCompleted(filename, err, msg);
        }, Qt::QueuedConnection);
    });
}

void LocalBackend::scanBackups()
{
    QString dir = backupDir();
    QPointer<LocalBackend> self(this);
    (void)QtConcurrent::run([self, dir] {
        QDir d(dir);
        QStringList entries = d.entryList({QStringLiteral("qtcloudbackup_*.bak")},
                                          QDir::Files, QDir::Name);

        static const QRegularExpression re(
            QStringLiteral("^qtcloudbackup_([a-zA-Z0-9_-]{1,64})_(\\d{8}_\\d{6}_\\d{3})_[a-z0-9]{4}\\.bak$"));

        QList<BackupInfo> backups;
        for (const QString &entry : entries) {
            BackupInfo info;
            info.filename = entry;
            info.downloadState = QtCloudBackup::DownloadState::Local;

            // Try to read .meta sidecar (bounded). A missing .meta is not
            // junk — under cloud sync it may be syncing, evicted, or it may
            // be a stale orphan from an interrupted delete. Surface honestly
            // via metadataAvailable=false and let the consumer/retention
            // decide how to treat it.
            QString metaPath = dir + QLatin1Char('/') + backupStem(entry) + QStringLiteral(".meta");
            QFile metaFile(metaPath);
            if (metaFile.open(QIODevice::ReadOnly)) {
                QJsonObject meta = QJsonDocument::fromJson(metaFile.read(MaxMetaFileSize)).object();
                info.sourceId = meta[QStringLiteral("sourceId")].toString();
                info.timestamp = QDateTime::fromString(meta[QStringLiteral("timestamp")].toString(),
                                                        Qt::ISODateWithMs);
                info.metadata = meta[QStringLiteral("metadata")].toObject().toVariantMap();
            } else {
                info.metadataAvailable = false;
            }

            // Fallback: parse filename (always needed when .meta is missing,
            // and a safety net when .meta is malformed)
            if (info.sourceId.isEmpty() || !info.timestamp.isValid()) {
                auto match = re.match(entry);
                if (match.hasMatch()) {
                    info.sourceId = match.captured(1);
                    info.timestamp = QDateTime::fromString(match.captured(2),
                                                            QStringLiteral("yyyyMMdd_HHmmss_zzz"));
                    info.timestamp.setTimeZone(QTimeZone::utc());
                }
            }

            backups.append(info);
        }

        QMetaObject::invokeMethod(qApp, [self, backups] {
            if (!self) return;
            emit self->scanCompleted(backups);
        }, Qt::QueuedConnection);
    });
}

void LocalBackend::triggerDownload(const QString &filename)
{
    // Local files are always available — emit success immediately
    QMetaObject::invokeMethod(this, [this, filename] {
        emit downloadCompleted(filename,
            int(QtCloudBackup::BackupError::NoError), QString());
    }, Qt::QueuedConnection);
}

void LocalBackend::scanOrphanedBackups()
{
    // Nothing below local — no orphans possible
    emit orphanScanCompleted({});
}

void LocalBackend::migrateOrphanedBackups(const QList<OrphanedBackupInfo> &)
{
    // Nothing to migrate
    emit migrationCompleted(0, int(QtCloudBackup::BackupError::NoError), QString());
}
