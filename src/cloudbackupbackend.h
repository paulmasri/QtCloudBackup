#ifndef QTCLOUDBACKUP_CLOUDBACKUPBACKEND_H
#define QTCLOUDBACKUP_CLOUDBACKUPBACKEND_H

#include "backupinfo.h"

#include <QJsonObject>
#include <QList>
#include <QObject>

class CloudBackupBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual ~CloudBackupBackend() = default;

    virtual void initialise() = 0;
    virtual QtCloudBackup::StorageStatus storageStatus() const = 0;
    virtual QString statusDetail() const = 0;
    virtual QtCloudBackup::StorageType storageType() const = 0;

    virtual void writeBackup(const QString &filename, const QByteArray &data, const QJsonObject &meta) = 0;
    virtual void readBackup(const QString &filename) = 0;
    virtual void deleteBackup(const QString &filename) = 0;
    virtual void scanBackups() = 0;
    virtual void triggerDownload(const QString &filename) = 0;
    virtual void scanOrphanedBackups() = 0;
    virtual void migrateOrphanedBackups(const QList<OrphanedBackupInfo> &orphans) = 0;

signals:
    void statusChanged(QtCloudBackup::StorageStatus status, const QString &detail);
    void writeSucceeded(const QString &filename);
    void writeFailed(const QString &reason);
    void readSucceeded(const QString &filename, const QByteArray &data, const QJsonObject &meta);
    void readFailed(const QString &filename, const QString &reason);
    void deleteCompleted(const QString &filename, bool success, const QString &reason);
    void scanCompleted(const QList<BackupInfo> &backups);
    void downloadProgress(const QString &filename, qint64 received, qint64 total);
    void downloadCompleted(const QString &filename, bool success, const QString &reason);
    void remoteChangeDetected(const QString &sourceId);
    void orphanScanCompleted(const QList<OrphanedBackupInfo> &orphans);
    void migrationProgress(int completed, int total);
    void migrationCompleted(bool success, int migratedCount, const QString &reason);
};

#endif // QTCLOUDBACKUP_CLOUDBACKUPBACKEND_H
