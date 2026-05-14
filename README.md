# QtCloudBackup

A Qt 6.8 library for writing backup files to cloud-synced local storage (iCloud on Apple platforms, OneDrive on Windows).

## What it does

QtCloudBackup provides a single C++ class (`CloudBackupManager`) registered as a `QML_ELEMENT` that:

- Detects whether a cloud-backed storage location is available
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
| Windows | OneDrive (auto-detected) | OneDrive sync folder, configurable subfolder |
| Other | Local fallback | `QStandardPaths::AppLocalDataLocation/Backups/` |

On Windows, the library auto-detects OneDrive using environment variables and registry keys, with a layered fallback chain: OneDrive for Business → OneDrive Personal → Documents folder.

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

No special setup required. The library auto-detects OneDrive via:

1. `%OneDriveCommercial%` environment variable
2. Registry: `HKCU\Software\Microsoft\OneDrive\Accounts\Business1\UserFolder`
3. `%OneDriveConsumer%` environment variable
4. Registry: `HKCU\Software\Microsoft\OneDrive\Accounts\Personal\UserFolder`
5. `%OneDrive%` environment variable
6. Documents folder fallback (`SHGetKnownFolderPath`)

Set `QTCLOUDBACKUP_WINDOWS_BACKUP_PATH` to a relative path within OneDrive (e.g. `"YourApp/Backups"`) to avoid polluting the root.

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
| `refresh()` | Re-check cloud availability and reinitialise |
| `prune(sourceId)` | Apply the current `retentionPolicy` to `sourceId` immediately. Useful after a policy change. No-op while a backup is in progress. |
| `checkForOrphanedBackups()` | Scan lower-priority locations for orphans (see [Orphaned backup migration](#orphaned-backup-migration)) |
| `migrateOrphanedBackups()` | Move detected orphans to the active backend |
| `makeRetentionPolicy(keepLast, keepDaily, keepWeekly, keepMonthly, keepYearly)` | Factory for constructing a `RetentionPolicy` from QML — gadget value types can't be assembled via JS-object literals. See [QML usage example](#qml-usage-example). |

### Signals

| Signal | Description |
|--------|-------------|
| `statusChanged(status, detail)` | Storage status changed |
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

**StorageStatus**: `Unknown`, `Ready`, `Unavailable`, `Disabled`, `LocalFallback`

**StorageType**: `None`, `ICloud`, `OneDrivePersonal`, `OneDriveCommercial`, `LocalDirectory`

**DownloadState** (on BackupInfo): `Local`, `CloudOnly`, `Downloading`, `Error`

**RestoreStatus**: `RestoreDownloading`, `RestoreInProgress`, `RestoreSucceeded`, `RestoreFailed`

**DownloadStatus**: `DownloadInProgress`, `DownloadSucceeded`, `DownloadFailed`

**MigrationStatus**: `MigrationInProgress`, `MigrationSucceeded`, `MigrationFailed`

**BackupError**: `NoError`, `InvalidArgument`, `IOError`, `MetadataIOError`, `CoordinationFailed`, `FileNotLocal`, `DownloadFailed`, `DownloadTimeout`, `MigrationPartial`, `UnknownError`

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
| `downloadState` | `Local` / `CloudOnly` / `Downloading` / `Error` |
| `metadataAvailable` | `true` when the `.meta` sidecar was readable. `false` when the `.bak` is observed without its `.meta` (mid-sync, evicted, or orphaned). Consumers may render these as "metadata syncing"; retention treats them as present-but-unconfirmed. |

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
            case CloudBackup.RestoreDownloading:
                console.log("Downloading from cloud...")
                break
            case CloudBackup.RestoreInProgress:
                console.log("Reading backup...")
                break
            case CloudBackup.RestoreSucceeded:
                console.log("Restored", data.byteLength, "bytes")
                // Use data and metadata here
                break
            case CloudBackup.RestoreFailed:
                console.log("Restore failed:", error, message)
                break
            }
        }
    }

    // Create a backup
    function save(sourceId, payload) {
        backupManager.createBackup(sourceId, payload, { "device": "iPhone" })
    }

    // List and restore
    Component.onCompleted: {
        backupManager.backupsListed.connect(function(backups) {
            if (backups.length > 0)
                backupManager.restoreBackup(backups[0].filename)
        })
        backupManager.listBackups()
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

// Create a backup
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

### Cloud-sync caveat — `metadataAvailable`

A backup is two files: `.bak` (data) and `.meta` (a JSON sidecar with `sourceId`, `timestamp`, and the application metadata map). The two-file structure is consistent on the writer's filesystem at the moment of writing — see [File-level atomicity](#file-level-atomicity). Cloud sync, however, has **no atomicity across multiple files**: iCloud and OneDrive sync per file, evict per file, and in iOS evictions can be aggressive.

The consequence: a reader on another device, or the writer after iOS evicts cached content, may observe partial state — `.bak` present without `.meta`, or vice versa. The scanner surfaces these honestly via `BackupInfo.metadataAvailable`:

- **`.bak` with `.meta`**: `metadataAvailable = true`. Standard case.
- **`.bak` without `.meta`**: `metadataAvailable = false`. The filename still encodes `sourceId` and `timestamp` (so those populate), but the user-supplied metadata map is empty.

Retention treats `metadataAvailable == false` entries as **present but unconfirmed**:

- **Excluded from prune candidates.** Not enough information to decide; safer to leave alone until a future scan.
- **Counted toward bucket occupancy.** A day with only a metadata-pending backup still counts as "this day has backups" for the purpose of N.
- **Counted toward the minimum-keep safety net.** Real files representing real backup data; their existence prevents retention from concluding "we have no backups".

Consequence: a mid-sync backup is left alone on this prune pass and gets full treatment on the next scan, once the `.meta` has arrived (or been hydrated). Consumers that need an atomic single-unit backup can embed any application metadata directly in the `.bak` payload — the library's `.meta` sidecar is reserved for library bookkeeping and is best-effort under cloud sync.

### Minimum-keep safety invariant

If the policy would prune every backup for the active `sourceId`, the latest one is retained anyway. A warning is logged on the `qtcloudbackup.retention` logging category. This makes a misconfigured all-zero policy non-catastrophic.

If there are any `metadataAvailable = false` entries (mid-sync, evicted), the safety net is already satisfied — those entries are never pruned and represent real backup data, so no force-keep of confirmed entries is needed.

## File-level atomicity

The library maintains a **writer-local invariant**: at the moment a write or delete returns, the writer's local filesystem holds either both files or neither.

- **Writes** are `.bak` first, then `.meta`. If the `.meta` write fails after the `.bak` succeeded, the `.bak` is rolled back. All three backends behave the same way.
- **Deletes** are `.meta` first, then `.bak`. If the delete is interrupted between the two removals, what remains is an orphan `.bak` — surfaced by the scanner with `metadataAvailable = false` rather than left invisible.

This invariant holds **only** at the writer's local filesystem at write/delete time. Cross-device consistency under cloud sync is not guaranteed in real time — see [the cloud-sync caveat](#cloud-sync-caveat--metadataavailable).

## Orphaned backup migration

*This feature is optional. Apps that don't need it can ignore these methods entirely — the library never auto-checks or auto-migrates.*

When storage availability changes between sessions — for example, the user signs into iCloud, installs OneDrive, or switches from OneDrive Personal to Commercial — the library selects a higher-priority backend on next launch. Backups created in the previous location still exist on disk but are no longer visible through `listBackups()`.

`checkForOrphanedBackups()` scans lower-priority storage locations for these leftover files. If any are found, `orphanedBackupsDetected` emits a list of `OrphanedBackupInfo` items (each with `sourceId`, `timestamp`, `filename`, `originStorageType`, and `originPath`), and `hasOrphanedBackups` becomes `true`. The app can then present this information to the user and call `migrateOrphanedBackups()` if the user opts in. Migration copies the files to the active backend, deletes the originals, and reports progress via `migrationUpdated`.

Both detection and migration are on-demand — the app decides when and whether to call them.

### QML example

```qml
CloudBackupManager {
    id: backupManager

    onOrphanedBackupsDetected: (orphans) => {
        if (orphans.length > 0)
            migrationDialog.open()  // Let the user decide
    }
    onMigrationUpdated: (status, migratedCount, totalCount, error, message) => {
        if (status === CloudBackup.MigrationSucceeded)
            console.log("Migrated", migratedCount, "backups")
        else if (status === CloudBackup.MigrationFailed)
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

| Active backend | Locations scanned for orphans |
|---------------|-------------------------------|
| iCloud | Local fallback (`AppLocalDataLocation/Backups/`) |
| OneDrive Commercial | Lower-priority OneDrive candidates + local fallback |
| OneDrive Personal | Local fallback |
| Local fallback | Nothing (no lower-priority location exists) |

### Edge cases

- **Duplicate filenames**: files already present in the active location are skipped (likely from a previous partial migration).
- **Partial failure**: migration continues past individual errors, reports `MigrationFailed` with the count of successes, and leaves failed originals in place for retry.
- **iCloud signed out**: the local fallback backend is active, so there's nothing lower-priority to scan — `checkForOrphanedBackups()` returns an empty list. Orphans reappear when the user signs back in.

## Known limitations

- **iCloud storage full** is undetectable — local writes succeed but sync silently stalls
- **Windows download progress** is indeterminate (`bytesTotal == -1`) because OneDrive hydration provides no granular progress API
- **macOS auto-downloads** iCloud files eagerly, so explicit download testing requires iOS
- **Unsigned macOS builds** cannot access iCloud — code signing is required even for development
- **OneDrive folder is user-visible** — backup files are stored in a regular OneDrive sync folder, so the user can view, delete, or corrupt them

## Licence

MIT — see [LICENSE](LICENSE).
