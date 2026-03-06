#include "localbackend.h"

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
        QString metaPath = bakPath.chopped(4) + QStringLiteral(".meta");

        // Write .bak file atomically
        {
            QSaveFile file(bakPath);
            if (!file.open(QIODevice::WriteOnly)) {
                QMetaObject::invokeMethod(qApp, [self,
                        reason = LocalBackend::tr("Failed to open backup file for writing")] {
                    if (!self) return;
                    emit self->writeFailed(reason);
                }, Qt::QueuedConnection);
                return;
            }
            file.write(data);
            if (!file.commit()) {
                QMetaObject::invokeMethod(qApp, [self,
                        reason = LocalBackend::tr("Failed to write backup file")] {
                    if (!self) return;
                    emit self->writeFailed(reason);
                }, Qt::QueuedConnection);
                return;
            }
        }

        // Write .meta file atomically
        {
            QSaveFile file(metaPath);
            if (!file.open(QIODevice::WriteOnly)) {
                QMetaObject::invokeMethod(qApp, [self,
                        reason = LocalBackend::tr("Failed to open metadata file for writing")] {
                    if (!self) return;
                    emit self->writeFailed(reason);
                }, Qt::QueuedConnection);
                return;
            }
            file.write(QJsonDocument(meta).toJson(QJsonDocument::Compact));
            if (!file.commit()) {
                QMetaObject::invokeMethod(qApp, [self,
                        reason = LocalBackend::tr("Failed to write metadata file")] {
                    if (!self) return;
                    emit self->writeFailed(reason);
                }, Qt::QueuedConnection);
                return;
            }
        }

        QMetaObject::invokeMethod(qApp, [self, filename] {
            if (!self) return;
            emit self->writeSucceeded(filename);
        }, Qt::QueuedConnection);
    });
}

void LocalBackend::readBackup(const QString &filename)
{
    QString dir = backupDir();
    QPointer<LocalBackend> self(this);
    (void)QtConcurrent::run([self, dir, filename] {
        QString bakPath = dir + QLatin1Char('/') + filename;
        QString metaPath = bakPath.chopped(4) + QStringLiteral(".meta");

        QFile bakFile(bakPath);
        if (!bakFile.open(QIODevice::ReadOnly)) {
            QMetaObject::invokeMethod(qApp, [self, filename,
                    reason = LocalBackend::tr("Failed to open backup file for reading")] {
                if (!self) return;
                emit self->readFailed(filename, reason);
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
            emit self->readSucceeded(filename, data, meta);
        }, Qt::QueuedConnection);
    });
}

void LocalBackend::deleteBackup(const QString &filename)
{
    QString dir = backupDir();
    QPointer<LocalBackend> self(this);
    (void)QtConcurrent::run([self, dir, filename] {
        QString bakPath = dir + QLatin1Char('/') + filename;
        QString metaPath = bakPath.chopped(4) + QStringLiteral(".meta");

        bool ok = QFile::remove(bakPath);
        QFile::remove(metaPath); // Best effort

        QMetaObject::invokeMethod(qApp, [self, filename, ok] {
            if (!self) return;
            emit self->deleteCompleted(filename, ok,
                                 ok ? QString() : LocalBackend::tr("Failed to delete backup file"));
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
            QStringLiteral("^qtcloudbackup_(.+)_(\\d{8}_\\d{6}_\\d{3})_[a-z0-9]{4}\\.bak$"));

        QList<BackupInfo> backups;
        for (const QString &entry : entries) {
            BackupInfo info;
            info.filename = entry;
            info.downloadState = QtCloudBackup::DownloadState::Local;

            // Try to read .meta sidecar (bounded)
            QString metaPath = dir + QLatin1Char('/') + entry.chopped(4) + QStringLiteral(".meta");
            QFile metaFile(metaPath);
            if (metaFile.open(QIODevice::ReadOnly)) {
                QJsonObject meta = QJsonDocument::fromJson(metaFile.read(MaxMetaFileSize)).object();
                info.sourceId = meta[QStringLiteral("sourceId")].toString();
                info.timestamp = QDateTime::fromString(meta[QStringLiteral("timestamp")].toString(),
                                                        Qt::ISODateWithMs);
                info.metadata = meta[QStringLiteral("metadata")].toObject().toVariantMap();
            }

            // Fallback: parse filename
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
        emit downloadCompleted(filename, true, QString());
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
    emit migrationCompleted(true, 0, {});
}
