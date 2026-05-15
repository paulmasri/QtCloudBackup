#pragma once

#include "backupinfo.h"

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <optional>

class CloudBackupBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual ~CloudBackupBackend() = default;

    // Transitional during issue #4 work: `initialise()` will be removed once
    // every backend implements `detect()`/`select()` (phase 6). For now both
    // paths coexist so the build stays green between phases.
    virtual void initialise() = 0;

    // Stage 1: enumerate candidate accounts. Side-effect-free — no filesystem
    // writes, no folder creation. Async; result delivered via
    // `accountsDetected`. Implementations cache the result internally for use
    // by `select()`, `resolveAccount()`, and `scanOrphanedBackups()`.
    virtual void detect() = 0;

    // Stage 2: activate the chosen account. Performs `mkpath` of the backup
    // subdirectory, brings up platform-specific machinery (e.g. iCloud's
    // NSMetadataQuery), and emits `statusChanged` with the resulting
    // `StorageStatus`. Reentrant — calling again with a different id switches
    // the active target without tearing down the backend object. Switching
    // does NOT migrate existing backups; that remains an explicit
    // `OrphanedBackupInfo` flow.
    virtual void select(const AccountId &id) = 0;

    // Synchronous resolver over the cached `detect()` result. Returns the
    // current `AccountId` for an account previously persisted by its durable
    // identity (`StorageType` + optional `tenantId` for Business + optional
    // `email`), or `nullopt` if no matching account is currently detected.
    // Consumers persist the durable identity (NOT `accountKey`) and call this
    // at startup to recover the slot.
    virtual std::optional<AccountId> resolveAccount(
        QtCloudBackup::StorageType type,
        const QString &tenantId,
        const QString &email) const = 0;

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
    void accountsDetected(const QList<DetectedAccount> &accounts);
    void statusChanged(QtCloudBackup::StorageStatus status, const QString &detail);
    void writeCompleted(const QString &filename, int error, const QString &message);
    void readCompleted(const QString &filename, const QByteArray &data, const QJsonObject &meta,
                       int error, const QString &message);
    void deleteCompleted(const QString &filename, int error, const QString &message);
    void scanCompleted(const QList<BackupInfo> &backups);
    void downloadProgress(const QString &filename, qint64 received, qint64 total);
    void downloadCompleted(const QString &filename, int error, const QString &message);
    void remoteChangeDetected(const QString &sourceId);
    void orphanScanCompleted(const QList<OrphanedBackupInfo> &orphans);
    void migrationProgress(int completed, int total);
    void migrationCompleted(int migratedCount, int error, const QString &message);
};
