#pragma once

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

namespace QtCloudBackup {
Q_NAMESPACE
QML_NAMED_ELEMENT(QtCloudBackup)

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
//   LocalActive   Local backend is the active selection. Only ever assigned
//                 when the consumer explicitly selects `LocalDirectory`;
//                 the library never assigns this as an automatic last
//                 resort, hence "active" rather than "fallback".
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
    LocalActive
};
Q_ENUM_NS(StorageStatus)

enum class DownloadState {
    Local,
    CloudOnly,
    Downloading,
    Error,
    Missing      // file does not exist on disk (only meaningful for .meta;
                 // .bak entries are surfaced via directory listing so are
                 // never reported Missing by the scanner)
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

// Persistable account identity — the durable counterpart to `AccountId`.
//
// `AccountId` is the in-memory stage-2 handle returned by `detect()`. Its
// `accountKey` is a platform slot name (OneDrive: "Personal", "Business1",
// ...) that is NOT stable across unlink/re-add cycles. Persisting it can
// silently re-point at the wrong account.
//
// `DurableAccountIdentity` is the value the consumer should persist:
//   StorageType   the backend (ICloud / OneDrivePersonal / OneDriveCommercial
//                 / LocalDirectory).
//   tenantId      Microsoft Entra tenant GUID for OneDrive Business; empty
//                 for Personal / Apple / Local.
//   email         Account email; empty for Apple and Local.
//
// At startup the consumer passes this to `CloudBackupManager::resolveAccount`
// to recover the current `AccountId` from the live `detect()` result.
class DurableAccountIdentity {
    Q_GADGET
    QML_VALUE_TYPE(durableAccountIdentity)
    Q_PROPERTY(QtCloudBackup::StorageType type MEMBER type FINAL)
    Q_PROPERTY(QString tenantId MEMBER tenantId FINAL)
    Q_PROPERTY(QString email MEMBER email FINAL)

public:
    QtCloudBackup::StorageType type = QtCloudBackup::StorageType::None;
    QString tenantId;
    QString email;

    bool isEmpty() const
    {
        return type == QtCloudBackup::StorageType::None
            && tenantId.isEmpty() && email.isEmpty();
    }

    friend bool operator==(const DurableAccountIdentity &a,
                           const DurableAccountIdentity &b)
    {
        return a.type == b.type && a.tenantId == b.tenantId && a.email == b.email;
    }
    friend bool operator!=(const DurableAccountIdentity &a,
                           const DurableAccountIdentity &b)
    {
        return !(a == b);
    }
};
