#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

namespace QtCloudBackup {
Q_NAMESPACE
QML_NAMED_ELEMENT(CloudBackup)

// StorageStatus values map to distinct user remediation flows rather than to
// distinct technical causes — if two states would lead the user to take the
// same next action, they collapse into one value.
//
//   Unknown       Pre-initialisation. No detection has been performed yet.
//
//   Ready         Storage is available and writable.
//                 At backend level (post-`select()`): the active storage
//                 target is available and writable.
//                 Per `DetectedAccount` during stage 1 (detection): the
//                 account is *selectable* — nothing currently rules it out.
//                 A `Ready` `DetectedAccount` is a candidate to pass to
//                 `select()`; it does not imply the account is in use.
//
//   Unavailable   Storage is not configured. User remediation is possible
//                 (install the client, sign in, enable the service). Examples:
//                 Apple — no ubiquity identity token (signed out / iCloud
//                 Drive off); Windows — partial registry residue, missing/
//                 unwritable OneDrive folder; Local — backup directory not
//                 writable. Surface a message that invites the user to
//                 complete setup.
//
//   Disabled      Storage is configured-but-blocked by something outside the
//                 user's control (IT policy, missing entitlements, unlicensed
//                 account, tenant restriction). No user action will help;
//                 the message must NOT invite a sign-in attempt. Examples:
//                 Apple — container URL unresolvable (most commonly missing
//                 entitlements/provisioning); Windows — Group Policy block
//                 (`DisableFileSyncNGSC` device-level, `DisablePersonalSync`
//                 personal, `AllowTenantList`/`BlockTenantList` business).
//
//   LocalFallback Storage backend is using a local directory in lieu of cloud
//                 sync. Only meaningful as an explicitly-selected target; the
//                 library never assigns this as an automatic last resort.
//
// Known limitation (Apple): MDM-restricted devices are not distinguished from
// signed-out / iCloud-Drive-off — both collapse into `Unavailable`. The
// CloudKit API (`CKContainer.accountStatus`) could split these out but would
// require every consumer to add the CloudKit entitlement and enable CloudKit
// on their App ID. Disproportionate cost for a niche case; deferred.
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

enum class BackupError {
    NoError,
    InvalidArgument,
    IOError,
    MetadataIOError,
    CoordinationFailed,
    FileNotLocal,
    DownloadError,
    DownloadTimeout,
    MigrationPartial,
    UnknownError
};
Q_ENUM_NS(BackupError)

} // namespace QtCloudBackup

// Stage-2 handle returned by detection and passed to `select()`. `accountKey`
// is an in-memory slot name (e.g. "Business2", "Personal", or "" for
// single-instance platforms). It is NOT stable across unlink/re-add cycles —
// OneDrive may re-slot the same account on the lowest free index — so
// consumers must NOT persist `accountKey`. Persist the durable identity
// (`StorageType` + `tenantId` + `email`) and re-resolve at startup via
// `CloudBackupManager::resolveAccount()`.
class AccountId {
    Q_GADGET
    Q_PROPERTY(QtCloudBackup::StorageType type MEMBER type)
    Q_PROPERTY(QString accountKey MEMBER accountKey)

public:
    QtCloudBackup::StorageType type = QtCloudBackup::StorageType::None;
    QString accountKey;

    bool operator==(const AccountId &other) const
    {
        return type == other.type && accountKey == other.accountKey;
    }
    bool operator!=(const AccountId &other) const { return !(*this == other); }
};

Q_DECLARE_METATYPE(AccountId)

// One row of stage-1 detection output. The library returns a `QList` of these
// per `detect()` call — zero, one, or many entries depending on backend.
// Apple always returns exactly one row; Windows returns 0..N; Local always
// returns one. `status` reflects whether the account is selectable
// (`Ready`), needs user remediation (`Unavailable`), or is blocked outside
// the user's control (`Disabled`) — see the `StorageStatus` doc comment.
class DetectedAccount {
    Q_GADGET
    Q_PROPERTY(AccountId id MEMBER id)
    Q_PROPERTY(QString displayName MEMBER displayName)
    Q_PROPERTY(QString email MEMBER email)
    Q_PROPERTY(QString tenantId MEMBER tenantId)
    Q_PROPERTY(QtCloudBackup::StorageStatus status MEMBER status)
    Q_PROPERTY(QString statusDetail MEMBER statusDetail)

public:
    AccountId id;
    QString displayName;
    QString email;
    QString tenantId;
    QtCloudBackup::StorageStatus status = QtCloudBackup::StorageStatus::Unknown;
    QString statusDetail;
};

Q_DECLARE_METATYPE(DetectedAccount)

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
    Q_PROPERTY(bool metadataAvailable MEMBER metadataAvailable)

public:
    QString sourceId;
    QDateTime timestamp;
    QVariantMap metadata;
    QString filename;
    QtCloudBackup::DownloadState downloadState = QtCloudBackup::DownloadState::Local;
    // false when the .meta sidecar is missing — may be syncing, evicted, or
    // never written. The filename still yields sourceId and timestamp.
    bool metadataAvailable = true;
};

Q_DECLARE_METATYPE(BackupInfo)
