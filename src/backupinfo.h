#ifndef QTCLOUDBACKUP_BACKUPINFO_H
#define QTCLOUDBACKUP_BACKUPINFO_H

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

namespace QtCloudBackup {
Q_NAMESPACE
QML_NAMED_ELEMENT(CloudBackup)

enum class StorageStatus {
    Unknown,
    Ready,
    Unavailable,
    Disabled,
    LocalFallback
};
Q_ENUM_NS(StorageStatus)

enum class DownloadState {
    Local,
    CloudOnly,
    Downloading,
    Error
};
Q_ENUM_NS(DownloadState)

enum class StorageType {
    None,
    ICloud,
    OneDrivePersonal,
    OneDriveCommercial,
    LocalDirectory
};
Q_ENUM_NS(StorageType)

enum class RestoreStatus {
    RestoreDownloading,
    RestoreInProgress,
    RestoreSucceeded,
    RestoreFailed
};
Q_ENUM_NS(RestoreStatus)

enum class DownloadStatus {
    DownloadInProgress,
    DownloadSucceeded,
    DownloadFailed
};
Q_ENUM_NS(DownloadStatus)

enum class MigrationStatus {
    MigrationInProgress,
    MigrationSucceeded,
    MigrationFailed
};
Q_ENUM_NS(MigrationStatus)

} // namespace QtCloudBackup

class OrphanedBackupInfo {
    Q_GADGET
    Q_PROPERTY(QString sourceId MEMBER sourceId)
    Q_PROPERTY(QDateTime timestamp MEMBER timestamp)
    Q_PROPERTY(QVariantMap metadata MEMBER metadata)
    Q_PROPERTY(QString filename MEMBER filename)
    Q_PROPERTY(QtCloudBackup::StorageType originStorageType MEMBER originStorageType)
    Q_PROPERTY(QString originPath MEMBER originPath)

public:
    QString sourceId;
    QDateTime timestamp;
    QVariantMap metadata;
    QString filename;
    QtCloudBackup::StorageType originStorageType = QtCloudBackup::StorageType::None;
    QString originPath;
};

Q_DECLARE_METATYPE(OrphanedBackupInfo)

class BackupInfo {
    Q_GADGET
    Q_PROPERTY(QString sourceId MEMBER sourceId)
    Q_PROPERTY(QDateTime timestamp MEMBER timestamp)
    Q_PROPERTY(QVariantMap metadata MEMBER metadata)
    Q_PROPERTY(QString filename MEMBER filename)
    Q_PROPERTY(QtCloudBackup::DownloadState downloadState MEMBER downloadState)

public:
    QString sourceId;
    QDateTime timestamp;
    QVariantMap metadata;
    QString filename;
    QtCloudBackup::DownloadState downloadState = QtCloudBackup::DownloadState::Local;
};

Q_DECLARE_METATYPE(BackupInfo)

#endif // QTCLOUDBACKUP_BACKUPINFO_H
