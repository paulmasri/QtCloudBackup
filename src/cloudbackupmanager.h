#pragma once

#include "backupinfo.h"

#include <QObject>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <memory>

class CloudBackupBackend;

class CloudBackupManager : public QObject {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QtCloudBackup::StorageStatus storageStatus READ storageStatus NOTIFY storageStatusChanged)
    Q_PROPERTY(QString statusDetail READ statusDetail NOTIFY statusDetailChanged)
    Q_PROPERTY(QtCloudBackup::StorageType storageType READ storageType NOTIFY storageTypeChanged)
    Q_PROPERTY(bool backupInProgress READ backupInProgress NOTIFY backupInProgressChanged)
    Q_PROPERTY(int maxBackupsPerSource READ maxBackupsPerSource WRITE setMaxBackupsPerSource NOTIFY maxBackupsPerSourceChanged)
    Q_PROPERTY(bool hasOrphanedBackups READ hasOrphanedBackups NOTIFY hasOrphanedBackupsChanged)

public:
    explicit CloudBackupManager(QObject *parent = nullptr);
    ~CloudBackupManager() override;

    QtCloudBackup::StorageStatus storageStatus() const;
    QString statusDetail() const;
    QtCloudBackup::StorageType storageType() const;
    bool backupInProgress() const;
    int maxBackupsPerSource() const;
    void setMaxBackupsPerSource(int n);
    bool hasOrphanedBackups() const;

    Q_INVOKABLE void createBackup(const QString &sourceId, const QByteArray &data, const QVariantMap &metadata = {});
    Q_INVOKABLE void listBackups();
    Q_INVOKABLE void requestDownload(const QString &filename);
    Q_INVOKABLE void restoreBackup(const QString &filename);
    Q_INVOKABLE void deleteBackup(const QString &filename);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void checkForOrphanedBackups();
    Q_INVOKABLE void migrateOrphanedBackups();

signals:
    void storageStatusChanged();
    void statusDetailChanged();
    void storageTypeChanged();
    void backupInProgressChanged();
    void maxBackupsPerSourceChanged();
    void hasOrphanedBackupsChanged();

    void statusChanged(QtCloudBackup::StorageStatus status, const QString &detail);
    void backupSucceeded(const QString &filename, const QDateTime &timestamp);
    void backupFailed(int error, const QString &message);
    void backupsListed(const QList<BackupInfo> &backups);
    void backupsListFailed(int error, const QString &message);
    void downloadProgressChanged(const QString &filename, qint64 bytesReceived, qint64 bytesTotal);
    void downloadUpdated(const QString &filename, QtCloudBackup::DownloadStatus status,
                         int error, const QString &message);
    void restoreUpdated(const QString &filename, QtCloudBackup::RestoreStatus status,
                        const QByteArray &data, const QVariantMap &metadata,
                        int error, const QString &message);
    void deleteSucceeded(const QString &filename);
    void deleteFailed(const QString &filename, int error, const QString &message);
    void remoteBackupDetected(const QString &sourceId);
    void orphanedBackupsDetected(const QVariantList &orphans);
    void migrationUpdated(QtCloudBackup::MigrationStatus status, int migratedCount,
                          int totalCount, int error, const QString &message);

private:
    void pruneBackups(const QString &sourceId);

    void handleReadFailed(const QString &filename, int error, const QString &message);

    std::unique_ptr<CloudBackupBackend> m_backend;
    bool m_backupInProgress = false;
    int m_maxBackupsPerSource = 3;
    QString m_currentBackupSourceId;
    QDateTime m_currentBackupTimestamp;
    QString m_pendingRestoreFilename; // set when auto-downloading for restore
    QList<OrphanedBackupInfo> m_orphanedBackups;
};
