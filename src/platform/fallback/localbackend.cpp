#include "localbackend.h"
#include "backupvalidation.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtConcurrent>

// Platform backends provide their own createPlatformBackend().
// The fallback is used only when no platform backend is compiled in.
#if !defined(Q_OS_WIN) && !defined(Q_OS_APPLE)
std::unique_ptr<CloudBackupBackend> createPlatformBackend()
{
    return std::make_unique<LocalBackend>();
}
#endif

static constexpr qint64 MaxMetaFileSize = 1024 * 1024; // 1 MB

void LocalBackend::detect()
{
    const int gen = ++m_detectionGeneration;
    const QString dir = backupDir();
    QPointer<LocalBackend> self(this);
    (void)QtConcurrent::run([self, gen, dir] {
        // Writability is checked without creating anything (detection is
        // side-effect-free). If the backup dir itself doesn't yet exist, walk
        // up the path to the first ancestor that does and check that one —
        // that's the directory mkpath would actually need to write into.
        QString probe = dir;
        while (!QFileInfo::exists(probe)) {
            const QString parent = QFileInfo(probe).absolutePath();
            if (parent == probe || parent.isEmpty())
                break;
            probe = parent;
        }
        const bool writable = QFileInfo::exists(probe) && QFileInfo(probe).isWritable();

        DetectedAccount a;
        a.id = { QtCloudBackup::StorageType::LocalDirectory, QString() };
        // displayName left empty — id.type uniquely identifies this row, the
        // consumer composes the label (e.g. "Local folder") itself. Same
        // convention as Apple iCloud. See DetectedAccount::displayName docs.
        if (writable) {
            a.status = QtCloudBackup::StorageStatus::Ready;
            a.statusDetail = LocalBackend::tr("Local storage is available");
        } else {
            a.status = QtCloudBackup::StorageStatus::Unavailable;
            a.statusDetail = LocalBackend::tr("Local backup directory is not writable");
        }

        QMetaObject::invokeMethod(qApp, [self, gen, a] {
            if (!self) return;
            self->applyDetectionResult({ a }, gen);
        }, Qt::QueuedConnection);
    });
}

void LocalBackend::select(const AccountId &id)
{
    if (id.type != QtCloudBackup::StorageType::LocalDirectory) {
        m_status = QtCloudBackup::StorageStatus::Unavailable;
        m_statusDetail = tr("Local backend supports only StorageType::LocalDirectory");
        emit statusChanged(m_status, m_statusDetail);
        return;
    }

    if (m_selectedId == id && m_status == QtCloudBackup::StorageStatus::LocalFallback)
        return; // reentrant no-op

    const int genAtStart = m_detectionGeneration;
    const QString dir = backupDir();
    QPointer<LocalBackend> self(this);
    (void)QtConcurrent::run([self, id, genAtStart, dir] {
        // Actual mkpath — the side effect deferred out of detect(). On
        // success the backend reports LocalFallback rather than Ready: the
        // consumer chose this explicitly (per spec, LocalFallback is never
        // assigned automatically as a last resort) and the status signals
        // "you are on local storage, not cloud sync".
        QDir d(dir);
        const bool ok = d.exists() || d.mkpath(QStringLiteral("."));

        QMetaObject::invokeMethod(qApp, [self, id, genAtStart, ok] {
            if (!self) return;
            // Staleness check: if a detect() ran while we were mkpathing,
            // only apply the result if the target is still Ready in the
            // latest detection. Otherwise the invalidation path has already
            // set correct state and we must not clobber it.
            if (self->m_detectionGeneration != genAtStart) {
                bool stillReady = false;
                for (const auto &a : self->m_lastDetection) {
                    if (a.id == id
                        && a.status == QtCloudBackup::StorageStatus::Ready) {
                        stillReady = true;
                        break;
                    }
                }
                if (!stillReady)
                    return;
            }
            if (ok) {
                self->m_selectedId = id;
                self->m_status = QtCloudBackup::StorageStatus::LocalFallback;
                self->m_statusDetail = LocalBackend::tr("Using local storage");
                emit self->statusChanged(self->m_status, self->m_statusDetail);
            } else {
                self->m_status = QtCloudBackup::StorageStatus::Unavailable;
                self->m_statusDetail = LocalBackend::tr("Failed to create backup directory");
                self->m_selectedId = {};
                emit self->statusChanged(self->m_status, self->m_statusDetail);
            }
        }, Qt::QueuedConnection);
    });
}

std::optional<AccountId> LocalBackend::resolveAccount(QtCloudBackup::StorageType type,
                                                     const QString & /*tenantId*/,
                                                     const QString & /*email*/) const
{
    if (type != QtCloudBackup::StorageType::LocalDirectory)
        return std::nullopt;
    // Single-instance platform. Resolution succeeds iff detect() saw a Ready
    // local entry.
    for (const auto &a : m_lastDetection) {
        if (a.id.type == QtCloudBackup::StorageType::LocalDirectory
            && a.status == QtCloudBackup::StorageStatus::Ready) {
            return a.id;
        }
    }
    return std::nullopt;
}

QtCloudBackup::StorageStatus LocalBackend::storageStatus() const
{
    return m_status;
}

QString LocalBackend::statusDetail() const
{
    return m_statusDetail;
}

QtCloudBackup::StorageType LocalBackend::storageType() const
{
    return QtCloudBackup::StorageType::LocalDirectory;
}

void LocalBackend::writeBackup(const QString &filename, const QByteArray &data,
                                const QJsonObject &meta)
{
    QString dir = backupDir();
    QPointer<LocalBackend> self(this);
    (void)QtConcurrent::run([self, dir, filename, data, meta] {
        QString bakPath = dir + QLatin1Char('/') + filename;
        QString metaPath = dir + QLatin1Char('/') + backupStem(filename) + QStringLiteral(".meta");

        // Write .bak file atomically
        {
            QSaveFile file(bakPath);
            if (!file.open(QIODevice::WriteOnly)) {
                QMetaObject::invokeMethod(qApp, [self, filename,
                        msg = LocalBackend::tr("Failed to open backup file for writing")] {
                    if (!self) return;
                    emit self->writeCompleted(filename,
                        int(QtCloudBackup::BackupError::IOError), msg);
                }, Qt::QueuedConnection);
                return;
            }
            file.write(data);
            if (!file.commit()) {
                QMetaObject::invokeMethod(qApp, [self, filename,
                        msg = LocalBackend::tr("Failed to write backup file")] {
                    if (!self) return;
                    emit self->writeCompleted(filename,
                        int(QtCloudBackup::BackupError::IOError), msg);
                }, Qt::QueuedConnection);
                return;
            }
        }

        // Write .meta file atomically. On failure, roll back the .bak so the
        // writer-local invariant (complete backup ⇔ both files exist) holds.
        {
            QSaveFile file(metaPath);
            if (!file.open(QIODevice::WriteOnly)) {
                QFile::remove(bakPath);
                QMetaObject::invokeMethod(qApp, [self, filename,
                        msg = LocalBackend::tr("Failed to open metadata file for writing")] {
                    if (!self) return;
                    emit self->writeCompleted(filename,
                        int(QtCloudBackup::BackupError::MetadataIOError), msg);
                }, Qt::QueuedConnection);
                return;
            }
            file.write(QJsonDocument(meta).toJson(QJsonDocument::Compact));
            if (!file.commit()) {
                QFile::remove(bakPath);
                QMetaObject::invokeMethod(qApp, [self, filename,
                        msg = LocalBackend::tr("Failed to write metadata file")] {
                    if (!self) return;
                    emit self->writeCompleted(filename,
                        int(QtCloudBackup::BackupError::MetadataIOError), msg);
                }, Qt::QueuedConnection);
                return;
            }
        }

        QMetaObject::invokeMethod(qApp, [self, filename] {
            if (!self) return;
            emit self->writeCompleted(filename,
                int(QtCloudBackup::BackupError::NoError), QString());
        }, Qt::QueuedConnection);
    });
}

void LocalBackend::readBackup(const QString &filename)
{
    QString dir = backupDir();
    QPointer<LocalBackend> self(this);
    (void)QtConcurrent::run([self, dir, filename] {
        QString bakPath = dir + QLatin1Char('/') + filename;
        QString metaPath = dir + QLatin1Char('/') + backupStem(filename) + QStringLiteral(".meta");

        QFile bakFile(bakPath);
        if (!bakFile.open(QIODevice::ReadOnly)) {
            QMetaObject::invokeMethod(qApp, [self, filename,
                    msg = LocalBackend::tr("Failed to open backup file for reading")] {
                if (!self) return;
                emit self->readCompleted(filename, {}, {},
                    int(QtCloudBackup::BackupError::IOError), msg);
            }, Qt::QueuedConnection);
            return;
        }
        QByteArray data = bakFile.readAll();

        QJsonObject meta;
        QFile metaFile(metaPath);
        if (metaFile.open(QIODevice::ReadOnly)) {
            meta = QJsonDocument::fromJson(metaFile.read(MaxMetaFileSize)).object();
        }

        QMetaObject::invokeMethod(qApp, [self, filename, data, meta] {
            if (!self) return;
            emit self->readCompleted(filename, data, meta,
                int(QtCloudBackup::BackupError::NoError), QString());
        }, Qt::QueuedConnection);
    });
}

void LocalBackend::deleteBackup(const QString &filename)
{
    QString dir = backupDir();
    QPointer<LocalBackend> self(this);
    (void)QtConcurrent::run([self, dir, filename] {
        QString bakPath = dir + QLatin1Char('/') + filename;
        QString metaPath = dir + QLatin1Char('/') + backupStem(filename) + QStringLiteral(".meta");

        // Delete .meta first (the completion marker), then .bak. Mirrors the
        // write protocol: .bak written first, .meta last. If interrupted after
        // .meta removal but before .bak removal, the .bak is left as an orphan
        // which the scanner surfaces with metadataAvailable=false rather than
        // an invisible orphan .meta.
        if (!QFile::remove(metaPath) && QFile::exists(metaPath))
            qWarning("Failed to remove metadata sidecar: %s", qPrintable(metaPath));
        bool ok = QFile::remove(bakPath);

        int err = ok ? int(QtCloudBackup::BackupError::NoError)
                     : int(QtCloudBackup::BackupError::IOError);
        QString msg = ok ? QString() : LocalBackend::tr("Failed to delete backup file");
        QMetaObject::invokeMethod(qApp, [self, filename, err, msg] {
            if (!self) return;
            emit self->deleteCompleted(filename, err, msg);
        }, Qt::QueuedConnection);
    });
}

void LocalBackend::scanBackups()
{
    QString dir = backupDir();
    QPointer<LocalBackend> self(this);
    (void)QtConcurrent::run([self, dir] {
        QDir d(dir);
        QStringList entries = d.entryList({QStringLiteral("qtcloudbackup_*.bak")},
                                          QDir::Files, QDir::Name);

        static const QRegularExpression re(
            QStringLiteral("^qtcloudbackup_([a-zA-Z0-9_-]{1,64})_(\\d{8}_\\d{6}_\\d{3})_[a-z0-9]{4}\\.bak$"));

        QList<BackupInfo> backups;
        for (const QString &entry : entries) {
            BackupInfo info;
            info.filename = entry;
            info.downloadState = QtCloudBackup::DownloadState::Local;

            // Try to read .meta sidecar (bounded). A missing .meta is not
            // junk — under cloud sync it may be syncing, evicted, or it may
            // be a stale orphan from an interrupted delete. Surface honestly
            // via metadataAvailable=false and let the consumer/retention
            // decide how to treat it.
            QString metaPath = dir + QLatin1Char('/') + backupStem(entry) + QStringLiteral(".meta");
            QFile metaFile(metaPath);
            if (metaFile.open(QIODevice::ReadOnly)) {
                QJsonObject meta = QJsonDocument::fromJson(metaFile.read(MaxMetaFileSize)).object();
                info.sourceId = meta[QStringLiteral("sourceId")].toString();
                info.timestamp = QDateTime::fromString(meta[QStringLiteral("timestamp")].toString(),
                                                        Qt::ISODateWithMs);
                info.metadata = meta[QStringLiteral("metadata")].toObject().toVariantMap();
            } else {
                info.metadataAvailable = false;
            }

            // Fallback: parse filename (always needed when .meta is missing,
            // and a safety net when .meta is malformed)
            if (info.sourceId.isEmpty() || !info.timestamp.isValid()) {
                auto match = re.match(entry);
                if (match.hasMatch()) {
                    info.sourceId = match.captured(1);
                    info.timestamp = QDateTime::fromString(match.captured(2),
                                                            QStringLiteral("yyyyMMdd_HHmmss_zzz"));
                    info.timestamp.setTimeZone(QTimeZone::utc());
                }
            }

            backups.append(info);
        }

        QMetaObject::invokeMethod(qApp, [self, backups] {
            if (!self) return;
            emit self->scanCompleted(backups);
        }, Qt::QueuedConnection);
    });
}

void LocalBackend::triggerDownload(const QString &filename)
{
    // Local files are always available — emit success immediately
    QMetaObject::invokeMethod(this, [this, filename] {
        emit downloadCompleted(filename,
            int(QtCloudBackup::BackupError::NoError), QString());
    }, Qt::QueuedConnection);
}

void LocalBackend::scanOrphanedBackups()
{
    // Nothing below local — no orphans possible
    emit orphanScanCompleted({});
}

void LocalBackend::migrateOrphanedBackups(const QList<OrphanedBackupInfo> &)
{
    // Nothing to migrate
    emit migrationCompleted(0, int(QtCloudBackup::BackupError::NoError), QString());
}

QString LocalBackend::backupDir() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
           + QStringLiteral("/Backups");
}

void LocalBackend::applyDetectionResult(QList<DetectedAccount> accounts, int gen)
{
    // Staleness gate: a fresher detect() has already started; drop this
    // stale completion to avoid clobbering m_lastDetection / re-emitting
    // outdated status.
    if (gen != m_detectionGeneration)
        return;

    m_lastDetection = accounts;

    // Selection-invalidation: if a previously-selected account is no longer
    // Ready in the new detection result, tear down the active state and
    // emit statusChanged BEFORE accountsDetected so the consumer sees
    // "your storage dropped" before "here are the current options". Same
    // pattern as the Apple and Windows backends; for single-account Local
    // this only fires when the local path becomes non-writable between
    // detect() calls.
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
            m_status = selectedNow ? selectedNow->status
                                   : QtCloudBackup::StorageStatus::Unavailable;
            m_statusDetail = selectedNow ? selectedNow->statusDetail
                                         : tr("Local directory is no longer detected");
            m_selectedId = {};
            emit statusChanged(m_status, m_statusDetail);
        }
    }

    emit accountsDetected(m_lastDetection);
}
