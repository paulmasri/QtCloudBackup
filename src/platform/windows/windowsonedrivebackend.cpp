#include "windowsonedrivebackend.h"

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QtConcurrent>

#include <qt_windows.h>
#include <shlobj.h>

#ifndef QTCLOUDBACKUP_WINDOWS_BACKUP_PATH
#define QTCLOUDBACKUP_WINDOWS_BACKUP_PATH ""
#endif

struct OneDriveCandidate {
    QString path;
    QtCloudBackup::StorageType type;
};

static QList<OneDriveCandidate> detectOneDriveCandidates()
{
    QList<OneDriveCandidate> candidates;

    // 1. %OneDriveCommercial%
    QString val = qEnvironmentVariable("OneDriveCommercial");
    if (!val.isEmpty())
        candidates.append({val, QtCloudBackup::StorageType::OneDriveCommercial});

    // 2. Registry Business1
    {
        QSettings reg(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\OneDrive\\Accounts\\Business1"),
                       QSettings::NativeFormat);
        val = reg.value(QStringLiteral("UserFolder")).toString();
        if (!val.isEmpty())
            candidates.append({val, QtCloudBackup::StorageType::OneDriveCommercial});
    }

    // 3. %OneDriveConsumer%
    val = qEnvironmentVariable("OneDriveConsumer");
    if (!val.isEmpty())
        candidates.append({val, QtCloudBackup::StorageType::OneDrivePersonal});

    // 4. Registry Personal
    {
        QSettings reg(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\OneDrive\\Accounts\\Personal"),
                       QSettings::NativeFormat);
        val = reg.value(QStringLiteral("UserFolder")).toString();
        if (!val.isEmpty())
            candidates.append({val, QtCloudBackup::StorageType::OneDrivePersonal});
    }

    // 5. %OneDrive% (assume personal)
    val = qEnvironmentVariable("OneDrive");
    if (!val.isEmpty())
        candidates.append({val, QtCloudBackup::StorageType::OneDrivePersonal});

    // 6. Documents folder fallback via SHGetKnownFolderPath
    {
        PWSTR path = nullptr;
        HRESULT hr = SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &path);
        if (SUCCEEDED(hr) && path) {
            candidates.append({QString::fromWCharArray(path), QtCloudBackup::StorageType::LocalDirectory});
            CoTaskMemFree(path);
        }
    }

    return candidates;
}

std::unique_ptr<CloudBackupBackend> createPlatformBackend()
{
    return std::make_unique<WindowsOneDriveBackend>();
}

QString WindowsOneDriveBackend::backupDir() const
{
    const QString relPath = QStringLiteral(QTCLOUDBACKUP_WINDOWS_BACKUP_PATH);
    if (relPath.isEmpty())
        return m_backupRoot;
    return m_backupRoot + QLatin1Char('/') + relPath;
}

void WindowsOneDriveBackend::initialise()
{
    (void)QtConcurrent::run([this] {
        auto candidates = detectOneDriveCandidates();

        QString chosenRoot;
        auto chosenType = QtCloudBackup::StorageType::None;

        for (const auto &c : candidates) {
            if (QDir(c.path).exists()) {
                chosenRoot = c.path;
                chosenType = c.type;
                break;
            }
        }

        if (chosenRoot.isEmpty()) {
            QMetaObject::invokeMethod(this, [this] {
                m_status = QtCloudBackup::StorageStatus::Unavailable;
                m_statusDetail = tr("No OneDrive or Documents folder found");
                m_storageType = QtCloudBackup::StorageType::None;
                emit statusChanged(m_status, m_statusDetail);
            }, Qt::QueuedConnection);
            return;
        }

        // Ensure backup subdirectory exists
        const QString relPath = QStringLiteral(QTCLOUDBACKUP_WINDOWS_BACKUP_PATH);
        QString fullDir = relPath.isEmpty() ? chosenRoot : chosenRoot + QLatin1Char('/') + relPath;
        QDir dir(fullDir);
        bool ok = dir.exists() || dir.mkpath(QStringLiteral("."));

        auto status = QtCloudBackup::StorageStatus::Unavailable;
        QString detail;

        if (!ok) {
            detail = tr("Failed to create backup directory");
        } else {
            switch (chosenType) {
            case QtCloudBackup::StorageType::OneDriveCommercial:
                status = QtCloudBackup::StorageStatus::Ready;
                detail = tr("Using OneDrive for Business");
                break;
            case QtCloudBackup::StorageType::OneDrivePersonal:
                status = QtCloudBackup::StorageStatus::Ready;
                detail = tr("Using OneDrive");
                break;
            case QtCloudBackup::StorageType::LocalDirectory:
                status = QtCloudBackup::StorageStatus::LocalFallback;
                detail = tr("Using local Documents folder");
                break;
            default:
                status = QtCloudBackup::StorageStatus::LocalFallback;
                detail = tr("Using local storage");
                break;
            }
        }

        auto finalType = ok ? chosenType : QtCloudBackup::StorageType::None;
        auto finalRoot = ok ? chosenRoot : QString();

        QMetaObject::invokeMethod(this, [this, status, detail, finalType, finalRoot] {
            m_backupRoot = finalRoot;
            m_status = status;
            m_statusDetail = detail;
            m_storageType = finalType;
            emit statusChanged(m_status, m_statusDetail);
        }, Qt::QueuedConnection);
    });
}

QtCloudBackup::StorageStatus WindowsOneDriveBackend::storageStatus() const
{
    return m_status;
}

QString WindowsOneDriveBackend::statusDetail() const
{
    return m_statusDetail;
}

QtCloudBackup::StorageType WindowsOneDriveBackend::storageType() const
{
    return m_storageType;
}

void WindowsOneDriveBackend::writeBackup(const QString &filename, const QByteArray &data,
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

void WindowsOneDriveBackend::readBackup(const QString &filename)
{
    QString dir = backupDir();
    (void)QtConcurrent::run([this, dir, filename] {
        QString bakPath = dir + QLatin1Char('/') + filename;
        QString metaPath = bakPath;
        metaPath.replace(QStringLiteral(".bak"), QStringLiteral(".meta"));

        // Emit indeterminate progress — QFile::open() may block during hydration
        QMetaObject::invokeMethod(this, [this, filename] {
            emit downloadProgress(filename, 0, -1);
        }, Qt::QueuedConnection);

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

void WindowsOneDriveBackend::deleteBackup(const QString &filename)
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

void WindowsOneDriveBackend::scanBackups()
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

            // Check download state via FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
            QString fullPath = dir + QLatin1Char('/') + entry;
            DWORD attrs = GetFileAttributesW(reinterpret_cast<LPCWSTR>(fullPath.utf16()));
            if (attrs != INVALID_FILE_ATTRIBUTES
                && (attrs & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) {
                info.downloadState = QtCloudBackup::DownloadState::CloudOnly;
            } else {
                info.downloadState = QtCloudBackup::DownloadState::Local;
            }

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

void WindowsOneDriveBackend::triggerDownload(const QString &filename)
{
    QString dir = backupDir();
    (void)QtConcurrent::run([this, dir, filename] {
        QString fullPath = dir + QLatin1Char('/') + filename;

        // Emit indeterminate progress
        QMetaObject::invokeMethod(this, [this, filename] {
            emit downloadProgress(filename, 0, -1);
        }, Qt::QueuedConnection);

        // Opening the file triggers transparent hydration on OneDrive
        QFile file(fullPath);
        bool ok = file.open(QIODevice::ReadOnly);
        if (ok)
            file.close();

        QMetaObject::invokeMethod(this, [this, filename, ok] {
            emit downloadCompleted(filename, ok,
                                   ok ? QString() : tr("Failed to download backup file"));
        }, Qt::QueuedConnection);
    });
}
