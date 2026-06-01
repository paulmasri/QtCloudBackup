#pragma once

#include "cloudbackuptypes.h"

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QVariantMap>

// Stage-2 handle returned by detection and passed to `select()`. `accountKey`
// is an in-memory slot name (e.g. "Business2", "Personal", or "" for
// single-instance platforms). It is NOT stable across unlink/re-add cycles —
// OneDrive may re-slot the same account on the lowest free index — so
// consumers must NOT persist `accountKey`. Persist the durable identity
// (`StorageType` + `tenantId` + `email`) and re-resolve at startup via
// `CloudBackupManager::resolveAccount()`.
class AccountId {
    Q_GADGET
    QML_VALUE_TYPE(accountId)
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

// One row of stage-1 detection output. The library returns a `QList` of these
// per `detect()` call — zero, one, or many entries depending on backend.
// Apple always returns exactly one row; Windows returns 0..N; Local always
// returns one. `status` reflects whether the account is selectable
// (`Ready`), needs user remediation (`Unavailable`), or is blocked outside
// the user's control (`Disabled`) — see the `StorageStatus` doc comment.
//
// Field semantics:
//
//   id          Stage-2 handle to pass back to `select()`.
//
//   displayName The most natural per-account label the platform provides.
//               For Windows OneDrive it is the on-disk folder basename as
//               Windows itself names it — i.e. what File Explorer's nav
//               pane shows: typically "OneDrive" for Personal and
//               "OneDrive - <org>" (e.g. "OneDrive - Contoso") for
//               Business. Renderable verbatim where Windows's naming
//               suffices (Business) and overridable where it doesn't
//               (Personal: the bare "OneDrive" doesn't distinguish it in a
//               multi-account picker). Empty for backends with no folder
//               concept (Apple, Local) — the consumer composes the full
//               label from `id.type` instead. The library deliberately
//               does NOT prefix / strip / translate this string: the
//               consumer owns capitalisation, translation, and surrounding
//               chrome.
//
//   email       Account email, where applicable. Empty for Apple (CloudKit
//               not in scope) and Local. Part of the durable identity for
//               OneDrive accounts.
//
//   tenantId    Microsoft Entra tenant GUID for OneDrive Business; empty
//               for Personal / Apple / Local. Part of the durable identity
//               for Business accounts.
//
//   status      Whether this row is selectable (Ready), needs user
//               remediation (Unavailable), or is blocked outside the user's
//               control (Disabled).
//
//   statusDetail Human-readable detail explaining the status; suitable for
//               showing as an inline note or tooltip on disabled rows.
class DetectedAccount {
    Q_GADGET
    QML_VALUE_TYPE(detectedAccount)
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

class OrphanedBackupInfo {
    Q_GADGET
    QML_VALUE_TYPE(orphanedBackupInfo)
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

class BackupInfo {
    Q_GADGET
    QML_VALUE_TYPE(backupInfo)
    Q_PROPERTY(QString sourceId MEMBER sourceId)
    Q_PROPERTY(QDateTime timestamp MEMBER timestamp)
    Q_PROPERTY(QVariantMap metadata MEMBER metadata)
    Q_PROPERTY(QString filename MEMBER filename)
    Q_PROPERTY(QtCloudBackup::DownloadState downloadState MEMBER downloadState)
    Q_PROPERTY(QtCloudBackup::DownloadState metaDownloadState MEMBER metaDownloadState)

public:
    QString sourceId;
    QDateTime timestamp;
    QVariantMap metadata;
    QString filename;
    QtCloudBackup::DownloadState downloadState = QtCloudBackup::DownloadState::Local;
    // Mirrors downloadState but for the .meta sidecar. Single source of
    // truth for whether the metadata map is trustworthy:
    //
    //   Local        .meta is on disk and was read+parsed successfully.
    //                Consumers can use `metadata`, `sourceId` and
    //                `timestamp` directly. Retention logic considers this
    //                row a candidate for prune.
    //   Missing      .meta is not present on disk. Filename regex still
    //                yields sourceId/timestamp. Excluded from prune.
    //   CloudOnly    .meta is a cloud placeholder (Apple FoD / OneDrive
    //                FoD). Scanner skipped open() to avoid blocking on
    //                hydration. Excluded from prune.
    //   Downloading  .meta is partially hydrated (Apple only). Excluded
    //                from prune.
    //   Error        .meta exists but couldn't be opened (permissions, IO)
    //                or its JSON failed to parse. Excluded from prune.
    //
    // Platform asymmetry: Apple distinguishes all five values; Windows
    // collapses Downloading into CloudOnly (no available attribute); Local
    // backend only produces Local / Missing / Error.
    QtCloudBackup::DownloadState metaDownloadState = QtCloudBackup::DownloadState::Local;
};

