#pragma once

#include "backupinfo.h"
#include "retentionpolicy.h"

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
    Q_PROPERTY(QtCloudBackup::RetentionPolicy retentionPolicy READ retentionPolicy WRITE setRetentionPolicy NOTIFY retentionPolicyChanged)
    Q_PROPERTY(bool hasOrphanedBackups READ hasOrphanedBackups NOTIFY hasOrphanedBackupsChanged)

public:
    explicit CloudBackupManager(QObject *parent = nullptr);
    ~CloudBackupManager() override;

    QtCloudBackup::StorageStatus storageStatus() const;
    QString statusDetail() const;
    QtCloudBackup::StorageType storageType() const;
    bool backupInProgress() const;
    QtCloudBackup::RetentionPolicy retentionPolicy() const;
    void setRetentionPolicy(const QtCloudBackup::RetentionPolicy &policy);
    bool hasOrphanedBackups() const;

    Q_INVOKABLE void createBackup(const QString &sourceId, const QByteArray &data, const QVariantMap &metadata = {});
    Q_INVOKABLE void listBackups();
    Q_INVOKABLE void requestDownload(const QString &filename);
    Q_INVOKABLE void restoreBackup(const QString &filename);
    Q_INVOKABLE void deleteBackup(const QString &filename);
    // Stage 1: enumerate candidate accounts. Result delivered via
    // accountsDetected. See the backend interface for full semantics.
    Q_INVOKABLE void detect();
    // Stage 2: activate the chosen account. `accountKey` comes from the
    // DetectedAccount.id.accountKey of the entry the user picked (or from
    // resolveAccount()). Pass an empty string for single-instance platforms
    // (Apple, Local).
    Q_INVOKABLE void select(QtCloudBackup::StorageType type, const QString &accountKey);
    // Resolves a persisted durable identity (StorageType + tenantId + email)
    // to the current in-memory AccountId, against the most recent detect()
    // result. Returns an empty map if no matching account is currently
    // detected; otherwise { "type": int, "accountKey": string } suitable
    // for passing to select().
    Q_INVOKABLE QVariantMap resolveAccount(QtCloudBackup::StorageType type,
                                           const QString &tenantId,
                                           const QString &email) const;
    Q_INVOKABLE void prune(const QString &sourceId);
    Q_INVOKABLE void checkForOrphanedBackups();
    Q_INVOKABLE void migrateOrphanedBackups();
    // Factory for constructing a RetentionPolicy from QML. Gadget value types
    // can't be `new`'d in JS and sub-property assignment goes via a temporary,
    // so consumers build a full policy and assign it:
    //   manager.retentionPolicy = manager.makeRetentionPolicy(3, 7, 4, 12, 0)
    Q_INVOKABLE QtCloudBackup::RetentionPolicy makeRetentionPolicy(
        int keepLast = 0, int keepDaily = 0, int keepWeekly = 0,
        int keepMonthly = 0, int keepYearly = 0) const;

signals:
    void storageStatusChanged();
    void statusDetailChanged();
    void storageTypeChanged();
    void backupInProgressChanged();
    void retentionPolicyChanged();
    void hasOrphanedBackupsChanged();

    void accountsDetected(const QList<DetectedAccount> &accounts);
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
    QtCloudBackup::RetentionPolicy m_retentionPolicy = { .keepLast = 3 };
    QString m_currentBackupSourceId;
    QDateTime m_currentBackupTimestamp;
    QString m_pendingRestoreFilename; // set when auto-downloading for restore
    QList<OrphanedBackupInfo> m_orphanedBackups;
};
