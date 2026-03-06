#include "windowsonedrivebackend.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QtConcurrent>

#include <qt_windows.h>
#include <shlobj.h>

#ifndef QTCLOUDBACKUP_WINDOWS_BACKUP_PATH
#define QTCLOUDBACKUP_WINDOWS_BACKUP_PATH ""
#endif

static constexpr qint64 MaxMetaFileSize = 1024 * 1024; // 1 MB

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

QString WindowsOneDriveBackend::localFallbackDir() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
           + QStringLiteral("/Backups");
}

void WindowsOneDriveBackend::initialise()
{
    QPointer<WindowsOneDriveBackend> self(this);
    (void)QtConcurrent::run([self] {
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
            QMetaObject::invokeMethod(qApp, [self] {
                if (!self) return;
                self->m_status = QtCloudBackup::StorageStatus::Unavailable;
                self->m_statusDetail = WindowsOneDriveBackend::tr("No OneDrive or Documents folder found");
                self->m_storageType = QtCloudBackup::StorageType::None;
                emit self->statusChanged(self->m_status, self->m_statusDetail);
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
            detail = WindowsOneDriveBackend::tr("Failed to create backup directory");
        } else {
            switch (chosenType) {
            case QtCloudBackup::StorageType::OneDriveCommercial:
                status = QtCloudBackup::StorageStatus::Ready;
                detail = WindowsOneDriveBackend::tr("Using OneDrive for Business");
                break;
            case QtCloudBackup::StorageType::OneDrivePersonal:
                status = QtCloudBackup::StorageStatus::Ready;
                detail = WindowsOneDriveBackend::tr("Using OneDrive");
                break;
            case QtCloudBackup::StorageType::LocalDirectory:
                status = QtCloudBackup::StorageStatus::LocalFallback;
                detail = WindowsOneDriveBackend::tr("Using local Documents folder");
                break;
            default:
                status = QtCloudBackup::StorageStatus::LocalFallback;
                detail = WindowsOneDriveBackend::tr("Using local storage");
                break;
            }
        }

        auto finalType = ok ? chosenType : QtCloudBackup::StorageType::None;
        auto finalRoot = ok ? chosenRoot : QString();

        QMetaObject::invokeMethod(qApp, [self, status, detail, finalType, finalRoot] {
            if (!self) return;
            self->m_backupRoot = finalRoot;
            self->m_status = status;
            self->m_statusDetail = detail;
            self->m_storageType = finalType;
            emit self->statusChanged(self->m_status, self->m_statusDetail);
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
    QPointer<WindowsOneDriveBackend> self(this);
    (void)QtConcurrent::run([self, dir, filename, data, meta] {
        QString bakPath = dir + QLatin1Char('/') + filename;
        QString metaPath = bakPath.chopped(4) + QStringLiteral(".meta");

        // Write .bak file atomically
        {
            QSaveFile file(bakPath);
            if (!file.open(QIODevice::WriteOnly)) {
                QMetaObject::invokeMethod(qApp, [self,
                        reason = WindowsOneDriveBackend::tr("Failed to open backup file for writing")] {
                    if (!self) return;
                    emit self->writeFailed(reason);
                }, Qt::QueuedConnection);
                return;
            }
            file.write(data);
            if (!file.commit()) {
                QMetaObject::invokeMethod(qApp, [self,
                        reason = WindowsOneDriveBackend::tr("Failed to write backup file")] {
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
                        reason = WindowsOneDriveBackend::tr("Failed to open metadata file for writing")] {
                    if (!self) return;
                    emit self->writeFailed(reason);
                }, Qt::QueuedConnection);
                return;
            }
            file.write(QJsonDocument(meta).toJson(QJsonDocument::Compact));
            if (!file.commit()) {
                QMetaObject::invokeMethod(qApp, [self,
                        reason = WindowsOneDriveBackend::tr("Failed to write metadata file")] {
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

void WindowsOneDriveBackend::readBackup(const QString &filename)
{
    QString dir = backupDir();
    QPointer<WindowsOneDriveBackend> self(this);
    (void)QtConcurrent::run([self, dir, filename] {
        QString bakPath = dir + QLatin1Char('/') + filename;
        QString metaPath = bakPath.chopped(4) + QStringLiteral(".meta");

        // Emit indeterminate progress — QFile::open() may block during hydration
        QMetaObject::invokeMethod(qApp, [self, filename] {
            if (!self) return;
            emit self->downloadProgress(filename, 0, -1);
        }, Qt::QueuedConnection);

        QFile bakFile(bakPath);
        if (!bakFile.open(QIODevice::ReadOnly)) {
            QMetaObject::invokeMethod(qApp, [self, filename,
                    reason = WindowsOneDriveBackend::tr("Failed to open backup file for reading")] {
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

void WindowsOneDriveBackend::deleteBackup(const QString &filename)
{
    QString dir = backupDir();
    QPointer<WindowsOneDriveBackend> self(this);
    (void)QtConcurrent::run([self, dir, filename] {
        QString bakPath = dir + QLatin1Char('/') + filename;
        QString metaPath = bakPath.chopped(4) + QStringLiteral(".meta");

        bool ok = QFile::remove(bakPath);
        QFile::remove(metaPath); // Best effort

        QMetaObject::invokeMethod(qApp, [self, filename, ok] {
            if (!self) return;
            emit self->deleteCompleted(filename, ok,
                                 ok ? QString() : WindowsOneDriveBackend::tr("Failed to delete backup file"));
        }, Qt::QueuedConnection);
    });
}

void WindowsOneDriveBackend::scanBackups()
{
    QString dir = backupDir();
    QPointer<WindowsOneDriveBackend> self(this);
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

            // Check download state via FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
            QString fullPath = dir + QLatin1Char('/') + entry;
            DWORD attrs = GetFileAttributesW(reinterpret_cast<LPCWSTR>(fullPath.utf16()));
            if (attrs != INVALID_FILE_ATTRIBUTES
                && (attrs & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) {
                info.downloadState = QtCloudBackup::DownloadState::CloudOnly;
            } else {
                info.downloadState = QtCloudBackup::DownloadState::Local;
            }

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

void WindowsOneDriveBackend::triggerDownload(const QString &filename)
{
    QString dir = backupDir();
    QPointer<WindowsOneDriveBackend> self(this);
    (void)QtConcurrent::run([self, dir, filename] {
        QString fullPath = dir + QLatin1Char('/') + filename;

        // Emit indeterminate progress
        QMetaObject::invokeMethod(qApp, [self, filename] {
            if (!self) return;
            emit self->downloadProgress(filename, 0, -1);
        }, Qt::QueuedConnection);

        // Opening the file triggers transparent hydration on OneDrive
        QFile file(fullPath);
        bool ok = file.open(QIODevice::ReadOnly);
        if (ok)
            file.close();

        QMetaObject::invokeMethod(qApp, [self, filename, ok] {
            if (!self) return;
            emit self->downloadCompleted(filename, ok,
                                   ok ? QString() : WindowsOneDriveBackend::tr("Failed to download backup file"));
        }, Qt::QueuedConnection);
    });
}

// --- Orphaned backup detection and migration ---

void WindowsOneDriveBackend::scanOrphanedBackups()
{
    QString activeRoot = m_backupRoot;
    auto activeType = m_storageType;
    QPointer<WindowsOneDriveBackend> self(this);
    (void)QtConcurrent::run([self, activeRoot, activeType] {
        auto candidates = detectOneDriveCandidates();

        // Find index of active candidate
        int activeIdx = -1;
        for (int i = 0; i < candidates.size(); ++i) {
            if (candidates[i].path == activeRoot) {
                activeIdx = i;
                break;
            }
        }

        // Scan candidates after the active one (lower priority)
        QList<QPair<QString, QtCloudBackup::StorageType>> dirsToScan;
        if (activeIdx >= 0) {
            for (int i = activeIdx + 1; i < candidates.size(); ++i)
                dirsToScan.append({candidates[i].path, candidates[i].type});
        }

        // Always include local fallback
        QString fallback = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                           + QStringLiteral("/Backups");
        bool fallbackAlreadyIncluded = false;
        for (const auto &d : dirsToScan) {
            if (d.first == fallback) {
                fallbackAlreadyIncluded = true;
                break;
            }
        }
        if (!fallbackAlreadyIncluded)
            dirsToScan.append({fallback, QtCloudBackup::StorageType::LocalDirectory});

        static const QRegularExpression re(
            QStringLiteral("^qtcloudbackup_(.+)_(\\d{8}_\\d{6}_\\d{3})_[a-z0-9]{4}\\.bak$"));

        QList<OrphanedBackupInfo> orphans;
        for (const auto &[dirPath, storType] : dirsToScan) {
            const QString relPath = QStringLiteral(QTCLOUDBACKUP_WINDOWS_BACKUP_PATH);
            QString scanDir = relPath.isEmpty() ? dirPath : dirPath + QLatin1Char('/') + relPath;

            QDir d(scanDir);
            if (!d.exists())
                continue;

            QStringList entries = d.entryList({QStringLiteral("qtcloudbackup_*.bak")},
                                              QDir::Files, QDir::Name);
            for (const QString &entry : entries) {
                OrphanedBackupInfo info;
                info.filename = entry;
                info.originStorageType = storType;
                info.originPath = scanDir;

                QString metaPath = scanDir + QLatin1Char('/') + entry.chopped(4)
                                   + QStringLiteral(".meta");
                QFile metaFile(metaPath);
                if (metaFile.open(QIODevice::ReadOnly)) {
                    QJsonObject meta = QJsonDocument::fromJson(metaFile.read(MaxMetaFileSize)).object();
                    info.sourceId = meta[QStringLiteral("sourceId")].toString();
                    info.timestamp = QDateTime::fromString(
                        meta[QStringLiteral("timestamp")].toString(), Qt::ISODateWithMs);
                    info.metadata = meta[QStringLiteral("metadata")].toObject().toVariantMap();
                }

                if (info.sourceId.isEmpty() || !info.timestamp.isValid()) {
                    auto match = re.match(entry);
                    if (match.hasMatch()) {
                        info.sourceId = match.captured(1);
                        info.timestamp = QDateTime::fromString(
                            match.captured(2), QStringLiteral("yyyyMMdd_HHmmss_zzz"));
                        info.timestamp.setTimeZone(QTimeZone::utc());
                    }
                }

                orphans.append(info);
            }
        }

        QMetaObject::invokeMethod(qApp, [self, orphans] {
            if (!self) return;
            emit self->orphanScanCompleted(orphans);
        }, Qt::QueuedConnection);
    });
}

void WindowsOneDriveBackend::migrateOrphanedBackups(const QList<OrphanedBackupInfo> &orphans)
{
    QString destDir = backupDir();
    QPointer<WindowsOneDriveBackend> self(this);
    (void)QtConcurrent::run([self, destDir, orphans] {
        int total = orphans.size();
        int migrated = 0;
        bool allSucceeded = true;

        for (int i = 0; i < total; ++i) {
            const auto &orphan = orphans[i];
            QString srcBak = orphan.originPath + QLatin1Char('/') + orphan.filename;
            QString srcMeta = orphan.originPath + QLatin1Char('/')
                              + orphan.filename.chopped(4) + QStringLiteral(".meta");
            QString destBak = destDir + QLatin1Char('/') + orphan.filename;
            QString destMeta = destDir + QLatin1Char('/')
                               + orphan.filename.chopped(4) + QStringLiteral(".meta");

            // Skip duplicates
            if (QFile::exists(destBak)) {
                ++migrated;
                QMetaObject::invokeMethod(qApp, [self, migrated, total] {
                    if (!self) return;
                    emit self->migrationProgress(migrated, total);
                }, Qt::QueuedConnection);
                continue;
            }

            // Copy .bak
            if (!QFile::copy(srcBak, destBak)) {
                allSucceeded = false;
                continue;
            }

            // Copy .meta (best effort)
            if (QFile::exists(srcMeta))
                QFile::copy(srcMeta, destMeta);

            // Delete originals
            QFile::remove(srcBak);
            QFile::remove(srcMeta);

            ++migrated;
            QMetaObject::invokeMethod(qApp, [self, migrated, total] {
                if (!self) return;
                emit self->migrationProgress(migrated, total);
            }, Qt::QueuedConnection);
        }

        bool success = allSucceeded;
        int count = migrated;
        QString reason = allSucceeded ? QString()
            : WindowsOneDriveBackend::tr("Some files could not be migrated");
        QMetaObject::invokeMethod(qApp, [self, success, count, reason] {
            if (!self) return;
            emit self->migrationCompleted(success, count, reason);
        }, Qt::QueuedConnection);
    });
}
