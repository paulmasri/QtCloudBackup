#include "localbackend.h"

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
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

QString LocalBackend::backupDir() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
           + QStringLiteral("/Backups");
}

void LocalBackend::initialise()
{
    (void)QtConcurrent::run([this] {
        QDir dir(backupDir());
        bool ok = dir.exists() || dir.mkpath(QStringLiteral("."));

        auto status = ok ? QtCloudBackup::StorageStatus::LocalFallback
                         : QtCloudBackup::StorageStatus::Unavailable;
        auto detail = ok ? tr("Using local storage")
                         : tr("Failed to create backup directory");

        QMetaObject::invokeMethod(this, [this, status, detail] {
            m_status = status;
            m_statusDetail = detail;
            emit statusChanged(status, detail);
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
    (void)QtConcurrent::run([this, dir, filename, data, meta] {
        QString bakPath = dir + QLatin1Char('/') + filename;
        QString metaPath = bakPath;
        metaPath.replace(QStringLiteral(".bak"), QStringLiteral(".meta"));

        // Write .bak file atomically
        {
            QSaveFile file(bakPath);
            if (!file.open(QIODevice::WriteOnly)) {
                QMetaObject::invokeMethod(this, [this, reason = tr("Failed to open backup file for writing")] {
                    emit writeFailed(reason);
                }, Qt::QueuedConnection);
                return;
            }
            file.write(data);
            if (!file.commit()) {
                QMetaObject::invokeMethod(this, [this, reason = tr("Failed to write backup file")] {
                    emit writeFailed(reason);
                }, Qt::QueuedConnection);
                return;
            }
        }

        // Write .meta file atomically
        {
            QSaveFile file(metaPath);
            if (!file.open(QIODevice::WriteOnly)) {
                QMetaObject::invokeMethod(this, [this, reason = tr("Failed to open metadata file for writing")] {
                    emit writeFailed(reason);
                }, Qt::QueuedConnection);
                return;
            }
            file.write(QJsonDocument(meta).toJson(QJsonDocument::Compact));
            if (!file.commit()) {
                QMetaObject::invokeMethod(this, [this, reason = tr("Failed to write metadata file")] {
                    emit writeFailed(reason);
                }, Qt::QueuedConnection);
                return;
            }
        }

        QMetaObject::invokeMethod(this, [this, filename] {
            emit writeSucceeded(filename);
        }, Qt::QueuedConnection);
    });
}

void LocalBackend::readBackup(const QString &filename)
{
    QString dir = backupDir();
    (void)QtConcurrent::run([this, dir, filename] {
        QString bakPath = dir + QLatin1Char('/') + filename;
        QString metaPath = bakPath;
        metaPath.replace(QStringLiteral(".bak"), QStringLiteral(".meta"));

        QFile bakFile(bakPath);
        if (!bakFile.open(QIODevice::ReadOnly)) {
            QMetaObject::invokeMethod(this, [this, filename, reason = tr("Failed to open backup file for reading")] {
                emit readFailed(filename, reason);
            }, Qt::QueuedConnection);
            return;
        }
        QByteArray data = bakFile.readAll();

        QJsonObject meta;
        QFile metaFile(metaPath);
        if (metaFile.open(QIODevice::ReadOnly)) {
            meta = QJsonDocument::fromJson(metaFile.readAll()).object();
        }

        QMetaObject::invokeMethod(this, [this, filename, data, meta] {
            emit readSucceeded(filename, data, meta);
        }, Qt::QueuedConnection);
    });
}

void LocalBackend::deleteBackup(const QString &filename)
{
    QString dir = backupDir();
    (void)QtConcurrent::run([this, dir, filename] {
        QString bakPath = dir + QLatin1Char('/') + filename;
        QString metaPath = bakPath;
        metaPath.replace(QStringLiteral(".bak"), QStringLiteral(".meta"));

        bool ok = QFile::remove(bakPath);
        QFile::remove(metaPath); // Best effort

        QMetaObject::invokeMethod(this, [this, filename, ok] {
            emit deleteCompleted(filename, ok,
                                 ok ? QString() : tr("Failed to delete backup file"));
        }, Qt::QueuedConnection);
    });
}

void LocalBackend::scanBackups()
{
    QString dir = backupDir();
    (void)QtConcurrent::run([this, dir] {
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

            // Try to read .meta sidecar
            QString metaPath = dir + QLatin1Char('/') + entry;
            metaPath.replace(QStringLiteral(".bak"), QStringLiteral(".meta"));
            QFile metaFile(metaPath);
            if (metaFile.open(QIODevice::ReadOnly)) {
                QJsonObject meta = QJsonDocument::fromJson(metaFile.readAll()).object();
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

        QMetaObject::invokeMethod(this, [this, backups] {
            emit scanCompleted(backups);
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
