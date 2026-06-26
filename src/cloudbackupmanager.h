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
    Q_PROPERTY(bool backupIoBusy READ backupIoBusy NOTIFY backupIoBusyChanged)
    Q_PROPERTY(QtCloudBackup::RetentionPolicy retentionPolicy READ retentionPolicy WRITE setRetentionPolicy NOTIFY retentionPolicyChanged)
    Q_PROPERTY(bool hasOrphanedBackups READ hasOrphanedBackups NOTIFY hasOrphanedBackupsChanged)

public:
    explicit CloudBackupManager(QObject *parent = nullptr);
    ~CloudBackupManager() override;

    QtCloudBackup::StorageStatus storageStatus() const;
    QString statusDetail() const;
    QtCloudBackup::StorageType storageType() const;
    bool backupIoBusy() const;
    QtCloudBackup::RetentionPolicy retentionPolicy() const;
    void setRetentionPolicy(const QtCloudBackup::RetentionPolicy &policy);
    bool hasOrphanedBackups() const;

    Q_INVOKABLE void createBackup(const QString &sourceId, const QByteArray &data, const QVariantMap &metadata = {});
    Q_INVOKABLE void listBackups();
    // Lightweight alternative to listBackups(): emits backupDigestsListed with
    // sourceId/timestamp/filename derived from filenames alone — no .meta
    // sidecar reads and no cloud-placeholder hydration. For cheap, repeatable
    // queries (does any backup exist? newest timestamp? which sourceIds?).
    // Anything needing metadata or download state must use listBackups().
    Q_INVOKABLE void listBackupDigests();
    Q_INVOKABLE void requestDownload(const QString &filename);
    Q_INVOKABLE void readBackup(const QString &filename);
    Q_INVOKABLE void deleteBackup(const QString &filename);
    // Stage 1: enumerate candidate accounts. Result delivered via
    // accountsDetected. See the backend interface for full semantics.
    Q_INVOKABLE void detect();
    // Stage 2: activate the chosen account. `id` comes from the
    // DetectedAccount.id of the entry the user picked, or from
    // resolveAccount().
    Q_INVOKABLE void select(const AccountId &id);
    // Resolves a persisted `DurableAccountIdentity` (StorageType + tenantId
    // + email) to the current in-memory AccountId, against the most recent
    // detect() result. Returns an AccountId whose `type == StorageType::None`
    // if no matching account is currently detected; otherwise the resolved
    // AccountId, suitable for passing straight to select().
    Q_INVOKABLE AccountId resolveAccount(const DurableAccountIdentity &identity) const;
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
    // Factory for constructing a DurableAccountIdentity from QML. Same root
    // cause as makeRetentionPolicy: gadget value types can't be `new`'d in
    // JS, and a JS object literal won't auto-convert to a Q_GADGET argument
    // at a QML→C++ call boundary. Consumers that need to construct the
    // identity from primitives (e.g. rehydrating from persisted settings)
    // build a full value here and pass it to resolveAccount:
    //   manager.resolveAccount(
    //       manager.makeDurableAccountIdentity(t, tenantId, email))
    // C++ consumers can use aggregate initialisation directly.
    Q_INVOKABLE DurableAccountIdentity makeDurableAccountIdentity(
        QtCloudBackup::StorageType type = QtCloudBackup::StorageType::None,
        const QString &tenantId = {},
        const QString &email = {}) const;
    // Factory for constructing an AccountId from QML. Same root cause as
    // makeDurableAccountIdentity / makeRetentionPolicy: gadgets can't
    // round-trip through JS object form. The common case is a picker
    // backed by a ListModel — append() serialises gadget-typed roles, so
    // `model.id` read back from a row is a plain object, not an AccountId.
    // Reconstruct on the way into select():
    //   backupManager.select(
    //       backupManager.makeAccountId(model.type, model.accountKey))
    // C++ consumers can use aggregate initialisation directly.
    Q_INVOKABLE AccountId makeAccountId(
        QtCloudBackup::StorageType type = QtCloudBackup::StorageType::None,
        const QString &accountKey = {}) const;

signals:
    void storageStatusChanged();
    void statusDetailChanged();
    void storageTypeChanged();
    void backupIoBusyChanged();
    void retentionPolicyChanged();
    void hasOrphanedBackupsChanged();

    void accountsDetected(const QList<DetectedAccount> &accounts);
    void statusChanged(QtCloudBackup::StorageStatus status, const QString &detail);
    void backupSucceeded(const QString &filename, const QDateTime &timestamp);
    void backupFailed(int error, const QString &message);
    void backupsListed(const QList<BackupInfo> &backups);
    void backupsListFailed(int error, const QString &message);
    void backupDigestsListed(const QList<BackupDigest> &digests);
    void downloadProgressChanged(const QString &filename, qint64 bytesReceived, qint64 bytesTotal);
    void downloadUpdated(const QString &filename, QtCloudBackup::DownloadStatus status,
                         int error, const QString &message);
    void backupReadStarted(const QString &filename);
    void backupReadCompleted(const QString &filename, const QByteArray &data,
                             const QVariantMap &metadata);
    void backupReadFailed(const QString &filename, int error, const QString &message);
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
    bool m_backupIoBusy = false;
    QtCloudBackup::RetentionPolicy m_retentionPolicy = { .keepLast = 3 };
    QString m_currentBackupSourceId;
    QDateTime m_currentBackupTimestamp;
    QString m_pendingReadFilename; // set when auto-downloading prior to a read retry
    QList<OrphanedBackupInfo> m_orphanedBackups;
};
