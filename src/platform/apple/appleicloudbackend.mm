#include "appleicloudbackend.h"
#include "backupvalidation.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QtConcurrent>

#import <Foundation/Foundation.h>

#ifndef QTCLOUDBACKUP_ICLOUD_CONTAINER
#define QTCLOUDBACKUP_ICLOUD_CONTAINER ""
#endif

static constexpr qint64 MaxMetaFileSize = 1024 * 1024; // 1 MB

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
    // Transitional shim during issue #4 — wires the legacy single-call
    // initialise() onto the new detect()/select() flow. Removed in phase 6
    // when CloudBackupManager switches to driving detect/select directly.
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(this, &CloudBackupBackend::accountsDetected, this,
        [this, conn](const QList<DetectedAccount> &accounts) {
            QObject::disconnect(*conn);
            if (accounts.isEmpty()) return;
            const auto &a = accounts.first();
            if (a.status == QtCloudBackup::StorageStatus::Ready) {
                select(a.id);
            } else {
                m_status = a.status;
                m_statusDetail = a.statusDetail;
                stopMetadataQuery();
                emit statusChanged(m_status, m_statusDetail);
            }
        });
    detect();
}

void AppleICloudBackend::detect()
{
    // Install the NSUbiquityIdentityDidChangeNotification observer once.
    // Re-detection runs on identity change (sign in/out, iCloud Drive
    // toggle), so the consumer's status stays current without polling.
    if (!m_notificationObserver) {
        QPointer<AppleICloudBackend> self(this);
        id observer = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSUbiquityIdentityDidChangeNotification
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification *) {
                        if (self)
                            self->detect();
                    }];
        m_notificationObserver = (void *)CFBridgingRetain(observer);
    }

    const int gen = ++m_detectionGeneration;

    // Stage 1 step 1: ubiquity identity token, on main thread (lightweight).
    id token = [[NSFileManager defaultManager] ubiquityIdentityToken];
    if (!token) {
        DetectedAccount a;
        a.id = { QtCloudBackup::StorageType::ICloud, QString() };
        // displayName is left empty: id.type uniquely identifies this as
        // iCloud, so the consumer composes the full label (e.g.
        // "iCloud Drive") itself. See DetectedAccount::displayName docs.
        a.status = QtCloudBackup::StorageStatus::Unavailable;
        a.statusDetail = tr("iCloud not signed in or iCloud Drive disabled");
        m_pendingContainerRoot.clear();
        applyDetectionResult({ a }, gen);
        return;
    }

    // Stage 1 step 2: container URL resolution off main — per Apple docs,
    // URLForUbiquityContainerIdentifier: can block on first call.
    QPointer<AppleICloudBackend> self(this);
    (void)QtConcurrent::run([self, gen] {
        @autoreleasepool {
            NSURL *url = [[NSFileManager defaultManager]
                URLForUbiquityContainerIdentifier:containerIdentifier()];

            QString rootPath = url ? QString::fromNSString(url.path) : QString();

            QMetaObject::invokeMethod(qApp, [self, gen, rootPath] {
                if (!self) return;
                DetectedAccount a;
                a.id = { QtCloudBackup::StorageType::ICloud, QString() };
                // displayName intentionally empty — see other branch.
                if (rootPath.isEmpty()) {
                    self->m_pendingContainerRoot.clear();
                    a.status = QtCloudBackup::StorageStatus::Disabled;
                    a.statusDetail = AppleICloudBackend::tr(
                        "iCloud container is not available — check entitlements and provisioning profile");
                } else {
                    self->m_pendingContainerRoot = QUrl::fromLocalFile(rootPath);
                    a.status = QtCloudBackup::StorageStatus::Ready;
                    a.statusDetail = AppleICloudBackend::tr("iCloud storage is available");
                }
                self->applyDetectionResult({ a }, gen);
            }, Qt::QueuedConnection);
        }
    });
}

void AppleICloudBackend::select(const AccountId &id)
{
    if (id.type != QtCloudBackup::StorageType::ICloud) {
        m_status = QtCloudBackup::StorageStatus::Unavailable;
        m_statusDetail = tr("Apple build supports only StorageType::ICloud");
        emit statusChanged(m_status, m_statusDetail);
        return;
    }
    if (m_pendingContainerRoot.isEmpty()) {
        m_status = QtCloudBackup::StorageStatus::Unavailable;
        m_statusDetail = tr("iCloud container not yet resolved — call detect() first");
        emit statusChanged(m_status, m_statusDetail);
        return;
    }

    if (m_selectedId == id && m_status == QtCloudBackup::StorageStatus::Ready)
        return; // reentrant no-op

    // Freeze the detection generation at the start of directory creation.
    // If a detect() runs while we're mid-flight and concludes the target
    // is no longer Ready, applyDetectionResult() will have already
    // invalidated the selection; our completion handler then refuses to
    // clobber that with a stale Ready.
    const int genAtStart = m_detectionGeneration;
    const QString rootPath = m_pendingContainerRoot.toLocalFile();
    QPointer<AppleICloudBackend> self(this);
    (void)QtConcurrent::run([self, id, genAtStart, rootPath] {
        @autoreleasepool {
            NSURL *rootUrl = [NSURL fileURLWithPath:rootPath.toNSString()];
            NSURL *backupsUrl = [rootUrl URLByAppendingPathComponent:@"Backups" isDirectory:YES];
            NSError *error = nil;
            BOOL ok = [[NSFileManager defaultManager] createDirectoryAtURL:backupsUrl
                                               withIntermediateDirectories:YES
                                                                attributes:nil
                                                                     error:&error];
            QString backupsPath = QString::fromNSString(backupsUrl.path);
            QString errMsg = ok ? QString()
                                : QString::fromNSString(error.localizedDescription ?: @"");

            QMetaObject::invokeMethod(qApp, [self, id, genAtStart, ok, backupsPath, errMsg] {
                if (!self) return;
                // If a detect() ran while we were creating the Backups
                // directory, only apply the result if the target account is
                // still Ready in the latest detection. Otherwise the
                // invalidation path has already set the correct state and
                // we must not clobber it.
                if (self->m_detectionGeneration != genAtStart) {
                    bool stillReady = false;
                    for (const auto &a : self->m_lastDetection) {
                        if (a.id == id && a.status == QtCloudBackup::StorageStatus::Ready) {
                            stillReady = true;
                            break;
                        }
                    }
                    if (!stillReady)
                        return;
                }
                if (ok) {
                    self->m_containerUrl = QUrl::fromLocalFile(backupsPath);
                    self->m_selectedId = id;
                    self->m_status = QtCloudBackup::StorageStatus::Ready;
                    self->m_statusDetail = AppleICloudBackend::tr("iCloud storage is available");
                    emit self->statusChanged(self->m_status, self->m_statusDetail);
                    self->startMetadataQuery();
                } else {
                    self->m_status = QtCloudBackup::StorageStatus::Unavailable;
                    self->m_statusDetail = AppleICloudBackend::tr(
                        "Failed to create Backups directory: %1").arg(errMsg);
                    self->m_containerUrl.clear();
                    self->m_selectedId = {};
                    self->stopMetadataQuery();
                    emit self->statusChanged(self->m_status, self->m_statusDetail);
                }
            }, Qt::QueuedConnection);
        }
    });
}

std::optional<AccountId> AppleICloudBackend::resolveAccount(QtCloudBackup::StorageType type,
                                                            const QString & /*tenantId*/,
                                                            const QString & /*email*/) const
{
    if (type != QtCloudBackup::StorageType::ICloud)
        return std::nullopt;
    // Apple: single-instance platform. Resolution succeeds iff detect() saw
    // a Ready iCloud account — i.e. token present AND container URL resolved.
    for (const auto &a : m_lastDetection) {
        if (a.id.type == QtCloudBackup::StorageType::ICloud
            && a.status == QtCloudBackup::StorageStatus::Ready) {
            return a.id;
        }
    }
    return std::nullopt;
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

void AppleICloudBackend::writeBackup(const QString &filename, const QByteArray &data,
                                      const QJsonObject &meta)
{
    QString dir = backupDir();
    QPointer<AppleICloudBackend> self(this);
    (void)QtConcurrent::run([self, dir, filename, data, meta] {
        @autoreleasepool {
            QString bakPath = dir + QLatin1Char('/') + filename;
            QString metaPath = dir + QLatin1Char('/') + backupStem(filename) + QStringLiteral(".meta");

            NSURL *bakUrl = [NSURL fileURLWithPath:bakPath.toNSString()];
            NSURL *metaUrl = [NSURL fileURLWithPath:metaPath.toNSString()];

            // Coordinated write for .bak
            NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
            __block bool success = true;
            __block int errorCode = int(QtCloudBackup::BackupError::NoError);
            __block QString errorMsg;

            NSError *coordError = nil;
            [coordinator coordinateWritingItemAtURL:bakUrl
                                           options:NSFileCoordinatorWritingForReplacing
                                             error:&coordError
                                        byAccessor:^(NSURL *newURL) {
                NSData *nsData = [NSData dataWithBytes:data.constData()
                                                length:static_cast<NSUInteger>(data.size())];
                NSError *writeError = nil;
                if (![nsData writeToURL:newURL options:NSDataWritingAtomic error:&writeError]) {
                    success = false;
                    errorCode = int(QtCloudBackup::BackupError::IOError);
                    errorMsg = AppleICloudBackend::tr("Failed to write backup file: %1")
                        .arg(QString::fromNSString(writeError.localizedDescription));
                }
            }];

            if (coordError) {
                success = false;
                errorCode = int(QtCloudBackup::BackupError::CoordinationFailed);
                errorMsg = AppleICloudBackend::tr("File coordination failed for backup: %1")
                    .arg(QString::fromNSString(coordError.localizedDescription));
            }

            if (!success) {
                int err = errorCode;
                QString msg = errorMsg;
                QMetaObject::invokeMethod(qApp, [self, filename, err, msg] {
                    if (!self) return;
                    emit self->writeCompleted(filename, err, msg);
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
                NSData *nsData = [NSData dataWithBytes:metaJson.constData()
                                                length:static_cast<NSUInteger>(metaJson.size())];
                NSError *writeError = nil;
                if (![nsData writeToURL:newURL options:NSDataWritingAtomic error:&writeError]) {
                    success = false;
                    errorCode = int(QtCloudBackup::BackupError::MetadataIOError);
                    errorMsg = AppleICloudBackend::tr("Failed to write metadata file: %1")
                        .arg(QString::fromNSString(writeError.localizedDescription));
                }
            }];

            if (metaCoordError) {
                success = false;
                errorCode = int(QtCloudBackup::BackupError::CoordinationFailed);
                errorMsg = AppleICloudBackend::tr("File coordination failed for metadata: %1")
                    .arg(QString::fromNSString(metaCoordError.localizedDescription));
            }

            if (!success) {
                // Remove the .bak file since meta failed
                [[NSFileManager defaultManager] removeItemAtURL:bakUrl error:nil];
                int err = errorCode;
                QString msg = errorMsg;
                QMetaObject::invokeMethod(qApp, [self, filename, err, msg] {
                    if (!self) return;
                    emit self->writeCompleted(filename, err, msg);
                }, Qt::QueuedConnection);
                return;
            }

            QMetaObject::invokeMethod(qApp, [self, filename] {
                if (!self) return;
                emit self->writeCompleted(filename,
                    int(QtCloudBackup::BackupError::NoError), QString());
            }, Qt::QueuedConnection);
        }
    });
}

void AppleICloudBackend::readBackup(const QString &filename)
{
    QString dir = backupDir();
    QPointer<AppleICloudBackend> self(this);
    (void)QtConcurrent::run([self, dir, filename] {
        @autoreleasepool {
            QString bakPath = dir + QLatin1Char('/') + filename;
            QString metaPath = dir + QLatin1Char('/') + backupStem(filename) + QStringLiteral(".meta");

            NSURL *bakUrl = [NSURL fileURLWithPath:bakPath.toNSString()];

            // Check if file needs downloading first
            NSString *downloadStatus = nil;
            [bakUrl getResourceValue:&downloadStatus
                              forKey:NSURLUbiquitousItemDownloadingStatusKey
                               error:nil];
            if ([downloadStatus isEqual:NSURLUbiquitousItemDownloadingStatusNotDownloaded]) {
                // Trigger download and report — caller should wait for downloadReady
                NSError *dlError = nil;
                [[NSFileManager defaultManager] startDownloadingUbiquitousItemAtURL:bakUrl
                                                                             error:&dlError];
                if (dlError) {
                    QMetaObject::invokeMethod(qApp, [self, filename,
                            msg = AppleICloudBackend::tr("Failed to start download: %1")
                                .arg(QString::fromNSString(dlError.localizedDescription))] {
                        if (!self) return;
                        emit self->readCompleted(filename, {}, {},
                            int(QtCloudBackup::BackupError::DownloadError), msg);
                    }, Qt::QueuedConnection);
                } else {
                    QMetaObject::invokeMethod(qApp, [self, filename,
                            msg = AppleICloudBackend::tr(
                                "File is cloud-only — download triggered, retry after downloadReady")] {
                        if (!self) return;
                        emit self->readCompleted(filename, {}, {},
                            int(QtCloudBackup::BackupError::FileNotLocal), msg);
                    }, Qt::QueuedConnection);
                }
                return;
            }

            // Coordinated read
            NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
            __block QByteArray data;
            __block bool success = true;
            __block int errorCode = int(QtCloudBackup::BackupError::NoError);
            __block QString errorMsg;

            NSError *coordError = nil;
            [coordinator coordinateReadingItemAtURL:bakUrl
                                           options:0
                                             error:&coordError
                                        byAccessor:^(NSURL *newURL) {
                NSData *nsData = [NSData dataWithContentsOfURL:newURL];
                if (nsData) {
                    data = QByteArray(reinterpret_cast<const char *>(nsData.bytes),
                                     static_cast<qsizetype>(nsData.length));
                } else {
                    success = false;
                    errorCode = int(QtCloudBackup::BackupError::IOError);
                    errorMsg = AppleICloudBackend::tr("Failed to read backup file");
                }
            }];

            if (coordError) {
                success = false;
                errorCode = int(QtCloudBackup::BackupError::CoordinationFailed);
                errorMsg = AppleICloudBackend::tr("File coordination failed for read: %1")
                    .arg(QString::fromNSString(coordError.localizedDescription));
            }

            if (!success) {
                int err = errorCode;
                QString msg = errorMsg;
                QMetaObject::invokeMethod(qApp, [self, filename, err, msg] {
                    if (!self) return;
                    emit self->readCompleted(filename, {}, {}, err, msg);
                }, Qt::QueuedConnection);
                return;
            }

            // Read metadata (best effort, bounded read)
            QJsonObject meta;
            NSURL *metaUrl = [NSURL fileURLWithPath:metaPath.toNSString()];
            NSData *metaData = [NSData dataWithContentsOfURL:metaUrl];
            if (metaData && static_cast<qint64>(metaData.length) <= MaxMetaFileSize) {
                QByteArray metaBytes(reinterpret_cast<const char *>(metaData.bytes),
                                     static_cast<qsizetype>(metaData.length));
                meta = QJsonDocument::fromJson(metaBytes).object();
            }

            QByteArray readData = data;
            QMetaObject::invokeMethod(qApp, [self, filename, readData, meta] {
                if (!self) return;
                emit self->readCompleted(filename, readData, meta,
                    int(QtCloudBackup::BackupError::NoError), QString());
            }, Qt::QueuedConnection);
        }
    });
}

void AppleICloudBackend::deleteBackup(const QString &filename)
{
    QString dir = backupDir();
    QPointer<AppleICloudBackend> self(this);
    (void)QtConcurrent::run([self, dir, filename] {
        @autoreleasepool {
            QString bakPath = dir + QLatin1Char('/') + filename;
            QString metaPath = dir + QLatin1Char('/') + backupStem(filename) + QStringLiteral(".meta");

            NSURL *bakUrl = [NSURL fileURLWithPath:bakPath.toNSString()];
            NSURL *metaUrl = [NSURL fileURLWithPath:metaPath.toNSString()];

            NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
            __block bool success = true;
            __block int errorCode = int(QtCloudBackup::BackupError::NoError);
            __block QString errorMsg;

            // Delete .meta first (the completion marker), then .bak. Mirrors the
            // write protocol (.bak first, .meta last) and ensures that an
            // interruption between the two removals leaves an orphan .bak that
            // the scanner surfaces with metadataAvailable=false, rather than an
            // invisible orphan .meta.
            NSError *metaRemoveError = nil;
            if (![[NSFileManager defaultManager] removeItemAtURL:metaUrl error:&metaRemoveError]
                && metaRemoveError.code != NSFileNoSuchFileError) {
                qWarning("Failed to remove metadata sidecar: %s",
                         qPrintable(QString::fromNSString(metaRemoveError.localizedDescription)));
            }

            NSError *coordError = nil;
            [coordinator coordinateWritingItemAtURL:bakUrl
                                           options:NSFileCoordinatorWritingForDeleting
                                             error:&coordError
                                        byAccessor:^(NSURL *newURL) {
                NSError *removeError = nil;
                if (![[NSFileManager defaultManager] removeItemAtURL:newURL error:&removeError]) {
                    success = false;
                    errorCode = int(QtCloudBackup::BackupError::IOError);
                    errorMsg = AppleICloudBackend::tr("Failed to delete backup file: %1")
                        .arg(QString::fromNSString(removeError.localizedDescription));
                }
            }];

            if (coordError) {
                success = false;
                errorCode = int(QtCloudBackup::BackupError::CoordinationFailed);
                errorMsg = AppleICloudBackend::tr("File coordination failed for delete: %1")
                    .arg(QString::fromNSString(coordError.localizedDescription));
            }

            // Resolve any file version conflicts
            NSArray *conflicts = [NSFileVersion unresolvedConflictVersionsOfItemAtURL:bakUrl];
            for (NSFileVersion *version in conflicts) {
                version.resolved = YES;
                [version removeAndReturnError:nil];
            }

            int err = errorCode;
            QString msg = errorMsg;
            QMetaObject::invokeMethod(qApp, [self, filename, err, msg] {
                if (!self) return;
                emit self->deleteCompleted(filename, err, msg);
            }, Qt::QueuedConnection);
        }
    });
}

void AppleICloudBackend::scanBackups()
{
    QString dir = backupDir();
    QPointer<AppleICloudBackend> self(this);
    auto queryGuard = m_queryGuard; // shared_ptr copy for thread safety
    (void)QtConcurrent::run([self, dir, queryGuard] {
        @autoreleasepool {
            QList<BackupInfo> backups;

            // Use QDir for local files
            QDir d(dir);
            QStringList entries = d.entryList({QStringLiteral("qtcloudbackup_*.bak")},
                                              QDir::Files, QDir::Name);

            static const QRegularExpression re(
                QStringLiteral("^qtcloudbackup_([a-zA-Z0-9_-]{1,64})_(\\d{8}_\\d{6}_\\d{3})_[a-z0-9]{4}\\.bak$"));

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

                // Try to read .meta sidecar (bounded). A missing .meta is
                // not junk under cloud sync — it may be syncing, evicted, or
                // a stale orphan. Surface honestly via metadataAvailable=false.
                QString metaPath = dir + QLatin1Char('/') + backupStem(entry) + QStringLiteral(".meta");
                QFile metaFile(metaPath);
                if (metaFile.open(QIODevice::ReadOnly)) {
                    QJsonObject meta = QJsonDocument::fromJson(metaFile.read(MaxMetaFileSize)).object();
                    info.sourceId = meta[QStringLiteral("sourceId")].toString();
                    info.timestamp = QDateTime::fromString(
                        meta[QStringLiteral("timestamp")].toString(), Qt::ISODateWithMs);
                    info.metadata = meta[QStringLiteral("metadata")].toObject().toVariantMap();
                } else {
                    info.metadataAvailable = false;
                }

                // Fallback: parse filename (always needed when .meta is
                // missing, and a safety net when .meta is malformed)
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

            // Also query NSMetadataQuery for cloud-only files not in directory listing.
            // Access via shared queryGuard — safe even if the backend is destroyed.
            {
                QMutexLocker locker(&queryGuard->mutex);
                if (queryGuard->query) {
                    NSMetadataQuery *query = (__bridge NSMetadataQuery *)queryGuard->query;
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
                        // Cloud-only: .bak isn't on local disk, so we haven't
                        // read its .meta sidecar either. Mark as unconfirmed
                        // so retention excludes from prune candidates until
                        // a subsequent scan after hydration.
                        info.metadataAvailable = false;

                        // Check download percentage
                        NSNumber *percent = [item valueForAttribute:
                            NSMetadataUbiquitousItemPercentDownloadedKey];
                        if (percent && percent.doubleValue > 0 && percent.doubleValue < 100) {
                            info.downloadState = QtCloudBackup::DownloadState::Downloading;
                        }

                        // Parse filename for sourceId + timestamp
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
            }

            QMetaObject::invokeMethod(qApp, [self, backups] {
                if (!self) return;
                emit self->scanCompleted(backups);
            }, Qt::QueuedConnection);
        }
    });
}

void AppleICloudBackend::triggerDownload(const QString &filename)
{
    QString dir = backupDir();
    QPointer<AppleICloudBackend> self(this);
    auto queryGuard = m_queryGuard; // shared_ptr copy for thread safety
    (void)QtConcurrent::run([self, dir, filename, queryGuard] {
        @autoreleasepool {
            QString fullPath = dir + QLatin1Char('/') + filename;
            NSURL *fileUrl = [NSURL fileURLWithPath:fullPath.toNSString()];

            NSError *error = nil;
            if (![[NSFileManager defaultManager] startDownloadingUbiquitousItemAtURL:fileUrl
                                                                               error:&error]) {
                QString msg = error
                    ? AppleICloudBackend::tr("Failed to start download: %1")
                          .arg(QString::fromNSString(error.localizedDescription))
                    : AppleICloudBackend::tr("Failed to start download");
                QMetaObject::invokeMethod(qApp, [self, filename, msg] {
                    if (!self) return;
                    emit self->downloadCompleted(filename,
                        int(QtCloudBackup::BackupError::DownloadError), msg);
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

                // Report progress if available via NSMetadataQuery.
                // Access via shared queryGuard — safe even if backend is destroyed.
                NSNumber *percent = nil;
                {
                    QMutexLocker locker(&queryGuard->mutex);
                    if (queryGuard->query) {
                        NSMetadataQuery *query = (__bridge NSMetadataQuery *)queryGuard->query;
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
                }

                if (percent) {
                    qint64 received = static_cast<qint64>(percent.doubleValue);
                    QMetaObject::invokeMethod(qApp, [self, filename, received] {
                        if (!self) return;
                        emit self->downloadProgress(filename, received, 100);
                    }, Qt::QueuedConnection);
                }

                [NSThread sleepForTimeInterval:0.1];
            }

            if (downloaded) {
                QMetaObject::invokeMethod(qApp, [self, filename] {
                    if (!self) return;
                    emit self->downloadCompleted(filename,
                        int(QtCloudBackup::BackupError::NoError), QString());
                }, Qt::QueuedConnection);
            } else {
                QMetaObject::invokeMethod(qApp, [self, filename,
                        msg = AppleICloudBackend::tr("Download timed out")] {
                    if (!self) return;
                    emit self->downloadCompleted(filename,
                        int(QtCloudBackup::BackupError::DownloadTimeout), msg);
                }, Qt::QueuedConnection);
            }
        }
    });
}

// --- Orphaned backup detection and migration ---

void AppleICloudBackend::scanOrphanedBackups()
{
    QString fallbackDir = localFallbackDir();
    QPointer<AppleICloudBackend> self(this);
    (void)QtConcurrent::run([self, fallbackDir] {
        @autoreleasepool {
            QList<OrphanedBackupInfo> orphans;

            QDir d(fallbackDir);
            if (!d.exists()) {
                QMetaObject::invokeMethod(qApp, [self, orphans] {
                    if (!self) return;
                    emit self->orphanScanCompleted(orphans);
                }, Qt::QueuedConnection);
                return;
            }

            QStringList entries = d.entryList({QStringLiteral("qtcloudbackup_*.bak")},
                                              QDir::Files, QDir::Name);

            static const QRegularExpression re(
                QStringLiteral("^qtcloudbackup_([a-zA-Z0-9_-]{1,64})_(\\d{8}_\\d{6}_\\d{3})_[a-z0-9]{4}\\.bak$"));

            for (const QString &entry : entries) {
                OrphanedBackupInfo info;
                info.filename = entry;
                info.originStorageType = QtCloudBackup::StorageType::LocalDirectory;
                info.originPath = fallbackDir;

                // Try to read .meta sidecar
                QString metaPath = fallbackDir + QLatin1Char('/') + backupStem(entry)
                                   + QStringLiteral(".meta");
                QFile metaFile(metaPath);
                if (metaFile.open(QIODevice::ReadOnly)) {
                    QJsonObject meta = QJsonDocument::fromJson(metaFile.read(MaxMetaFileSize)).object();
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

                orphans.append(info);
            }

            QMetaObject::invokeMethod(qApp, [self, orphans] {
                if (!self) return;
                emit self->orphanScanCompleted(orphans);
            }, Qt::QueuedConnection);
        }
    });
}

void AppleICloudBackend::migrateOrphanedBackups(const QList<OrphanedBackupInfo> &orphans)
{
    QString destDir = backupDir();
    QPointer<AppleICloudBackend> self(this);
    (void)QtConcurrent::run([self, destDir, orphans] {
        @autoreleasepool {
            int total = orphans.size();
            int migrated = 0;
            bool allSucceeded = true;

            auto coordinatedWrite = [](const QString &path, const QByteArray &data) -> bool {
                NSURL *url = [NSURL fileURLWithPath:path.toNSString()];
                NSFileCoordinator *coordinator =
                    [[NSFileCoordinator alloc] initWithFilePresenter:nil];
                __block bool ok = true;

                NSError *coordError = nil;
                [coordinator coordinateWritingItemAtURL:url
                                               options:NSFileCoordinatorWritingForReplacing
                                                 error:&coordError
                                            byAccessor:^(NSURL *newURL) {
                    NSData *nsData = [NSData dataWithBytes:data.constData()
                                                    length:static_cast<NSUInteger>(data.size())];
                    NSError *err = nil;
                    if (![nsData writeToURL:newURL options:NSDataWritingAtomic error:&err])
                        ok = false;
                }];

                if (coordError)
                    ok = false;
                return ok;
            };

            for (int i = 0; i < total; ++i) {
                const auto &orphan = orphans[i];
                QString srcBak = orphan.originPath + QLatin1Char('/') + orphan.filename;
                QString srcMeta = orphan.originPath + QLatin1Char('/')
                                  + backupStem(orphan.filename) + QStringLiteral(".meta");
                QString destBak = destDir + QLatin1Char('/') + orphan.filename;
                QString destMeta = destDir + QLatin1Char('/')
                                   + backupStem(orphan.filename) + QStringLiteral(".meta");

                // Skip duplicates
                if (QFile::exists(destBak)) {
                    ++migrated;
                    QMetaObject::invokeMethod(qApp, [self, migrated, total] {
                        if (!self) return;
                        emit self->migrationProgress(migrated, total);
                    }, Qt::QueuedConnection);
                    continue;
                }

                // Read source .bak
                QFile bakFile(srcBak);
                if (!bakFile.open(QIODevice::ReadOnly)) {
                    allSucceeded = false;
                    continue;
                }
                QByteArray bakData = bakFile.readAll();
                bakFile.close();

                // Coordinated write .bak to iCloud
                if (!coordinatedWrite(destBak, bakData)) {
                    allSucceeded = false;
                    continue;
                }

                // Read and write .meta (best effort)
                QFile metaFile(srcMeta);
                if (metaFile.open(QIODevice::ReadOnly)) {
                    QByteArray metaData = metaFile.read(MaxMetaFileSize);
                    metaFile.close();
                    coordinatedWrite(destMeta, metaData);
                }

                // Delete originals
                QFile::remove(srcBak);
                QFile::remove(srcMeta);

                ++migrated;
                QMetaObject::invokeMethod(qApp, [self, migrated, total] {
                    if (!self) return;
                    emit self->migrationProgress(migrated, total);
                }, Qt::QueuedConnection);
            }

            int count = migrated;
            int err = allSucceeded ? int(QtCloudBackup::BackupError::NoError)
                                   : int(QtCloudBackup::BackupError::MigrationPartial);
            QString msg = allSucceeded ? QString()
                : AppleICloudBackend::tr("Some files could not be migrated");
            QMetaObject::invokeMethod(qApp, [self, count, err, msg] {
                if (!self) return;
                emit self->migrationCompleted(count, err, msg);
            }, Qt::QueuedConnection);
        }
    });
}

QString AppleICloudBackend::backupDir() const
{
    return m_containerUrl.toLocalFile();
}

QString AppleICloudBackend::localFallbackDir() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
           + QStringLiteral("/Backups");
}

// --- NSMetadataQuery for file discovery and remote change detection ---

void AppleICloudBackend::startMetadataQuery()
{
    QMutexLocker locker(&m_queryGuard->mutex);
    if (m_queryGuard->query)
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
        m_queryGuard->query = (void *)CFBridgingRetain(query);
    }
}

void AppleICloudBackend::stopMetadataQuery()
{
    QMutexLocker locker(&m_queryGuard->mutex);
    if (!m_queryGuard->query)
        return;

    @autoreleasepool {
        NSMetadataQuery *query = (__bridge NSMetadataQuery *)m_queryGuard->query;
        [query stopQuery];
        [[NSNotificationCenter defaultCenter] removeObserver:query];
        CFRelease(m_queryGuard->query);
        m_queryGuard->query = nullptr;
    }
}

void AppleICloudBackend::handleQueryResults()
{
    // Called from NSMetadataQuery notifications on the main thread.
    // Access the query's results array (safe) rather than notification
    // userInfo items (proxy objects that can be deallocated).
    QMutexLocker locker(&m_queryGuard->mutex);
    if (!m_queryGuard->query)
        return;

    @autoreleasepool {
        NSMetadataQuery *query = (__bridge NSMetadataQuery *)m_queryGuard->query;
        [query disableUpdates];

        static const QRegularExpression re(
            QStringLiteral("^qtcloudbackup_([a-zA-Z0-9_-]{1,64})_(\\d{8}_\\d{6}_\\d{3})_[a-z0-9]{4}\\.bak$"));

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
        locker.unlock();

        for (const QString &sourceId : sourceIds)
            emit remoteChangeDetected(sourceId);
    }
}

void AppleICloudBackend::applyDetectionResult(QList<DetectedAccount> accounts, int gen)
{
    // Generation gate: a fresher detect() has already started; drop this
    // stale completion to avoid clobbering m_lastDetection / re-emitting
    // outdated status.
    if (gen != m_detectionGeneration)
        return;

    m_lastDetection = accounts;

    // Selection-invalidation: if a previously-selected account is no longer
    // Ready in the new detection result, tear down the active state and
    // emit statusChanged so the consumer learns the active target is gone.
    // Issued BEFORE accountsDetected so the consumer sees "your storage
    // dropped" before "here are the current options".
    if (m_selectedId.type != QtCloudBackup::StorageType::None) {
        const DetectedAccount *selectedNow = nullptr;
        for (const auto &a : accounts) {
            if (a.id == m_selectedId) {
                selectedNow = &a;
                break;
            }
        }
        const bool stillReady = selectedNow
            && selectedNow->status == QtCloudBackup::StorageStatus::Ready;
        if (!stillReady) {
            stopMetadataQuery();
            m_containerUrl.clear();
            m_status = selectedNow ? selectedNow->status
                                   : QtCloudBackup::StorageStatus::Unavailable;
            m_statusDetail = selectedNow ? selectedNow->statusDetail
                                         : tr("Previously-selected iCloud account is no longer detected");
            m_selectedId = {};
            emit statusChanged(m_status, m_statusDetail);
        }
    }

    emit accountsDetected(m_lastDetection);
}
