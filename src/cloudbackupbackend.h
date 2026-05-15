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

    // Stage 1: enumerate candidate accounts. No filesystem side effects — no
    // writes, no folder creation. Async; result delivered via
    // `accountsDetected`. Implementations cache the result internally for use
    // by `select()`, `resolveAccount()`, and `scanOrphanedBackups()`.
    //
    // May also emit `statusChanged` as a side effect: if a previously-
    // selected account is no longer `Ready` in the new detection result,
    // the active selection is invalidated (platform machinery torn down,
    // `storageStatus()` reflects the new reality). This keeps the backend
    // honest when external events (user signs out, IT policy changes, etc.)
    // make the active target unusable. The `statusChanged` signal is
    // emitted BEFORE `accountsDetected` so consumers see "storage dropped"
    // before "here are the current options".
    //
    // May be triggered internally by platform notifications (e.g. iCloud's
    // NSUbiquityIdentityDidChangeNotification) — consumers therefore receive
    // `accountsDetected` and `statusChanged` events at any time, not only in
    // direct response to their own `detect()` / `select()` calls.
    virtual void detect() = 0;

    // Stage 2: activate the chosen account. Performs `mkpath` of the backup
    // subdirectory, brings up platform-specific machinery (e.g. iCloud's
    // NSMetadataQuery), and emits `statusChanged` with the resulting
    // `StorageStatus`. Reentrant — calling again with a different id switches
    // the active target without tearing down the backend object. Switching
    // does NOT migrate existing backups; that remains an explicit
    // `OrphanedBackupInfo` flow.
    //
    // A `Ready` outcome is not permanent. External events (user signs out,
    // policy change, network identity loss) can invalidate the selection
    // later — see `detect()` and the `statusChanged` signal.
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
    // Fires whenever the active target's status changes — from `select()`
    // completion (success or failure), from `detect()`-driven invalidation
    // (a previously-selected account is no longer Ready), and from
    // platform-event-driven re-detection. Consumers must connect once at
    // startup and tolerate events at any time, not just after a `select()`.
    // In-flight file operations may complete with errors after an
    // invalidating `statusChanged` — surface those as errors, not panics.
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
