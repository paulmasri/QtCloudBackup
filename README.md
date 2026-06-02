# QtCloudBackup

A Qt 6.8 library for writing backup files to cloud-synced local storage (iCloud on Apple platforms, OneDrive on Windows).

## What it does

QtCloudBackup provides a single C++ class (`CloudBackupManager`) registered as a `QML_ELEMENT` that:

- Enumerates available storage accounts (cloud or local) without side effects
- Activates a consumer-chosen account, including creating the backup subdirectory
- Writes opaque `QByteArray` payloads as timestamped backup files
- Lists available backups with metadata and download state
- Triggers hydration (download) of cloud-only files
- Restores a backup by returning its `QByteArray` payload, auto-downloading if cloud-only
- Deletes backups
- Prunes old backups per source ID
- Emits status signals for every operation stage

## What it does NOT do

- **Scheduling** — the consuming app decides when to call `createBackup()`
- **Serialisation / encoding / encryption** — the app prepares the `QByteArray`; the library stores it as-is
- **Sync or conflict resolution** — the library writes files; the OS syncs them
- **User authentication** — the app provides a `sourceId` string; the library doesn't interpret it

## Platform support

| Platform | Backend | Storage location |
|----------|---------|-----------------|
| iOS | iCloud Drive (ubiquity container) | App's iCloud container, invisible to Files app |
| macOS | iCloud Drive (ubiquity container) | `~/Library/Mobile Documents/<container>/Backups/` |
| Windows | OneDrive (Personal and/or Business; 0..N accounts) | OneDrive sync folder, configurable subfolder |
| Other | Local directory | `QStandardPaths::AppLocalDataLocation/Backups/` |

Each build compiles exactly one platform backend. The library enumerates whatever accounts that backend can see and hands the list to the consumer to choose from — the consumer's app picks (or auto-picks against a persisted preference); the library never invents a hierarchy or falls back silently.

**Picker shape differs by platform** — consumers writing a picker UI should not assume uniformity:
- **Apple**: always exactly one row (the device's single iCloud account, with status reflecting its state).
- **Windows**: zero, one, or many rows (a device may have 0 OneDrive accounts, or 1 Personal + 0..9 Business).
- **Local**: exactly one row.

## Requirements

- Qt 6.8+
- CMake 3.21+
- Xcode (for Apple platforms — code signing required for iCloud access)
- MSVC (for Windows)

## Integration

Add as a Git submodule:

```bash
git submodule add <repo-url> lib/QtCloudBackup
```

In your `CMakeLists.txt`:

```cmake
# Configure before add_subdirectory
set(QTCLOUDBACKUP_APP_NAME "YourApp" CACHE STRING "")
set(QTCLOUDBACKUP_ICLOUD_CONTAINER "iCloud.com.yourcompany.YourApp" CACHE STRING "")
set(QTCLOUDBACKUP_WINDOWS_BACKUP_PATH "YourApp/Backups" CACHE STRING "")

add_subdirectory(lib/QtCloudBackup)

target_link_libraries(YourApp PRIVATE
    QtCloudBackup
    QtCloudBackupplugin
)
```

Import the QML module in your `.qml` files:

```qml
import QtCloudBackup
```

If using `engine.loadFromModule()` and it fails with the Xcode generator, use `engine.load(QUrl("qrc:/..."))` instead.

## Apple setup

### Entitlements

An entitlements template is provided at `platform/apple/QtCloudBackup.entitlements.template`. Copy it into your project and replace `$(ICLOUD_CONTAINER_IDENTIFIER)` with your container ID, or use CMake's `configure_file()`:

```cmake
if(APPLE)
    set(QTCLOUDBACKUP_ICLOUD_CONTAINER "iCloud.com.yourcompany.YourApp")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/YourApp.entitlements.in"
        "${CMAKE_CURRENT_BINARY_DIR}/YourApp.entitlements"
        @ONLY
    )
    set_target_properties(YourApp PROPERTIES
        MACOSX_BUNDLE TRUE
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
        XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS
            "${CMAKE_CURRENT_BINARY_DIR}/YourApp.entitlements"
    )
endif()
```

### Apple Developer portal (one-time)

1. Go to Certificates, Identifiers & Profiles.
2. Under **Identifiers**, create an **iCloud Container** with identifier `iCloud.com.yourcompany.YourApp`.
3. Under **Identifiers**, edit (or create) the **App ID** for your app.
4. Enable the **iCloud** capability (CloudKit support is auto-selected — this is fine) and click **Configure** to assign your iCloud Container.
5. Regenerate the provisioning profile.

Do **not** add `NSUbiquitousContainers` to your Info.plist — omitting it keeps the iCloud container hidden from the Files app.

### macOS development builds

macOS development builds must be code-signed to access iCloud. Use the Xcode CMake generator (`-G Xcode`) or sign manually.

## Windows setup

No special setup required. The library enumerates OneDrive accounts from the registry under `HKCU\Software\Microsoft\OneDrive\Accounts\*`, recognising `Personal` and `Business1`..`Business9` subkeys (gap-tolerant — accounts removed and re-added can leave gaps). For each subkey, it reads `UserEmail`, `UserFolder`, and (for Business) `ConfiguredTenantId`; all three must be present for the account to be considered configured.

Set `QTCLOUDBACKUP_WINDOWS_BACKUP_PATH` to a relative path within OneDrive (e.g. `"YourApp/Backups"`) to avoid polluting the root. **Do not use `"Personal Vault"`** — that name collides with OneDrive's locked virtual folder and writes will fail when the vault is locked. The library's CMake enforces this at configure time with a `FATAL_ERROR`. Choose a unique, app-namespaced subfolder name.

### Group Policy

The library honours these Group Policy settings. Note the **two different policy roots** — `HKLM\SOFTWARE\Policies\Microsoft\Windows\OneDrive\` (with `Windows\`) vs `HKLM\SOFTWARE\Policies\Microsoft\OneDrive\` (no `Windows\`) — easy to confuse:

| Policy value | Root | Scope | Effect |
|---|---|---|---|
| `DisableFileSyncNGSC` (DWORD) | `HKLM\SOFTWARE\Policies\Microsoft\`**`Windows\`**`OneDrive\` | Device-level | If `1`, OneDrive sync is disabled across the device. Library reports a backend-level `Disabled`; no accounts are enumerated. |
| `DisablePersonalSync` (DWORD) | `HKCU` or `HKLM\SOFTWARE\Policies\Microsoft\OneDrive\` | Personal accounts only | If `1` in either hive, any detected `Personal` account is reported as `Disabled`. |
| `AllowTenantList` (values) | `HKLM\SOFTWARE\Policies\Microsoft\OneDrive\AllowTenantList` | Business accounts | When non-empty, only the listed tenant GUIDs may sign in. Business accounts whose `ConfiguredTenantId` is **not** in the list are reported as `Disabled`. Takes precedence over `BlockTenantList`. |
| `BlockTenantList` (values) | `HKLM\SOFTWARE\Policies\Microsoft\OneDrive\BlockTenantList` | Business accounts | Business accounts whose `ConfiguredTenantId` is in the list are reported as `Disabled`. |

`AllowTenantList` / `BlockTenantList` are registry **keys** whose **values** name tenant GUIDs (not subkeys). They do not apply to Personal accounts (MSAs have no tenant).

Other Group Policy values (`DisableFileSync` legacy, `DisableNewAccountDetection`) exist but are not consulted — they're not useful for the "is this account usable?" question.

## API quick reference

### Properties (QML-bindable)

| Property | Type | Description |
|----------|------|-------------|
| `storageStatus` | `StorageStatus` | Current storage availability |
| `statusDetail` | `QString` | Human-readable status detail |
| `storageType` | `StorageType` | Which backend is active |
| `backupInProgress` | `bool` | Whether a create/restore operation is running |
| `retentionPolicy` | `RetentionPolicy` | Configurable union-of-keeps retention (default: `{ keepLast = 3 }`). See [How pruning works](#how-pruning-works). |
| `hasOrphanedBackups` | `bool` | Whether orphaned backups were found (see [Orphaned backup migration](#orphaned-backup-migration)) |

### Methods (Q_INVOKABLE)

| Method | Description |
|--------|-------------|
| `createBackup(sourceId, data, metadata)` | Write a backup with optional metadata map |
| `listBackups()` | Scan for all backups and emit `backupsListed` |
| `restoreBackup(filename)` | Read a backup; auto-downloads if cloud-only |
| `requestDownload(filename)` | Trigger hydration of a cloud-only file |
| `deleteBackup(filename)` | Delete a backup and its metadata sidecar |
| `detect()` | Stage 1: enumerate candidate accounts. No filesystem side effects. Result delivered via `accountsDetected`. See [Storage lifecycle](#storage-lifecycle). |
| `select(id)` | Stage 2: activate the chosen account. `id` is the `AccountId` from a `DetectedAccount.id` (or the resolved id from `resolveAccount`). Creates the backup subdirectory; brings up platform machinery. Status delivered via `statusChanged`. Reentrant — switching does not migrate existing backups. |
| `resolveAccount(identity)` | Maps a persisted `DurableAccountIdentity` to the current in-memory `AccountId`. Returns an `AccountId` whose `type == StorageType::None` if the account is no longer detected; otherwise the resolved `AccountId`, suitable for passing straight to `select()`. |
| `prune(sourceId)` | Apply the current `retentionPolicy` to `sourceId` immediately. Useful after a policy change. No-op while a backup is in progress. |
| `checkForOrphanedBackups()` | Scan lower-priority locations for orphans (see [Orphaned backup migration](#orphaned-backup-migration)) |
| `migrateOrphanedBackups()` | Move detected orphans to the active backend |
| `makeRetentionPolicy(keepLast, keepDaily, keepWeekly, keepMonthly, keepYearly)` | Factory for constructing a `RetentionPolicy` from QML — gadget value types can't be assembled via JS-object literals. See [QML usage example](#qml-usage-example). |
| `makeDurableAccountIdentity(type, tenantId, email)` | Factory for constructing a `DurableAccountIdentity` from QML — same root cause as `makeRetentionPolicy`. Pair with `resolveAccount()` to rehydrate a persisted choice. |
| `makeAccountId(type, accountKey)` | Factory for constructing an `AccountId` from QML. Needed when a picker stores accounts in a `ListModel` — `append()` serialises gadget-typed roles, so `model.id` read back is no longer an `AccountId`. Pass `model.type` and `model.accountKey` through this factory and into `select()`. |

### Signals

| Signal | Description |
|--------|-------------|
| `accountsDetected(accounts)` | `detect()` complete; `accounts` is a `QList<DetectedAccount>`. May fire any time platform events trigger re-detection (e.g. iCloud sign-in change). |
| `statusChanged(status, detail)` | Active target's status changed. Fires from `select()` completion, from `detect()`-driven invalidation (a previously-selected account is no longer Ready), and from platform-event-driven re-detection. See [Storage state can change at runtime](#storage-state-can-change-at-runtime). |
| `backupSucceeded(filename, timestamp)` | Backup created |
| `backupFailed(error, message)` | Backup creation failed (see BackupError enum) |
| `backupsListed(backups)` | Scan complete; `backups` is a `QList<BackupInfo>` |
| `restoreUpdated(filename, status, data, metadata, error, message)` | Restore status update (see RestoreStatus, BackupError enums) |
| `downloadUpdated(filename, status, error, message)` | Download status update (see DownloadStatus, BackupError enums) |
| `downloadProgressChanged(filename, bytesReceived, bytesTotal)` | Download progress (`bytesTotal == -1` means indeterminate) |
| `deleteSucceeded(filename)` / `deleteFailed(filename, error, message)` | Delete result (see BackupError enum) |
| `remoteBackupDetected(sourceId)` | A new backup appeared from another device |
| `orphanedBackupsDetected(orphans)` | Orphan scan complete; `orphans` is a `QVariantList` of `OrphanedBackupInfo` |
| `migrationUpdated(status, migratedCount, totalCount, error, message)` | Migration progress/result (see MigrationStatus, BackupError enums) |

### Enums

**StorageStatus**: `Unknown`, `Ready`, `Unavailable`, `Disabled`, `LocalActive`

**StorageType**: `None`, `ICloud`, `OneDrivePersonal`, `OneDriveCommercial`, `LocalDirectory`

**DownloadState** (on BackupInfo, used by both `downloadState` and `metaDownloadState`): `Local`, `CloudOnly`, `Downloading`, `Error`, `Missing`

**RestoreStatus**: `RestoreDownloading`, `RestoreInProgress`, `RestoreSucceeded`, `RestoreFailed`

**DownloadStatus**: `DownloadInProgress`, `DownloadSucceeded`, `DownloadFailed`

**MigrationStatus**: `MigrationInProgress`, `MigrationSucceeded`, `MigrationFailed`

**BackupError**: `NoError`, `InvalidArgument`, `IOError`, `MetadataIOError`, `CoordinationFailed`, `FileNotLocal`, `DownloadError`, `DownloadTimeout`, `MigrationPartial`, `UnknownError`

### Value types

**RetentionPolicy** — five independent keep rules, each defaulting to 0 (disabled). See [How pruning works](#how-pruning-works) for semantics.

| Field | Description |
|-------|-------------|
| `keepLast` | N most recent backups overall, no time-bucketing |
| `keepDaily` | Latest of each of the N most-recent days-with-backups |
| `keepWeekly` | Latest of each of the N most-recent ISO-weeks-with-backups |
| `keepMonthly` | Latest of each of the N most-recent calendar-months-with-backups |
| `keepYearly` | Latest of each of the N most-recent calendar-years-with-backups |

**BackupInfo** — entries in `backupsListed`:

| Field | Description |
|-------|-------------|
| `sourceId`, `timestamp`, `filename` | Identifying fields |
| `metadata` | Application-supplied map from `createBackup` |
| `downloadState` | State of the `.bak` payload — `Local` / `CloudOnly` / `Downloading` / `Error` / `Missing`. |
| `metaDownloadState` | State of the `.meta` sidecar — same enum as `downloadState`. The metadata map is trustworthy iff `metaDownloadState == Local`; any other value means `metadata` is empty and only `sourceId` / `timestamp` (from the filename) are populated. Values: `Local` (sidecar on disk, parsed); `Missing` (sidecar absent — mid-write, orphan, or interrupted delete); `CloudOnly` (cloud placeholder, scanner skipped open — Apple only, see caveat); `Downloading` (partially hydrated — Apple only); `Error` (open or JSON parse failed). Retention treats anything `!= Local` as **present but unconfirmed** — never pruned, but counted toward bucket occupancy and the min-keep safety net. **Platform asymmetry**: Apple distinguishes all five (and kicks off background hydration on `CloudOnly`); Windows never produces `CloudOnly` / `Downloading` because the scanner always opens (no consumer-callable hydration API on Windows — see the cloud-sync caveat); Local backend only produces `Local`/`Missing`/`Error`. |

**DetectedAccount** — entries in `accountsDetected`:

| Field | Description |
|-------|-------------|
| `id` | `AccountId` — pair this with `select()`. |
| `displayName` | Per-account label fragment. For Windows OneDrive it's the on-disk folder basename (e.g. `"OneDrive"` for Personal, `"OneDrive - Contoso"` for Business — what File Explorer shows). Empty for Apple and Local: `id.type` uniquely identifies those rows, so the consumer composes the label itself. |
| `email` | Account email (OneDrive). Empty for Apple and Local. Part of the durable identity for OneDrive. |
| `tenantId` | Microsoft Entra tenant GUID for OneDrive Business; empty otherwise. Part of the durable identity for Business. |
| `status` | `Ready` (selectable), `Unavailable` (user remediation possible), `Disabled` (blocked outside user's control). |
| `statusDetail` | Human-readable explanation suitable for inline tooltips on disabled rows. |

**AccountId** — in-memory handle returned by detection:

| Field | Description |
|-------|-------------|
| `type` | `StorageType` of the account. |
| `accountKey` | Slot name (e.g. `"Business2"`, `"Personal"`, or `""`). **In-memory only — do not persist.** OneDrive may re-slot the same account at a different index across unlink/re-add, so a saved `accountKey` may resolve to the wrong account or to nothing. Persist a `DurableAccountIdentity` and call `resolveAccount()` at startup. |

**DurableAccountIdentity** — the persistable account identity. Pair to `AccountId`:

| Field | Description |
|-------|-------------|
| `type` | `StorageType`. |
| `tenantId` | Microsoft Entra tenant GUID for OneDrive Business; empty for Personal / Apple / Local. |
| `email` | Account email; empty for Apple and Local. |

Persist this value (as JSON, settings, etc.) and pass it to `resolveAccount()` at startup to recover the current `AccountId`.

## QML usage example

```qml
import QtQuick
import QtCloudBackup

Item {
    CloudBackupManager {
        id: backupManager
        // RetentionPolicy is a gadget value type — assign a whole struct, not
        // sub-properties. Use makeRetentionPolicy() to build it from QML.
        Component.onCompleted: {
            backupManager.retentionPolicy =
                backupManager.makeRetentionPolicy(3, 7, 4, 12, 0)
            //   keepLast=3, keepDaily=7, keepWeekly=4, keepMonthly=12, keepYearly=0
        }

        onStatusChanged: (status, detail) => {
            console.log("Storage:", detail)
        }
        onBackupSucceeded: (filename, timestamp) => {
            console.log("Backup created:", filename)
        }
        onRestoreUpdated: (filename, status, data, metadata, error, message) => {
            switch (status) {
            case QtCloudBackup.RestoreDownloading:
                console.log("Downloading from cloud...")
                break
            case QtCloudBackup.RestoreInProgress:
                console.log("Reading backup...")
                break
            case QtCloudBackup.RestoreSucceeded:
                console.log("Restored", data.byteLength, "bytes")
                // Use data and metadata here
                break
            case QtCloudBackup.RestoreFailed:
                console.log("Restore failed:", error, message)
                break
            }
        }
    }

    // Create a backup
    function save(sourceId, payload) {
        backupManager.createBackup(sourceId, payload, { "device": "iPhone" })
    }

    // Lifecycle: detect candidate accounts, then select one (here: the first
    // Ready entry; a real consumer would either resolve a persisted choice
    // via resolveAccount() or show a picker).
    onAccountsDetected: (accounts) => {
        for (let i = 0; i < accounts.length; i++) {
            if (accounts[i].status === QtCloudBackup.StorageStatus.Ready) {
                backupManager.select(accounts[i].id)
                return
            }
        }
    }

    // List and restore
    Component.onCompleted: {
        backupManager.backupsListed.connect(function(backups) {
            if (backups.length > 0)
                backupManager.restoreBackup(backups[0].filename)
        })
        backupManager.detect()
    }
}
```

## C++ usage example

```cpp
#include <QtCloudBackup/cloudbackupmanager.h>

auto *manager = new CloudBackupManager(this);
manager->setRetentionPolicy({ .keepLast = 3, .keepDaily = 7,
                              .keepWeekly = 4, .keepMonthly = 12 });

connect(manager, &CloudBackupManager::backupSucceeded,
        this, [](const QString &filename, const QDateTime &timestamp) {
    qDebug() << "Backup created:" << filename << "at" << timestamp;
});

connect(manager, &CloudBackupManager::restoreUpdated,
        this, [](const QString &filename, QtCloudBackup::RestoreStatus status,
                 const QByteArray &data, const QVariantMap &metadata,
                 int error, const QString &message) {
    if (status == QtCloudBackup::RestoreStatus::RestoreSucceeded)
        qDebug() << "Restored" << data.size() << "bytes";
    else if (status == QtCloudBackup::RestoreStatus::RestoreFailed)
        qDebug() << "Restore failed:" << error << message;
});

// Lifecycle: detect, then select. A real consumer would resolve a persisted
// durable identity here via resolveAccount() or present a picker.
connect(manager, &CloudBackupManager::accountsDetected, this,
        [manager](const QList<DetectedAccount> &accounts) {
    for (const auto &a : accounts) {
        if (a.status == QtCloudBackup::StorageStatus::Ready) {
            manager->select(a.id);
            return;
        }
    }
});
manager->detect();

// Create a backup once status is Ready
QByteArray payload = /* your serialised data */;
QVariantMap meta = { { "device", "iPhone" } };
manager->createBackup("my-source-id", payload, meta);

// List and restore the latest
connect(manager, &CloudBackupManager::backupsListed,
        this, [manager](const QList<BackupInfo> &backups) {
    if (!backups.isEmpty())
        manager->restoreBackup(backups.first().filename);
});
manager->listBackups();
```

## Storage lifecycle

Construction of `CloudBackupManager` is side-effect-free — no registry reads, no filesystem touches. The consumer drives the lifecycle in two stages:

1. **`detect()`** — enumerates candidate accounts. Result delivered asynchronously via `accountsDetected(QList<DetectedAccount>)`. No filesystem writes; no folder creation.
2. **`select(id)`** — activates one of the detected accounts (`id` is the `AccountId` from a `DetectedAccount.id`). Creates the backup subdirectory, brings up platform machinery (e.g. iCloud's `NSMetadataQuery`), and emits `statusChanged` with the result.

Typical startup flow with a persisted user preference:

```cpp
connect(manager, &CloudBackupManager::accountsDetected, this,
    [manager, savedIdentity /* DurableAccountIdentity */](auto) {
        const AccountId id = manager->resolveAccount(savedIdentity);
        if (id.type != QtCloudBackup::StorageType::None)
            manager->select(id);
        else
            showPicker();  // saved account no longer detected
    });
manager->detect();
```

Or in QML — use `makeDurableAccountIdentity()` to build the persisted identity from primitives (a JS object literal won't auto-convert to a `Q_GADGET` argument at a QML→C++ call boundary):

```qml
CloudBackupManager {
    id: backupManager
    onAccountsDetected: (accounts) => {
        const id = backupManager.resolveAccount(
            backupManager.makeDurableAccountIdentity(
                savedType, savedTenantId, savedEmail))
        if (id.type !== QtCloudBackup.StorageType.None)
            backupManager.select(id)
        else
            picker.open()
    }
    Component.onCompleted: detect()
}
```

`select()` is **reentrant** — calling it with a different `AccountId` switches the active target without tearing down the backend. **Switching does not migrate existing backups**: backups already on the previous target remain there as orphans. Migration is a separate, explicit operation via [Orphaned backup migration](#orphaned-backup-migration); implicit migration on every `select()` would be surprising (a user who picks the wrong account and switches back wouldn't expect a round-trip of file moves).

## Status semantics

`StorageStatus` values map to **distinct user remediation flows** rather than to distinct technical causes. If two states would lead the user to take the same next action, they collapse into one value.

| Value | Meaning | Who can fix |
|---|---|---|
| `Unknown` | Pre-`detect()`. No detection has run yet. | — |
| `Ready` | Storage is available and writable. Per-`DetectedAccount` during detection: "selectable". After `select()`: "the active target is up". | — |
| `Unavailable` | Storage is not configured. User can complete setup (install client / sign in / enable service). | User |
| `Disabled` | Storage is configured-but-blocked by something outside the user's control (IT policy, missing entitlements, unlicensed account, tenant restriction). | Nobody locally |
| `LocalActive` | Local backend is the active selection. Only ever assigned when the consumer explicitly selects `LocalDirectory` — never automatic, hence "active" rather than "fallback". | — |

UI implication: `Disabled` rows should explain the situation without inviting a sign-in attempt; `Unavailable` rows should prompt setup.

### Per-platform mapping

**Apple iCloud** (detection signals: `[NSFileManager ubiquityIdentityToken]`, `URLForUbiquityContainerIdentifier:`):

| Condition | Status |
|---|---|
| Ubiquity token nil (signed out / iCloud Drive off / MDM-restricted) | `Unavailable` |
| Token present, container URL nil (entitlements / provisioning) | `Disabled` |
| Both succeed | `Ready` |

**Windows OneDrive** (detection signals: registry under `HKCU\Software\Microsoft\OneDrive\Accounts\*`; Group Policy as documented above):

| Condition | Status |
|---|---|
| Device-level `DisableFileSyncNGSC = 1` | Backend-level `Disabled`; no accounts enumerated |
| Account fields incomplete (signed-out residue, partial config) | `Unavailable` (per account) |
| Personal account with `DisablePersonalSync = 1` (HKCU or HKLM) | `Disabled` (per account) |
| Business account blocked by `AllowTenantList` / `BlockTenantList` | `Disabled` (per account) |
| `UserFolder` missing or not writable | `Unavailable` (per account) |
| All fields complete, no policy block, folder writable | `Ready` (per account) |

**Local** (detection signal: `QFileInfo::isWritable()` walking up to first existing ancestor):

| Condition | Status |
|---|---|
| Backup directory writable | `Ready` |
| Not writable | `Unavailable` |
| Successfully selected | `LocalActive` |

### Known limitation: Apple MDM-restricted

`CKContainer.accountStatus` (CloudKit) can distinguish four account states including `.restricted` (MDM / parental controls / Screen Time blocks), which would map cleanly to `Disabled`. The library deliberately does **not** bring CloudKit into scope — doing so would require every consumer to add the `CloudKit` (or `CloudKit-Anonymous`) entitlement, enable CloudKit on the App ID in the Apple Developer portal, and regenerate provisioning profiles. Disproportionate cost for a niche case.

Consequence: on Apple, MDM-restricted devices collapse into `Unavailable` along with signed-out and iCloud-Drive-off. The contract is slightly leaky here. If this becomes important later, `CKContainer.accountStatus` can be added as a deliberate enhancement.

## Storage state can change at runtime

A `Ready` outcome from `select()` is not permanent. External events can invalidate the active target without any action from the consumer:

- **Apple**: the library observes `NSUbiquityIdentityDidChangeNotification` and re-runs `detect()` automatically when the user signs in/out of iCloud or toggles iCloud Drive in System Settings.
- **Windows**: re-detection currently only runs when the consumer calls `detect()`, but the same invalidation logic applies whenever a fresh detection shows the active account is no longer `Ready` (policy change, account unlinked, etc.).
- **Local**: same — re-detection is consumer-driven.

When re-detection finds that the previously-selected account is no longer `Ready`, the library tears down the active state (clears the cached path, stops platform machinery) and emits `statusChanged` reflecting the new reality. **This signal is emitted before `accountsDetected`** so the consumer sees "your storage dropped" before "here are the current options".

**Consumer obligations:**

- Connect to `statusChanged` **once at startup** and tolerate events at any time — not only in direct response to your own `select()` calls.
- Don't cache `storageStatus == Ready` and act on the cached value later. Read it at the point of use.
- **In-flight file operations may complete with errors after an invalidating `statusChanged`.** A `writeBackup` worker captured the path before invalidation and runs to OS-level failure; the resulting `writeCompleted(IOError, …)` is a normal error, not a panic. Surface it like any other I/O error.
- Treat `statusChanged(Unavailable | Disabled)` after a previously-Ready state as a prompt to re-show the picker (or whatever your UX is for "storage went away").

## How pruning works

QtCloudBackup uses the **union-of-keeps** retention model familiar from Borg, restic, sanoid, rsnapshot, and (in hardcoded form) Apple Time Machine. A `RetentionPolicy` is a flat collection of five independent keep rules; the set of backups *kept* is the union of every rule's selection, the set *pruned* is everything else. Rules are order-agnostic — `keepDaily 7 + keepWeekly 4` produces the same result as `keepWeekly 4 + keepDaily 7` — and a single backup can satisfy multiple rules (it's kept once).

Pruning runs automatically after each successful `createBackup()` against the just-written `sourceId`, and can be triggered manually via `prune(sourceId)`. The library never persists the policy itself — the consuming app owns persistence (typically `QSettings`) and sets the policy on the manager at startup.

### Worked examples

#### Today + recent days

> "Keep the latest 2 from today, plus the latest one from each of the 5 most-recent prior days that had backups."

```cpp
RetentionPolicy { .keepLast = 2, .keepDaily = 6 }
```

`keepLast = 2` keeps the 2 most recent backups overall (today's, since today is by definition the most recent day with backups). `keepDaily = 6` keeps the latest of each of the 6 most-recent days-with-backups: today plus 5 prior. The "+1" is intentional — a daily rule *includes today* as one of its N days. To get "today plus N prior", ask for `keepDaily (N+1)`. This is the standard Borg/restic convention.

#### Generational rotation

> "Latest 10, plus daily for a week, weekly for a month, monthly for a year."

```cpp
RetentionPolicy {
    .keepLast    = 10,
    .keepDaily   = 7,
    .keepWeekly  = 4,
    .keepMonthly = 12,
}
```

Four independent rules; their selections overlap (today's backup is in all four; the latest backup of each Sunday is in three; etc.) and the union is what's kept.

#### Simple keep-latest-N

```cpp
RetentionPolicy { .keepLast = 5 }
```

Equivalent to a "keep only the most recent N" policy. No bucketing.

#### Long-tail history with dense recent

> "All backups from the last few days, then thinning out indefinitely."

```cpp
RetentionPolicy {
    .keepLast    = 20,
    .keepWeekly  = 8,
    .keepMonthly = 24,
    .keepYearly  = 10,
}
```

Total retained ≈ 20 + 8 + 24 + 10 = 62 (overlap reduces the actual count).

#### Zero retention (safety net engages)

```cpp
RetentionPolicy { }   // all fields zero
```

Every rule disabled. The min-keep safety invariant overrides: the latest backup for the active `sourceId` is retained anyway, and a warning is logged on `qtcloudbackup.retention`. This is the practical "I want as little as possible" policy.

### The "today is consumed by keepDaily" rule

This is the single most common point of user confusion. A daily rule's first bucket is *today*, not "yesterday".

**Trace.** State before write: yesterday × 4, day-2 × 1, day-4 × 1, day-7 × 1, day-10 × 1. Just wrote: today's 1st backup. Apply `RetentionPolicy { .keepLast = 3, .keepDaily = 5 }`:

- `keepLast = 3` selects: today, yesterday's most recent, yesterday's 2nd most recent.
- `keepDaily = 5` selects: today, yesterday's latest, day-2, day-4, day-7.

**Union**: today + yesterday-latest + yesterday-2nd + day-2 + day-4 + day-7 = 6 retained. **Pruned**: yesterday's two oldest + day-10.

Properties that fall out:
- The just-written backup is always safe (most recent overall + latest of today).
- `keepLast` "spills" into prior days when today is sparse: with 1 backup today and `keepLast = 3`, the other 2 slots come from yesterday.
- `keepDaily = 5` includes today as one of its 5 days. For "today plus 5 prior days", ask for `keepDaily = 6`.

### Days-with-backups, not calendar days (the holiday-gap property)

Every bucketed rule counts **buckets that contain backups**, not calendar buckets. This is a deliberate safety property inherited from Borg/restic: if you go on holiday for two weeks and don't back up, you don't return to find the policy has pruned everything.

> A user backs up daily, then takes a 2-week holiday with no backups, then resumes today.
>
> - `keepLast 10`: today + the 9 most-recent backups before the holiday.
> - `keepDaily 7`: today + the 6 most-recent *days-with-backups* before the holiday. The 14-day gap is irrelevant.
> - `keepWeekly 4`: this week + the 3 most-recent *ISO-weeks-with-backups* before the holiday.
> - `keepMonthly 12`: this month + the 11 most-recent *months-with-backups*.
>
> The retained set looks essentially the same as it would without the holiday: dense recent + thinning history.

The alternative reading — "the last 7 calendar days" — would be catastrophic here: returning after a holiday, `keepDaily 7` would find 0 backups in the last 7 calendar days and prune everything. The library never does this.

If you have a single backup from 3 years ago and no activity since, `keepYearly 1` keeps it indefinitely. It's the only record from that year; the library will not delete it just because no new years-with-backups have accumulated.

### Local-time bucketing

All bucketing is done in **local time**. Day boundaries are local midnight; ISO week boundaries are local Monday 00:00; month boundaries are the 1st of the local month, 00:00; year boundaries are 1 January 00:00.

Edge case: a backup taken at 23:59 and another at 00:01 fall into **different** day-buckets. Users near midnight should expect this.

### Per-`sourceId` independence

Retention is evaluated **per `sourceId`**. Each source maintains its own independent bucketing. Two source IDs sharing a storage target do not compete for retention slots.

**Pruning applies only to the `sourceId` that just received a successful write** — or the `sourceId` passed to an explicit `prune()`. Backups for other source IDs are never touched. This matters because two devices may share a storage target (e.g. the same iCloud container) running different app versions with different policies; the running app must not impose its policy on the other device's data.

### Cloud-sync caveat — `metaDownloadState`

A backup is two files: `.bak` (data) and `.meta` (a JSON sidecar with `sourceId`, `timestamp`, and the application metadata map). The two-file structure is consistent on the writer's filesystem at the moment of writing — see [File-level atomicity](#file-level-atomicity). Cloud sync, however, has **no atomicity across multiple files**: iCloud and OneDrive sync per file, evict per file, and on iOS evictions can be aggressive.

The consequence: a reader on another device, or the writer after iOS evicts cached content, may observe partial state — `.bak` present without `.meta`, or one or both as a cloud placeholder. The scanner surfaces these honestly via `BackupInfo.metaDownloadState`, which has the same enum shape as the `.bak`'s `downloadState`:

- **`Local`**: `.meta` is on disk and was parsed successfully — `metadata` / `sourceId` / `timestamp` populated from JSON.
- **`Missing`**: `.meta` is absent (mid-write, orphan, or interrupted delete).
- **`CloudOnly`**: `.meta` is a cloud placeholder. **Apple only** — the scanner deliberately does not call `open()` and instead kicks off background hydration (see below). On Windows, the scanner always opens, so this state never arises.
- **`Downloading`**: `.meta` is partially hydrated. Apple only.
- **`Error`**: `.meta` exists but couldn't be opened (permissions, IO) or its JSON failed to parse.

Whenever `metaDownloadState != Local`, the user-supplied `metadata` map is empty; the filename regex still yields `sourceId` and `timestamp`.

**Why the scanner behaves differently on Apple vs Windows.** Apple has `startDownloadingUbiquitousItemAtURL:` — a non-blocking syscall that asks `bird` to hydrate a file and returns immediately. So Apple gets skip-and-mark + fire-and-forget: cloud-only `.meta` is surfaced fast (`CloudOnly` / `Downloading`), the hydration is requested in the background, and a subsequent `listBackups()` after sync converges sees `Local`. Without that kick-off, evicted `.meta` would stay perpetually `!= Local` and the corresponding `.bak` would be permanently excluded from retention prune — silently leaking storage. Apple's API makes the fix free.

Windows has no equivalent. The only hydration entry point (`CfHydratePlaceholder`) is part of the Cloud Sync Engines provider API, intended for sync-engine implementers (OneDrive itself), not for code that *uses* OneDrive. Without a kick-off, skip-and-mark on Windows would create the very retention-correctness problem that the Apple kick-off prevents — and the obvious workaround (read the file ourselves in a background thread) reintroduces the stuck-thread / pool-pollution risks that the skip was designed to avoid. So Windows takes the opposite trade: the scanner always opens. First-scan latency on a new PC (where every `.meta` may be a placeholder) is paid up-front; subsequent scans are fast because each row is now `Local`. Per-file cost is small enough (~0.3s measured against a real OneDrive) that linear scaling is acceptable.

The practical UX consequence: on Apple, the backup list refreshes instantly and rows fill in their metadata over the next few seconds as `bird` services the requests. On Windows, the first refresh after a fresh install may take several seconds while the OS hydrates the sidecars; subsequent refreshes are instant. Consumers don't need different code paths — `metaDownloadState != Local` means "show as syncing" everywhere — but the *frequency* of seeing that state differs.

Retention treats `metaDownloadState != Local` as **present but unconfirmed**:

- **Excluded from prune candidates.** Not enough information to decide; safer to leave alone until a future scan.
- **Counted toward bucket occupancy.** A day with only a metadata-pending backup still counts as "this day has backups" for the purpose of N.
- **Counted toward the minimum-keep safety net.** Real files representing real backup data; their existence prevents retention from concluding "we have no backups".

Consumers that need an atomic single-unit backup can embed any application metadata directly in the `.bak` payload — the library's `.meta` sidecar is reserved for library bookkeeping and is best-effort under cloud sync.

### Minimum-keep safety invariant

If the policy would prune every backup for the active `sourceId`, the latest one is retained anyway. A warning is logged on the `qtcloudbackup.retention` logging category. This makes a misconfigured all-zero policy non-catastrophic.

If there are any `metaDownloadState != Local` entries (mid-sync, evicted, cloud-only), the safety net is already satisfied — those entries are never pruned and represent real backup data, so no force-keep of confirmed entries is needed.

## File-level atomicity

The library maintains a **writer-local invariant**: at the moment a write or delete returns, the writer's local filesystem holds either both files or neither.

- **Writes** are `.bak` first, then `.meta`. If the `.meta` write fails after the `.bak` succeeded, the `.bak` is rolled back. All three backends behave the same way.
- **Deletes** are `.meta` first, then `.bak`. If the delete is interrupted between the two removals, what remains is an orphan `.bak` — surfaced by the scanner with `metaDownloadState = Missing` rather than left invisible.

This invariant holds **only** at the writer's local filesystem at write/delete time. Cross-device consistency under cloud sync is not guaranteed in real time — see [the cloud-sync caveat](#cloud-sync-caveat--metadownloadstate).

## Orphaned backup migration

*This feature is optional. Apps that don't need it can ignore these methods entirely — the library never auto-checks or auto-migrates.*

When the consumer switches the active account (e.g. the user picks a different OneDrive account, or signs into iCloud and the app re-selects it), backups created against the previous target still exist on disk but are no longer visible through `listBackups()`. `select()` deliberately does **not** migrate — see [Storage lifecycle](#storage-lifecycle) — so the previous backups become orphans until explicitly migrated.

`checkForOrphanedBackups()` scans **the cached detection result minus the currently-selected account**, restricted to accounts whose status is `Ready` (orphans aren't discoverable from `Disabled` / `Unavailable` accounts). For each such account it enumerates the backup subdirectory and emits any `*.bak` files as `OrphanedBackupInfo` entries via `orphanedBackupsDetected`. The app can then present this information to the user and call `migrateOrphanedBackups()` if the user opts in. Migration copies the files to the active backend, deletes the originals, and reports progress via `migrationUpdated`.

Both detection and migration are on-demand — the app decides when and whether to call them.

**Precondition:** `select()` must have been called. Orphans are defined relative to a migration target; calling `checkForOrphanedBackups()` first returns an empty list and logs a warning.

### QML example

```qml
CloudBackupManager {
    id: backupManager

    onOrphanedBackupsDetected: (orphans) => {
        if (orphans.length > 0)
            migrationDialog.open()  // Let the user decide
    }
    onMigrationUpdated: (status, migratedCount, totalCount, error, message) => {
        if (status === QtCloudBackup.MigrationSucceeded)
            console.log("Migrated", migratedCount, "backups")
        else if (status === QtCloudBackup.MigrationFailed)
            console.log("Migration failed:", error, message)
    }
}

// Call at startup or on a button press
Component.onCompleted: backupManager.checkForOrphanedBackups()

// After user confirms
function doMigrate() {
    backupManager.migrateOrphanedBackups()
}
```

### C++ example

```cpp
connect(manager, &CloudBackupManager::orphanedBackupsDetected,
        this, [](const QVariantList &orphans) {
    qDebug() << "Found" << orphans.size() << "orphaned backups";
    // Present to user, then call manager->migrateOrphanedBackups()
});

connect(manager, &CloudBackupManager::migrationUpdated,
        this, [](QtCloudBackup::MigrationStatus status, int migrated,
                 int total, int error, const QString &message) {
    if (status == QtCloudBackup::MigrationStatus::MigrationSucceeded)
        qDebug() << "Migrated" << migrated << "of" << total;
});

manager->checkForOrphanedBackups();
```

### What gets scanned

Concretely: for each `DetectedAccount` in the cached `detect()` result where the id differs from the currently-selected account and the status is `Ready`, the backup subdirectory is enumerated. In current builds (one platform backend compiled in), the practical result is:

- **Apple**: always empty — Apple builds expose a single iCloud account per device.
- **Windows**: any other Ready OneDrive account (Personal ↔ Business, or another Business account on the same machine) becomes an orphan source after switching.
- **Local**: always empty — Local builds expose a single account.

The API is shaped to support future multi-backend builds (e.g. iCloud + Local on macOS) where additional detected accounts naturally appear as orphan sources without further wiring.

### Edge cases

- **Duplicate filenames**: files already present in the active location are skipped (likely from a previous partial migration).
- **Partial failure**: migration continues past individual errors, reports `MigrationFailed` with the count of successes, and leaves failed originals in place for retry.
- **Cloud-sync partial state**: orphan accounts often go unsynced for longer than the active account, so partial states (`.bak` present, `.meta` not yet synced, or vice versa) are more common when scanning orphans. `OrphanedBackupInfo` doesn't carry a `metaDownloadState` field — orphan rows with a cloud-only or missing `.meta` simply have an empty `metadata` map, with `sourceId` and `timestamp` populated from the filename. Consumers presenting an orphan migration UI should expect the listing to evolve as cloud sync converges.
- **A `Disabled` / `Unavailable` account as orphan source**: skipped — orphans are only discoverable from accessible accounts.

## Known limitations

- **iCloud storage full** is undetectable — local writes succeed but sync silently stalls
- **Windows download progress** is indeterminate (`bytesTotal == -1`) because OneDrive hydration provides no granular progress API
- **macOS auto-downloads** iCloud files eagerly, so explicit download testing requires iOS
- **Unsigned macOS builds** cannot access iCloud — code signing is required even for development
- **OneDrive folder is user-visible** — backup files are stored in a regular OneDrive sync folder, so the user can view, delete, or corrupt them

## Licence

MIT — see [LICENSE](LICENSE).
