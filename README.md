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
| `maxBackupsPerSource` | `int` | Pruning threshold per source ID (default: 3) |
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
| `checkForOrphanedBackups()` | Scan lower-priority locations for orphans (see [Orphaned backup migration](#orphaned-backup-migration)) |
| `migrateOrphanedBackups()` | Move detected orphans to the active backend |

### Signals

| Signal | Description |
|--------|-------------|
| `statusChanged(status, detail)` | Storage status changed |
| `backupSucceeded(filename, timestamp)` | Backup created |
| `backupFailed(reason)` | Backup creation failed |
| `backupsListed(backups)` | Scan complete; `backups` is a `QList<BackupInfo>` |
| `restoreUpdated(filename, status, data, metadata, reason)` | Restore status update (see RestoreStatus enum) |
| `downloadUpdated(filename, status, reason)` | Download status update (see DownloadStatus enum) |
| `downloadProgressChanged(filename, bytesReceived, bytesTotal)` | Download progress (`bytesTotal == -1` means indeterminate) |
| `deleteSucceeded(filename)` / `deleteFailed(filename, reason)` | Delete result |
| `remoteBackupDetected(sourceId)` | A new backup appeared from another device |
| `orphanedBackupsDetected(orphans)` | Orphan scan complete; `orphans` is a `QVariantList` of `OrphanedBackupInfo` |
| `migrationUpdated(status, migratedCount, totalCount, reason)` | Migration progress/result (see MigrationStatus enum) |

### Enums

**StorageStatus**: `Unknown`, `Ready`, `Unavailable`, `Disabled`, `LocalFallback`

**StorageType**: `None`, `ICloud`, `OneDrivePersonal`, `OneDriveCommercial`, `LocalDirectory`

**DownloadState** (on BackupInfo): `Local`, `CloudOnly`, `Downloading`, `Error`

**RestoreStatus**: `RestoreDownloading`, `RestoreInProgress`, `RestoreSucceeded`, `RestoreFailed`

**DownloadStatus**: `DownloadInProgress`, `DownloadSucceeded`, `DownloadFailed`

**MigrationStatus**: `MigrationInProgress`, `MigrationSucceeded`, `MigrationFailed`

## QML usage example

```qml
import QtQuick
import QtCloudBackup

Item {
    CloudBackupManager {
        id: backupManager
        maxBackupsPerSource: 3

        onStatusChanged: (status, detail) => {
            console.log("Storage:", detail)
        }
        onBackupSucceeded: (filename, timestamp) => {
            console.log("Backup created:", filename)
        }
        onRestoreUpdated: (filename, status, data, metadata, reason) => {
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
                console.log("Restore failed:", reason)
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
manager->setMaxBackupsPerSource(3);

connect(manager, &CloudBackupManager::backupSucceeded,
        this, [](const QString &filename, const QDateTime &timestamp) {
    qDebug() << "Backup created:" << filename << "at" << timestamp;
});

connect(manager, &CloudBackupManager::restoreUpdated,
        this, [](const QString &filename, QtCloudBackup::RestoreStatus status,
                 const QByteArray &data, const QVariantMap &metadata,
                 const QString &reason) {
    if (status == QtCloudBackup::RestoreStatus::RestoreSucceeded)
        qDebug() << "Restored" << data.size() << "bytes";
    else if (status == QtCloudBackup::RestoreStatus::RestoreFailed)
        qDebug() << "Restore failed:" << reason;
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

After each successful `createBackup()`, the manager scans for all backups matching the same `sourceId` and deletes the oldest ones that exceed `maxBackupsPerSource` (default: 3). Pruning happens automatically — the consuming app does not need to manage it.

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
    onMigrationUpdated: (status, migratedCount, totalCount, reason) => {
        if (status === CloudBackup.MigrationSucceeded)
            console.log("Migrated", migratedCount, "backups")
        else if (status === CloudBackup.MigrationFailed)
            console.log("Migration failed:", reason)
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
                 int total, const QString &reason) {
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
