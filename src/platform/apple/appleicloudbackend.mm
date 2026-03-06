#include "appleicloudbackend.h"

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QRegularExpression>
#include <QtConcurrent>

#import <Foundation/Foundation.h>

#ifndef QTCLOUDBACKUP_ICLOUD_CONTAINER
#define QTCLOUDBACKUP_ICLOUD_CONTAINER ""
#endif

static NSString *containerIdentifier()
{
    NSString *id = @QTCLOUDBACKUP_ICLOUD_CONTAINER;
    return id.length > 0 ? id : nil;
}

std::unique_ptr<CloudBackupBackend> createPlatformBackend()
{
    return std::make_unique<AppleICloudBackend>();
}

AppleICloudBackend::~AppleICloudBackend()
{
    stopMetadataQuery();

    if (m_notificationObserver) {
        [[NSNotificationCenter defaultCenter] removeObserver:(__bridge id)m_notificationObserver];
        CFRelease(m_notificationObserver);
        m_notificationObserver = nullptr;
    }
}

void AppleICloudBackend::initialise()
{
    // Observe iCloud account changes
    if (!m_notificationObserver) {
        auto *backend = this;
        id observer = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSUbiquityIdentityDidChangeNotification
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification *) {
                        backend->initialise();
                    }];
        m_notificationObserver = (void *)CFBridgingRetain(observer);
    }

    // Check ubiquity identity token on main thread (lightweight)
    id token = [[NSFileManager defaultManager] ubiquityIdentityToken];
    if (!token) {
        m_status = QtCloudBackup::StorageStatus::Unavailable;
        m_statusDetail = tr("iCloud is not available — not signed in or iCloud Drive is disabled");
        m_containerUrl.clear();
        stopMetadataQuery();
        emit statusChanged(m_status, m_statusDetail);
        return;
    }

    // Resolve container URL off main thread — blocks on first call
    (void)QtConcurrent::run([this] {
        @autoreleasepool {
            NSURL *url = [[NSFileManager defaultManager]
                URLForUbiquityContainerIdentifier:containerIdentifier()];

            if (!url) {
                QMetaObject::invokeMethod(this, [this] {
                    m_status = QtCloudBackup::StorageStatus::Disabled;
                    m_statusDetail = tr("iCloud container is not available — check entitlements and provisioning profile");
                    m_containerUrl.clear();
                    stopMetadataQuery();
                    emit statusChanged(m_status, m_statusDetail);
                }, Qt::QueuedConnection);
                return;
            }

            // Create Backups subdirectory
            NSURL *backupsUrl = [url URLByAppendingPathComponent:@"Backups" isDirectory:YES];
            NSError *error = nil;
            [[NSFileManager defaultManager] createDirectoryAtURL:backupsUrl
                                     withIntermediateDirectories:YES
                                                      attributes:nil
                                                           error:&error];

            QString containerPath = QString::fromNSString(backupsUrl.path);

            QMetaObject::invokeMethod(this, [this, containerPath] {
                m_containerUrl = QUrl::fromLocalFile(containerPath);
                m_status = QtCloudBackup::StorageStatus::Ready;
                m_statusDetail = tr("iCloud storage is available");
                emit statusChanged(m_status, m_statusDetail);
                startMetadataQuery();
            }, Qt::QueuedConnection);
        }
    });
}

QtCloudBackup::StorageStatus AppleICloudBackend::storageStatus() const
{
    return m_status;
}

QString AppleICloudBackend::statusDetail() const
{
    return m_statusDetail;
}

QtCloudBackup::StorageType AppleICloudBackend::storageType() const
{
    return QtCloudBackup::StorageType::ICloud;
}

QString AppleICloudBackend::backupDir() const
{
    return m_containerUrl.toLocalFile();
}

// --- NSMetadataQuery for file discovery and remote change detection ---

void AppleICloudBackend::startMetadataQuery()
{
    if (m_metadataQuery)
        return;

    @autoreleasepool {
        NSMetadataQuery *query = [[NSMetadataQuery alloc] init];
        query.searchScopes = @[NSMetadataQueryUbiquitousDataScope];
        query.predicate = [NSPredicate predicateWithFormat:@"%K LIKE 'qtcloudbackup_*.bak'",
                           NSMetadataItemFSNameKey];

        QPointer<AppleICloudBackend> guard(this);

        [[NSNotificationCenter defaultCenter]
            addObserverForName:NSMetadataQueryDidFinishGatheringNotification
                        object:query
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification *) {
                        if (guard)
                            guard->handleQueryResults();
                    }];

        [[NSNotificationCenter defaultCenter]
            addObserverForName:NSMetadataQueryDidUpdateNotification
                        object:query
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification *) {
                        if (!guard)
                            return;
                        // Don't iterate note.userInfo items — they are proxy objects
                        // that can be deallocated by the time this block runs, causing
                        // EXC_BAD_ACCESS. Instead, just notify that changes occurred.
                        guard->handleQueryResults();
                    }];

        [query startQuery];
        m_metadataQuery = (void *)CFBridgingRetain(query);
    }
}

void AppleICloudBackend::stopMetadataQuery()
{
    if (!m_metadataQuery)
        return;

    @autoreleasepool {
        NSMetadataQuery *query = (__bridge NSMetadataQuery *)m_metadataQuery;
        [query stopQuery];
        [[NSNotificationCenter defaultCenter] removeObserver:query];
        CFRelease(m_metadataQuery);
        m_metadataQuery = nullptr;
    }
}

void AppleICloudBackend::handleQueryResults()
{
    // Called from NSMetadataQuery notifications on the main thread.
    // Access the query's results array (safe) rather than notification
    // userInfo items (proxy objects that can be deallocated).
    if (!m_metadataQuery)
        return;

    @autoreleasepool {
        NSMetadataQuery *query = (__bridge NSMetadataQuery *)m_metadataQuery;
        [query disableUpdates];

        static const QRegularExpression re(
            QStringLiteral("^qtcloudbackup_(.+)_(\\d{8}_\\d{6}_\\d{3})_[a-z0-9]{4}\\.bak$"));

        QSet<QString> sourceIds;
        for (NSUInteger i = 0; i < query.resultCount; i++) {
            NSMetadataItem *item = [query resultAtIndex:i];
            NSString *name = [item valueForAttribute:NSMetadataItemFSNameKey];
            if (!name)
                continue;
            QString qName = QString::fromNSString(name);
            auto match = re.match(qName);
            if (match.hasMatch())
                sourceIds.insert(match.captured(1));
        }

        [query enableUpdates];

        for (const QString &sourceId : sourceIds)
            emit remoteChangeDetected(sourceId);
    }
}

// --- File operations ---

void AppleICloudBackend::writeBackup(const QString &filename, const QByteArray &data,
                                      const QJsonObject &meta)
{
    QString dir = backupDir();
    (void)QtConcurrent::run([this, dir, filename, data, meta] {
        @autoreleasepool {
            QString bakPath = dir + QLatin1Char('/') + filename;
            QString metaPath = bakPath;
            metaPath.replace(QStringLiteral(".bak"), QStringLiteral(".meta"));

            NSURL *bakUrl = [NSURL fileURLWithPath:bakPath.toNSString()];
            NSURL *metaUrl = [NSURL fileURLWithPath:metaPath.toNSString()];

            // Coordinated write for .bak
            NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
            __block bool success = true;
            __block QString errorReason;

            NSError *coordError = nil;
            [coordinator coordinateWritingItemAtURL:bakUrl
                                           options:NSFileCoordinatorWritingForReplacing
                                             error:&coordError
                                        byAccessor:^(NSURL *newURL) {
                NSData *nsData = [NSData dataWithBytes:data.constData() length:data.size()];
                NSError *writeError = nil;
                if (![nsData writeToURL:newURL options:NSDataWritingAtomic error:&writeError]) {
                    success = false;
                    errorReason = tr("Failed to write backup file: %1")
                        .arg(QString::fromNSString(writeError.localizedDescription));
                }
            }];

            if (coordError) {
                success = false;
                errorReason = tr("File coordination failed for backup: %1")
                    .arg(QString::fromNSString(coordError.localizedDescription));
            }

            if (!success) {
                QString reason = errorReason;
                QMetaObject::invokeMethod(this, [this, reason] {
                    emit writeFailed(reason);
                }, Qt::QueuedConnection);
                return;
            }

            // Coordinated write for .meta
            NSError *metaCoordError = nil;
            [coordinator coordinateWritingItemAtURL:metaUrl
                                           options:NSFileCoordinatorWritingForReplacing
                                             error:&metaCoordError
                                        byAccessor:^(NSURL *newURL) {
                QByteArray metaJson = QJsonDocument(meta).toJson(QJsonDocument::Compact);
                NSData *nsData = [NSData dataWithBytes:metaJson.constData() length:metaJson.size()];
                NSError *writeError = nil;
                if (![nsData writeToURL:newURL options:NSDataWritingAtomic error:&writeError]) {
                    success = false;
                    errorReason = tr("Failed to write metadata file: %1")
                        .arg(QString::fromNSString(writeError.localizedDescription));
                }
            }];

            if (metaCoordError) {
                success = false;
                errorReason = tr("File coordination failed for metadata: %1")
                    .arg(QString::fromNSString(metaCoordError.localizedDescription));
            }

            if (!success) {
                // Remove the .bak file since meta failed
                [[NSFileManager defaultManager] removeItemAtURL:bakUrl error:nil];
                QString reason = errorReason;
                QMetaObject::invokeMethod(this, [this, reason] {
                    emit writeFailed(reason);
                }, Qt::QueuedConnection);
                return;
            }

            QMetaObject::invokeMethod(this, [this, filename] {
                emit writeSucceeded(filename);
            }, Qt::QueuedConnection);
        }
    });
}

void AppleICloudBackend::readBackup(const QString &filename)
{
    QString dir = backupDir();
    (void)QtConcurrent::run([this, dir, filename] {
        @autoreleasepool {
            QString bakPath = dir + QLatin1Char('/') + filename;
            QString metaPath = bakPath;
            metaPath.replace(QStringLiteral(".bak"), QStringLiteral(".meta"));

            NSURL *bakUrl = [NSURL fileURLWithPath:bakPath.toNSString()];

            // Check if file needs downloading first
            NSNumber *isDownloaded = nil;
            [bakUrl getResourceValue:&isDownloaded
                              forKey:NSURLUbiquitousItemDownloadingStatusKey
                               error:nil];
            if (isDownloaded &&
                [isDownloaded isEqual:NSURLUbiquitousItemDownloadingStatusNotDownloaded]) {
                // Trigger download and report — caller should wait for downloadReady
                NSError *dlError = nil;
                [[NSFileManager defaultManager] startDownloadingUbiquitousItemAtURL:bakUrl
                                                                             error:&dlError];
                if (dlError) {
                    QMetaObject::invokeMethod(this, [this, filename,
                            reason = tr("Failed to start download: %1")
                                .arg(QString::fromNSString(dlError.localizedDescription))] {
                        emit readFailed(filename, reason);
                    }, Qt::QueuedConnection);
                } else {
                    QMetaObject::invokeMethod(this, [this, filename,
                            reason = tr("File is cloud-only — download triggered, retry after downloadReady")] {
                        emit readFailed(filename, reason);
                    }, Qt::QueuedConnection);
                }
                return;
            }

            // Coordinated read
            NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
            __block QByteArray data;
            __block bool success = true;
            __block QString errorReason;

            NSError *coordError = nil;
            [coordinator coordinateReadingItemAtURL:bakUrl
                                           options:0
                                             error:&coordError
                                        byAccessor:^(NSURL *newURL) {
                NSData *nsData = [NSData dataWithContentsOfURL:newURL];
                if (nsData) {
                    data = QByteArray(reinterpret_cast<const char *>(nsData.bytes),
                                     static_cast<int>(nsData.length));
                } else {
                    success = false;
                    errorReason = tr("Failed to read backup file");
                }
            }];

            if (coordError) {
                success = false;
                errorReason = tr("File coordination failed for read: %1")
                    .arg(QString::fromNSString(coordError.localizedDescription));
            }

            if (!success) {
                QString reason = errorReason;
                QMetaObject::invokeMethod(this, [this, filename, reason] {
                    emit readFailed(filename, reason);
                }, Qt::QueuedConnection);
                return;
            }

            // Read metadata (best effort, no coordination needed for small file)
            QJsonObject meta;
            NSURL *metaUrl = [NSURL fileURLWithPath:metaPath.toNSString()];
            NSData *metaData = [NSData dataWithContentsOfURL:metaUrl];
            if (metaData) {
                QByteArray metaBytes(reinterpret_cast<const char *>(metaData.bytes),
                                     static_cast<int>(metaData.length));
                meta = QJsonDocument::fromJson(metaBytes).object();
            }

            QByteArray readData = data;
            QMetaObject::invokeMethod(this, [this, filename, readData, meta] {
                emit readSucceeded(filename, readData, meta);
            }, Qt::QueuedConnection);
        }
    });
}

void AppleICloudBackend::deleteBackup(const QString &filename)
{
    QString dir = backupDir();
    (void)QtConcurrent::run([this, dir, filename] {
        @autoreleasepool {
            QString bakPath = dir + QLatin1Char('/') + filename;
            QString metaPath = bakPath;
            metaPath.replace(QStringLiteral(".bak"), QStringLiteral(".meta"));

            NSURL *bakUrl = [NSURL fileURLWithPath:bakPath.toNSString()];
            NSURL *metaUrl = [NSURL fileURLWithPath:metaPath.toNSString()];

            NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
            __block bool success = true;
            __block QString errorReason;

            NSError *coordError = nil;
            [coordinator coordinateWritingItemAtURL:bakUrl
                                           options:NSFileCoordinatorWritingForDeleting
                                             error:&coordError
                                        byAccessor:^(NSURL *newURL) {
                NSError *removeError = nil;
                if (![[NSFileManager defaultManager] removeItemAtURL:newURL error:&removeError]) {
                    success = false;
                    errorReason = tr("Failed to delete backup file: %1")
                        .arg(QString::fromNSString(removeError.localizedDescription));
                }
            }];

            if (coordError) {
                success = false;
                errorReason = tr("File coordination failed for delete: %1")
                    .arg(QString::fromNSString(coordError.localizedDescription));
            }

            // Delete meta sidecar (best effort)
            [[NSFileManager defaultManager] removeItemAtURL:metaUrl error:nil];

            // Resolve any file version conflicts
            NSArray *conflicts = [NSFileVersion unresolvedConflictVersionsOfItemAtURL:bakUrl];
            for (NSFileVersion *version in conflicts) {
                version.resolved = YES;
                [version removeAndReturnError:nil];
            }

            bool deleteOk = success;
            QString reason = errorReason;
            QMetaObject::invokeMethod(this, [this, filename, deleteOk, reason] {
                emit deleteCompleted(filename, deleteOk, reason);
            }, Qt::QueuedConnection);
        }
    });
}

void AppleICloudBackend::scanBackups()
{
    QString dir = backupDir();
    (void)QtConcurrent::run([this, dir] {
        @autoreleasepool {
            QList<BackupInfo> backups;

            // Use QDir for local files
            QDir d(dir);
            QStringList entries = d.entryList({QStringLiteral("qtcloudbackup_*.bak")},
                                              QDir::Files, QDir::Name);

            static const QRegularExpression re(
                QStringLiteral("^qtcloudbackup_(.+)_(\\d{8}_\\d{6}_\\d{3})_[a-z0-9]{4}\\.bak$"));

            for (const QString &entry : entries) {
                BackupInfo info;
                info.filename = entry;

                // Determine download state
                QString fullPath = dir + QLatin1Char('/') + entry;
                NSURL *fileUrl = [NSURL fileURLWithPath:fullPath.toNSString()];
                NSString *downloadStatus = nil;
                [fileUrl getResourceValue:&downloadStatus
                                   forKey:NSURLUbiquitousItemDownloadingStatusKey
                                    error:nil];

                if (!downloadStatus ||
                    [downloadStatus isEqual:NSURLUbiquitousItemDownloadingStatusCurrent]) {
                    info.downloadState = QtCloudBackup::DownloadState::Local;
                } else if ([downloadStatus isEqual:NSURLUbiquitousItemDownloadingStatusNotDownloaded]) {
                    info.downloadState = QtCloudBackup::DownloadState::CloudOnly;
                } else if ([downloadStatus isEqual:NSURLUbiquitousItemDownloadingStatusDownloaded]) {
                    info.downloadState = QtCloudBackup::DownloadState::Downloading;
                }

                // Try to read .meta sidecar
                QString metaPath = dir + QLatin1Char('/') + entry;
                metaPath.replace(QStringLiteral(".bak"), QStringLiteral(".meta"));
                QFile metaFile(metaPath);
                if (metaFile.open(QIODevice::ReadOnly)) {
                    QJsonObject meta = QJsonDocument::fromJson(metaFile.readAll()).object();
                    info.sourceId = meta[QStringLiteral("sourceId")].toString();
                    info.timestamp = QDateTime::fromString(
                        meta[QStringLiteral("timestamp")].toString(), Qt::ISODateWithMs);
                    info.metadata = meta[QStringLiteral("metadata")].toObject().toVariantMap();
                }

                // Fallback: parse filename
                if (info.sourceId.isEmpty() || !info.timestamp.isValid()) {
                    auto match = re.match(entry);
                    if (match.hasMatch()) {
                        info.sourceId = match.captured(1);
                        info.timestamp = QDateTime::fromString(
                            match.captured(2), QStringLiteral("yyyyMMdd_HHmmss_zzz"));
                        info.timestamp.setTimeZone(QTimeZone::utc());
                    }
                }

                backups.append(info);
            }

            // Also query NSMetadataQuery for cloud-only files not in directory listing
            if (m_metadataQuery) {
                NSMetadataQuery *query = (__bridge NSMetadataQuery *)m_metadataQuery;
                [query disableUpdates];

                QSet<QString> localFilenames;
                for (const auto &b : backups)
                    localFilenames.insert(b.filename);

                for (NSUInteger i = 0; i < query.resultCount; i++) {
                    NSMetadataItem *item = [query resultAtIndex:i];
                    NSString *name = [item valueForAttribute:NSMetadataItemFSNameKey];
                    QString qName = QString::fromNSString(name);

                    if (localFilenames.contains(qName))
                        continue; // Already found via QDir

                    BackupInfo info;
                    info.filename = qName;
                    info.downloadState = QtCloudBackup::DownloadState::CloudOnly;

                    // Check download percentage
                    NSNumber *percent = [item valueForAttribute:
                        NSMetadataUbiquitousItemPercentDownloadedKey];
                    if (percent && percent.doubleValue > 0 && percent.doubleValue < 100) {
                        info.downloadState = QtCloudBackup::DownloadState::Downloading;
                    }

                    // Parse filename for metadata
                    auto match = re.match(qName);
                    if (match.hasMatch()) {
                        info.sourceId = match.captured(1);
                        info.timestamp = QDateTime::fromString(
                            match.captured(2), QStringLiteral("yyyyMMdd_HHmmss_zzz"));
                        info.timestamp.setTimeZone(QTimeZone::utc());
                    }

                    backups.append(info);
                }

                [query enableUpdates];
            }

            QMetaObject::invokeMethod(this, [this, backups] {
                emit scanCompleted(backups);
            }, Qt::QueuedConnection);
        }
    });
}

void AppleICloudBackend::triggerDownload(const QString &filename)
{
    QString dir = backupDir();
    (void)QtConcurrent::run([this, dir, filename] {
        @autoreleasepool {
            QString fullPath = dir + QLatin1Char('/') + filename;
            NSURL *fileUrl = [NSURL fileURLWithPath:fullPath.toNSString()];

            NSError *error = nil;
            if (![[NSFileManager defaultManager] startDownloadingUbiquitousItemAtURL:fileUrl
                                                                               error:&error]) {
                QString reason = error
                    ? tr("Failed to start download: %1").arg(QString::fromNSString(error.localizedDescription))
                    : tr("Failed to start download");
                QMetaObject::invokeMethod(this, [this, filename, reason] {
                    emit downloadCompleted(filename, false, reason);
                }, Qt::QueuedConnection);
                return;
            }

            // Monitor download progress by polling.
            // NSURL caches resource values, so we must create a fresh URL
            // each iteration to get updated status.
            bool downloaded = false;
            for (int attempt = 0; attempt < 600; ++attempt) { // Up to ~60 seconds
                // Create fresh NSURL to avoid cached resource values
                NSURL *freshUrl = [NSURL fileURLWithPath:fullPath.toNSString()];

                NSString *downloadStatus = nil;
                [freshUrl getResourceValue:&downloadStatus
                                    forKey:NSURLUbiquitousItemDownloadingStatusKey
                                     error:nil];

                // nil status means the file is not ubiquitous (local-only) or
                // already fully materialized — either way, it's available
                if (!downloadStatus ||
                    [downloadStatus isEqual:NSURLUbiquitousItemDownloadingStatusCurrent]) {
                    downloaded = true;
                    break;
                }

                // "Downloaded" means data is local but may still be processing
                if ([downloadStatus isEqual:NSURLUbiquitousItemDownloadingStatusDownloaded]) {
                    // Verify the file is actually readable
                    if ([[NSFileManager defaultManager] isReadableFileAtPath:freshUrl.path]) {
                        downloaded = true;
                        break;
                    }
                }

                // Report progress if available via NSMetadataQuery
                NSNumber *percent = nil;
                if (m_metadataQuery) {
                    NSMetadataQuery *query = (__bridge NSMetadataQuery *)m_metadataQuery;
                    [query disableUpdates];
                    for (NSUInteger i = 0; i < query.resultCount; i++) {
                        NSMetadataItem *item = [query resultAtIndex:i];
                        NSString *itemName = [item valueForAttribute:NSMetadataItemFSNameKey];
                        if ([itemName isEqualToString:filename.toNSString()]) {
                            percent = [item valueForAttribute:
                                NSMetadataUbiquitousItemPercentDownloadedKey];
                            break;
                        }
                    }
                    [query enableUpdates];
                }

                if (percent) {
                    qint64 received = static_cast<qint64>(percent.doubleValue);
                    QMetaObject::invokeMethod(this, [this, filename, received] {
                        emit downloadProgress(filename, received, 100);
                    }, Qt::QueuedConnection);
                }

                [NSThread sleepForTimeInterval:0.1];
            }

            if (downloaded) {
                QMetaObject::invokeMethod(this, [this, filename] {
                    emit downloadCompleted(filename, true, QString());
                }, Qt::QueuedConnection);
            } else {
                QMetaObject::invokeMethod(this, [this, filename,
                        reason = tr("Download timed out")] {
                    emit downloadCompleted(filename, false, reason);
                }, Qt::QueuedConnection);
            }
        }
    });
}
