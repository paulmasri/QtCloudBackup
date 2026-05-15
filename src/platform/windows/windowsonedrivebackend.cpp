#include "windowsonedrivebackend.h"
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
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QtConcurrent>

#include <qt_windows.h>
#include <shlobj.h>

#ifndef QTCLOUDBACKUP_WINDOWS_BACKUP_PATH
#define QTCLOUDBACKUP_WINDOWS_BACKUP_PATH ""
#endif

static constexpr qint64 MaxMetaFileSize = 1024 * 1024; // 1 MB

struct OneDriveCandidate {
    QString path;
    QtCloudBackup::StorageType type;
};

static QList<OneDriveCandidate> detectOneDriveCandidates()
{
    QList<OneDriveCandidate> candidates;

    // 1. %OneDriveCommercial%
    QString val = qEnvironmentVariable("OneDriveCommercial");
    if (!val.isEmpty())
        candidates.append({val, QtCloudBackup::StorageType::OneDriveCommercial});

    // 2. Registry Business1
    {
        QSettings reg(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\OneDrive\\Accounts\\Business1"),
                       QSettings::NativeFormat);
        val = reg.value(QStringLiteral("UserFolder")).toString();
        if (!val.isEmpty())
            candidates.append({val, QtCloudBackup::StorageType::OneDriveCommercial});
    }

    // 3. %OneDriveConsumer%
    val = qEnvironmentVariable("OneDriveConsumer");
    if (!val.isEmpty())
        candidates.append({val, QtCloudBackup::StorageType::OneDrivePersonal});

    // 4. Registry Personal
    {
        QSettings reg(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\OneDrive\\Accounts\\Personal"),
                       QSettings::NativeFormat);
        val = reg.value(QStringLiteral("UserFolder")).toString();
        if (!val.isEmpty())
            candidates.append({val, QtCloudBackup::StorageType::OneDrivePersonal});
    }

    // 5. %OneDrive% (assume personal)
    val = qEnvironmentVariable("OneDrive");
    if (!val.isEmpty())
        candidates.append({val, QtCloudBackup::StorageType::OneDrivePersonal});

    // 6. Documents folder fallback via SHGetKnownFolderPath
    {
        PWSTR path = nullptr;
        HRESULT hr = SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &path);
        if (SUCCEEDED(hr) && path) {
            candidates.append({QString::fromWCharArray(path), QtCloudBackup::StorageType::LocalDirectory});
            CoTaskMemFree(path);
        }
    }

    return candidates;
}

std::unique_ptr<CloudBackupBackend> createPlatformBackend()
{
    return std::make_unique<WindowsOneDriveBackend>();
}

void WindowsOneDriveBackend::initialise()
{
    // Transitional shim during issue #4 — wires the legacy single-call
    // initialise() onto the new detect()/select() flow. Removed in phase 6
    // when CloudBackupManager switches to driving detect/select directly.
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(this, &CloudBackupBackend::accountsDetected, this,
        [this, conn](const QList<DetectedAccount> &accounts) {
            QObject::disconnect(*conn);
            // Pick the first Ready account (Personal preferred over Business
            // is no longer the library's call — but for the legacy shim we
            // just pick the first available match to keep behaviour stable).
            for (const auto &a : accounts) {
                if (a.status == QtCloudBackup::StorageStatus::Ready) {
                    select(a.id);
                    return;
                }
            }
            // No Ready account. The applyDetectionResult invalidation path
            // already emitted statusChanged with appropriate detail for the
            // backend-level case (device GP block). For per-account failures
            // with no prior selection, surface the first account's status
            // so the consumer learns why nothing is usable.
            if (m_status != QtCloudBackup::StorageStatus::Disabled) {
                if (!accounts.isEmpty()) {
                    m_status = accounts.first().status;
                    m_statusDetail = accounts.first().statusDetail;
                } else {
                    m_status = QtCloudBackup::StorageStatus::Unavailable;
                    m_statusDetail = tr("No OneDrive accounts found");
                }
                emit statusChanged(m_status, m_statusDetail);
            }
        });
    detect();
}

void WindowsOneDriveBackend::detect()
{
    const int gen = ++m_detectionGeneration;
    QPointer<WindowsOneDriveBackend> self(this);
    (void)QtConcurrent::run([self, gen] {
        // 1. Device-level Group Policy: HKLM\SOFTWARE\Policies\Microsoft\
        //    Windows\OneDrive\DisableFileSyncNGSC (REG_DWORD). Note the
        //    `Windows\` segment — distinct from the per-account-policy root
        //    HKLM\SOFTWARE\Policies\Microsoft\OneDrive\ (no `Windows\`).
        //    Easy to confuse, often is.
        QSettings devicePolicy(
            QStringLiteral(R"(HKEY_LOCAL_MACHINE\SOFTWARE\Policies\Microsoft\Windows\OneDrive)"),
            QSettings::NativeFormat);
        const bool deviceBlocked =
            devicePolicy.value(QStringLiteral("DisableFileSyncNGSC"), 0).toInt() == 1;

        QList<DetectedAccount> accounts;
        QMap<QString, QString> userFolders;
        QString deviceDetail;

        if (deviceBlocked) {
            deviceDetail = WindowsOneDriveBackend::tr(
                "OneDrive is disabled by your organisation's policy");
        } else {
            // 2. Per-account GP precompute (the OTHER policy root — no
            //    `Windows\` segment). Personal sync block can be set under
            //    HKCU or HKLM; either is sufficient.
            QSettings policyHkcu(
                QStringLiteral(R"(HKEY_CURRENT_USER\SOFTWARE\Policies\Microsoft\OneDrive)"),
                QSettings::NativeFormat);
            QSettings policyHklm(
                QStringLiteral(R"(HKEY_LOCAL_MACHINE\SOFTWARE\Policies\Microsoft\OneDrive)"),
                QSettings::NativeFormat);
            const bool personalSyncDisabled =
                policyHkcu.value(QStringLiteral("DisablePersonalSync"), 0).toInt() == 1
                || policyHklm.value(QStringLiteral("DisablePersonalSync"), 0).toInt() == 1;

            // AllowTenantList / BlockTenantList are keys whose VALUES (not
            // subkeys) name tenant GUIDs. Enumerate via childKeys().
            // AllowTenantList takes precedence over BlockTenantList per
            // Microsoft docs. Applies to Business accounts only (Personal
            // MSAs have no tenant).
            QSettings allowList(
                QStringLiteral(R"(HKEY_LOCAL_MACHINE\SOFTWARE\Policies\Microsoft\OneDrive\AllowTenantList)"),
                QSettings::NativeFormat);
            QSettings blockList(
                QStringLiteral(R"(HKEY_LOCAL_MACHINE\SOFTWARE\Policies\Microsoft\OneDrive\BlockTenantList)"),
                QSettings::NativeFormat);
            const QStringList allowed = allowList.childKeys();
            const QStringList blocked = blockList.childKeys();

            // 3. Enumerate configured accounts. Business numbering can have
            //    gaps (Business1, Business3 if accounts removed and re-added)
            //    so we scan rather than assume contiguity. Devices with no
            //    OneDrive client installed (Win 10 IoT etc.) have no Accounts
            //    key at all — childGroups() returns empty, accounts stays
            //    empty, no statusChanged at backend level. The consumer sees
            //    an empty DetectedAccount list, which maps to the same user
            //    remediation as "signed out": set up OneDrive (or pick local).
            QSettings accountsKey(
                QStringLiteral(R"(HKEY_CURRENT_USER\Software\Microsoft\OneDrive\Accounts)"),
                QSettings::NativeFormat);
            const QStringList childKeys = accountsKey.childGroups();
            for (const QString &key : childKeys) {
                const bool isPersonal = (key == QLatin1String("Personal"));
                const bool isBusiness = key.startsWith(QLatin1String("Business"));
                if (!isPersonal && !isBusiness)
                    continue; // Skip unknown subkeys (e.g. "Common")

                QSettings acc(QStringLiteral(R"(HKEY_CURRENT_USER\Software\Microsoft\OneDrive\Accounts\)")
                                  + key,
                              QSettings::NativeFormat);
                const QString email = acc.value(QStringLiteral("UserEmail")).toString();
                const QString folder = acc.value(QStringLiteral("UserFolder")).toString();
                const QString tenantId = acc.value(QStringLiteral("ConfiguredTenantId")).toString();

                // "All fields non-empty" is the configured-account predicate.
                // OneDrive sign-out has been observed to leave partial
                // registry residue (e.g. UserFolder populated after UserEmail
                // is gone); treat any missing field as not-active.
                const bool fieldsComplete = !email.isEmpty() && !folder.isEmpty()
                    && (isPersonal || !tenantId.isEmpty());

                DetectedAccount a;
                a.id.type = isPersonal ? QtCloudBackup::StorageType::OneDrivePersonal
                                       : QtCloudBackup::StorageType::OneDriveCommercial;
                a.id.accountKey = key;
                // displayName is the on-disk folder basename as Windows
                // itself names it — i.e. what File Explorer's nav pane
                // shows. Typically "OneDrive" for Personal and
                // "OneDrive - <org>" (e.g. "OneDrive - Contoso") for
                // Business. The consumer renders Business verbatim and
                // composes its own label for Personal (Windows's bare
                // "OneDrive" isn't enough to distinguish it in a multi-
                // account picker). Falls back to the account key for
                // partial-registry residue.
                a.displayName = fieldsComplete ? QFileInfo(folder).fileName() : key;
                a.email = email;
                a.tenantId = isPersonal ? QString() : tenantId;

                if (!fieldsComplete) {
                    a.status = QtCloudBackup::StorageStatus::Unavailable;
                    a.statusDetail = WindowsOneDriveBackend::tr(
                        "OneDrive account partially configured");
                } else if (isPersonal && personalSyncDisabled) {
                    a.status = QtCloudBackup::StorageStatus::Disabled;
                    a.statusDetail = WindowsOneDriveBackend::tr(
                        "Personal OneDrive sync is disabled by your organisation's policy");
                } else if (isBusiness && !allowed.isEmpty()
                           && !allowed.contains(tenantId, Qt::CaseInsensitive)) {
                    a.status = QtCloudBackup::StorageStatus::Disabled;
                    a.statusDetail = WindowsOneDriveBackend::tr(
                        "This work account isn't allowed by your organisation's policy");
                } else if (isBusiness && blocked.contains(tenantId, Qt::CaseInsensitive)) {
                    a.status = QtCloudBackup::StorageStatus::Disabled;
                    a.statusDetail = WindowsOneDriveBackend::tr(
                        "This work account is blocked by your organisation's policy");
                } else if (!QFileInfo(folder).isWritable()) {
                    // Metadata check, NOT a write. Detection is side-effect-
                    // free; the actual writability gate is the mkpath in
                    // select(). Missing folder also fails isWritable(), which
                    // is the behaviour we want — signed-out residue surfaces
                    // as Unavailable rather than Ready.
                    a.status = QtCloudBackup::StorageStatus::Unavailable;
                    a.statusDetail = WindowsOneDriveBackend::tr(
                        "OneDrive folder is missing or not writable");
                } else {
                    a.status = QtCloudBackup::StorageStatus::Ready;
                    a.statusDetail = isPersonal
                        ? WindowsOneDriveBackend::tr("OneDrive (personal) is available")
                        : WindowsOneDriveBackend::tr("OneDrive for Business is available");
                }

                if (!folder.isEmpty())
                    userFolders.insert(key, folder);
                accounts.append(a);
            }
        }

        QMetaObject::invokeMethod(qApp,
            [self, gen, accounts, userFolders, deviceBlocked, deviceDetail] {
                if (!self) return;
                // Staleness gate: a fresher detect() has already started;
                // drop this completion rather than letting it clobber
                // m_lastDetection / re-emit outdated status.
                if (gen != self->m_detectionGeneration) return;
                self->applyDetectionResult(accounts, userFolders, deviceBlocked, deviceDetail);
            }, Qt::QueuedConnection);
    });
}

void WindowsOneDriveBackend::applyDetectionResult(QList<DetectedAccount> accounts,
                                                  QMap<QString, QString> userFolders,
                                                  bool deviceBlocked,
                                                  QString deviceDetail)
{
    m_lastDetection = accounts;
    m_userFolders = userFolders;

    if (deviceBlocked) {
        // Device-level GP block trumps everything: the consumer can't pick
        // any account. Tear down any active selection and emit Disabled.
        if (m_selectedId.type != QtCloudBackup::StorageType::None
            || m_status != QtCloudBackup::StorageStatus::Disabled) {
            m_backupRoot.clear();
            m_storageType = QtCloudBackup::StorageType::None;
            m_selectedId = {};
            m_status = QtCloudBackup::StorageStatus::Disabled;
            m_statusDetail = deviceDetail;
            emit statusChanged(m_status, m_statusDetail);
        }
    } else if (m_selectedId.type != QtCloudBackup::StorageType::None) {
        // Selection-invalidation: if the previously-selected account is no
        // longer Ready in the new detection result, tear down the active
        // state. statusChanged fires BEFORE accountsDetected so consumers
        // see "your storage dropped" before "here are the current options".
        const DetectedAccount *selectedNow = nullptr;
        for (const auto &a : m_lastDetection) {
            if (a.id == m_selectedId) {
                selectedNow = &a;
                break;
            }
        }
        const bool stillReady = selectedNow
            && selectedNow->status == QtCloudBackup::StorageStatus::Ready;
        if (!stillReady) {
            m_backupRoot.clear();
            m_storageType = QtCloudBackup::StorageType::None;
            m_status = selectedNow ? selectedNow->status
                                   : QtCloudBackup::StorageStatus::Unavailable;
            m_statusDetail = selectedNow
                ? selectedNow->statusDetail
                : tr("Previously-selected OneDrive account is no longer detected");
            m_selectedId = {};
            emit statusChanged(m_status, m_statusDetail);
        }
    }

    emit accountsDetected(m_lastDetection);
}

void WindowsOneDriveBackend::select(const AccountId &id)
{
    if (id.type != QtCloudBackup::StorageType::OneDrivePersonal
        && id.type != QtCloudBackup::StorageType::OneDriveCommercial) {
        m_status = QtCloudBackup::StorageStatus::Unavailable;
        m_statusDetail = tr("Windows build supports only OneDrive accounts in select()");
        emit statusChanged(m_status, m_statusDetail);
        return;
    }
    if (!m_userFolders.contains(id.accountKey)) {
        m_status = QtCloudBackup::StorageStatus::Unavailable;
        m_statusDetail = tr("Account not in current detection — call detect() first");
        emit statusChanged(m_status, m_statusDetail);
        return;
    }

    if (m_selectedId == id && m_status == QtCloudBackup::StorageStatus::Ready)
        return; // reentrant no-op

    const int genAtStart = m_detectionGeneration;
    const QString userFolder = m_userFolders.value(id.accountKey);
    const QString relPath = QStringLiteral(QTCLOUDBACKUP_WINDOWS_BACKUP_PATH);
    const QString fullDir = relPath.isEmpty() ? userFolder
                                              : userFolder + QLatin1Char('/') + relPath;

    QPointer<WindowsOneDriveBackend> self(this);
    (void)QtConcurrent::run([self, id, genAtStart, userFolder, fullDir] {
        QDir dir(fullDir);
        const bool ok = dir.exists() || dir.mkpath(QStringLiteral("."));

        QMetaObject::invokeMethod(qApp,
            [self, id, genAtStart, ok, userFolder] {
                if (!self) return;
                // If a detect() ran while we were creating the backup
                // directory, only apply the result if the target account is
                // still Ready in the latest detection. Otherwise the
                // invalidation path has already set the correct state and
                // we must not clobber it.
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
                    self->m_backupRoot = userFolder;
                    self->m_storageType = id.type;
                    self->m_selectedId = id;
                    self->m_status = QtCloudBackup::StorageStatus::Ready;
                    self->m_statusDetail = (id.type == QtCloudBackup::StorageType::OneDrivePersonal)
                        ? WindowsOneDriveBackend::tr("Using OneDrive (personal)")
                        : WindowsOneDriveBackend::tr("Using OneDrive for Business");
                    emit self->statusChanged(self->m_status, self->m_statusDetail);
                } else {
                    self->m_status = QtCloudBackup::StorageStatus::Unavailable;
                    self->m_statusDetail = WindowsOneDriveBackend::tr(
                        "Failed to create backup directory");
                    self->m_backupRoot.clear();
                    self->m_storageType = QtCloudBackup::StorageType::None;
                    self->m_selectedId = {};
                    emit self->statusChanged(self->m_status, self->m_statusDetail);
                }
            }, Qt::QueuedConnection);
    });
}

std::optional<AccountId> WindowsOneDriveBackend::resolveAccount(QtCloudBackup::StorageType type,
                                                                const QString &tenantId,
                                                                const QString &email) const
{
    if (type != QtCloudBackup::StorageType::OneDrivePersonal
        && type != QtCloudBackup::StorageType::OneDriveCommercial) {
        return std::nullopt;
    }
    // Durable identity: Personal matches by email only (MSAs have no tenant);
    // Business matches by (tenantId, email). accountKey is in-memory only —
    // OneDrive may re-slot the same account at a different index across
    // unlink/re-add, so we must not consult the persisted accountKey.
    for (const auto &a : m_lastDetection) {
        if (a.id.type != type)
            continue;
        const bool emailMatch = a.email.compare(email, Qt::CaseInsensitive) == 0;
        if (type == QtCloudBackup::StorageType::OneDrivePersonal) {
            if (emailMatch)
                return a.id;
        } else {
            const bool tenantMatch = a.tenantId.compare(tenantId, Qt::CaseInsensitive) == 0;
            if (tenantMatch && emailMatch)
                return a.id;
        }
    }
    return std::nullopt;
}

QtCloudBackup::StorageStatus WindowsOneDriveBackend::storageStatus() const
{
    return m_status;
}

QString WindowsOneDriveBackend::statusDetail() const
{
    return m_statusDetail;
}

QtCloudBackup::StorageType WindowsOneDriveBackend::storageType() const
{
    return m_storageType;
}

void WindowsOneDriveBackend::writeBackup(const QString &filename, const QByteArray &data,
                                           const QJsonObject &meta)
{
    QString dir = backupDir();
    QPointer<WindowsOneDriveBackend> self(this);
    (void)QtConcurrent::run([self, dir, filename, data, meta] {
        QString bakPath = dir + QLatin1Char('/') + filename;
        QString metaPath = dir + QLatin1Char('/') + backupStem(filename) + QStringLiteral(".meta");

        // Write .bak file atomically
        {
            QSaveFile file(bakPath);
            if (!file.open(QIODevice::WriteOnly)) {
                QMetaObject::invokeMethod(qApp, [self, filename,
                        msg = WindowsOneDriveBackend::tr("Failed to open backup file for writing")] {
                    if (!self) return;
                    emit self->writeCompleted(filename,
                        int(QtCloudBackup::BackupError::IOError), msg);
                }, Qt::QueuedConnection);
                return;
            }
            file.write(data);
            if (!file.commit()) {
                QMetaObject::invokeMethod(qApp, [self, filename,
                        msg = WindowsOneDriveBackend::tr("Failed to write backup file")] {
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
                        msg = WindowsOneDriveBackend::tr("Failed to open metadata file for writing")] {
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
                        msg = WindowsOneDriveBackend::tr("Failed to write metadata file")] {
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

void WindowsOneDriveBackend::readBackup(const QString &filename)
{
    QString dir = backupDir();
    QPointer<WindowsOneDriveBackend> self(this);
    (void)QtConcurrent::run([self, dir, filename] {
        QString bakPath = dir + QLatin1Char('/') + filename;
        QString metaPath = dir + QLatin1Char('/') + backupStem(filename) + QStringLiteral(".meta");

        // Emit indeterminate progress — QFile::open() may block during hydration
        QMetaObject::invokeMethod(qApp, [self, filename] {
            if (!self) return;
            emit self->downloadProgress(filename, 0, -1);
        }, Qt::QueuedConnection);

        QFile bakFile(bakPath);
        if (!bakFile.open(QIODevice::ReadOnly)) {
            QMetaObject::invokeMethod(qApp, [self, filename,
                    msg = WindowsOneDriveBackend::tr("Failed to open backup file for reading")] {
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

void WindowsOneDriveBackend::deleteBackup(const QString &filename)
{
    QString dir = backupDir();
    QPointer<WindowsOneDriveBackend> self(this);
    (void)QtConcurrent::run([self, dir, filename] {
        QString bakPath = dir + QLatin1Char('/') + filename;
        QString metaPath = dir + QLatin1Char('/') + backupStem(filename) + QStringLiteral(".meta");

        // Delete .meta first (the completion marker), then .bak. Mirrors the
        // write protocol: .bak first, .meta last. If interrupted between the
        // two removals, an orphan .bak is left and the scanner surfaces it
        // with metadataAvailable=false rather than leaving an invisible
        // orphan .meta.
        if (!QFile::remove(metaPath) && QFile::exists(metaPath))
            qWarning("Failed to remove metadata sidecar: %s", qPrintable(metaPath));
        bool ok = QFile::remove(bakPath);

        int err = ok ? int(QtCloudBackup::BackupError::NoError)
                     : int(QtCloudBackup::BackupError::IOError);
        QString msg = ok ? QString() : WindowsOneDriveBackend::tr("Failed to delete backup file");
        QMetaObject::invokeMethod(qApp, [self, filename, err, msg] {
            if (!self) return;
            emit self->deleteCompleted(filename, err, msg);
        }, Qt::QueuedConnection);
    });
}

void WindowsOneDriveBackend::scanBackups()
{
    QString dir = backupDir();
    QPointer<WindowsOneDriveBackend> self(this);
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

            // Check download state via FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
            QString fullPath = dir + QLatin1Char('/') + entry;
            DWORD attrs = GetFileAttributesW(reinterpret_cast<LPCWSTR>(fullPath.utf16()));
            if (attrs != INVALID_FILE_ATTRIBUTES
                && (attrs & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) {
                info.downloadState = QtCloudBackup::DownloadState::CloudOnly;
            } else {
                info.downloadState = QtCloudBackup::DownloadState::Local;
            }

            // Try to read .meta sidecar (bounded). A missing .meta is not
            // junk under cloud sync — surface honestly via
            // metadataAvailable=false. May also indicate a Files-On-Demand
            // placeholder that hasn't yet hydrated.
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

void WindowsOneDriveBackend::triggerDownload(const QString &filename)
{
    QString dir = backupDir();
    QPointer<WindowsOneDriveBackend> self(this);
    (void)QtConcurrent::run([self, dir, filename] {
        QString fullPath = dir + QLatin1Char('/') + filename;

        // Emit indeterminate progress
        QMetaObject::invokeMethod(qApp, [self, filename] {
            if (!self) return;
            emit self->downloadProgress(filename, 0, -1);
        }, Qt::QueuedConnection);

        // Opening the file triggers transparent hydration on OneDrive
        QFile file(fullPath);
        bool ok = file.open(QIODevice::ReadOnly);
        if (ok)
            file.close();

        int err = ok ? int(QtCloudBackup::BackupError::NoError)
                     : int(QtCloudBackup::BackupError::DownloadError);
        QString msg = ok ? QString() : WindowsOneDriveBackend::tr("Failed to download backup file");
        QMetaObject::invokeMethod(qApp, [self, filename, err, msg] {
            if (!self) return;
            emit self->downloadCompleted(filename, err, msg);
        }, Qt::QueuedConnection);
    });
}

// --- Orphaned backup detection and migration ---

void WindowsOneDriveBackend::scanOrphanedBackups()
{
    QString activeRoot = m_backupRoot;
    auto activeType = m_storageType;
    QPointer<WindowsOneDriveBackend> self(this);
    (void)QtConcurrent::run([self, activeRoot, activeType] {
        auto candidates = detectOneDriveCandidates();

        // Find index of active candidate
        int activeIdx = -1;
        for (int i = 0; i < candidates.size(); ++i) {
            if (candidates[i].path == activeRoot) {
                activeIdx = i;
                break;
            }
        }

        // Scan candidates after the active one (lower priority), deduplicating paths
        QSet<QString> seenPaths;
        if (activeIdx >= 0)
            seenPaths.insert(QDir(activeRoot).canonicalPath());

        // Build list of full scan directories (with QTCLOUDBACKUP_WINDOWS_BACKUP_PATH applied)
        const QString relPath = QStringLiteral(QTCLOUDBACKUP_WINDOWS_BACKUP_PATH);
        QList<QPair<QString, QtCloudBackup::StorageType>> dirsToScan;
        if (activeIdx >= 0) {
            for (int i = activeIdx + 1; i < candidates.size(); ++i) {
                QString canonical = QDir(candidates[i].path).canonicalPath();
                if (canonical.isEmpty() || seenPaths.contains(canonical))
                    continue;
                seenPaths.insert(canonical);
                QString scanDir = relPath.isEmpty() ? candidates[i].path
                                                    : candidates[i].path + QLatin1Char('/') + relPath;
                dirsToScan.append({scanDir, candidates[i].type});
            }
        }

        // Always include local fallback (already a complete path — no relPath appended)
        QString fallback = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                           + QStringLiteral("/Backups");
        QString fallbackCanonical = QDir(fallback).canonicalPath();
        if (!fallbackCanonical.isEmpty() && !seenPaths.contains(fallbackCanonical))
            dirsToScan.append({fallback, QtCloudBackup::StorageType::LocalDirectory});

        static const QRegularExpression re(
            QStringLiteral("^qtcloudbackup_([a-zA-Z0-9_-]{1,64})_(\\d{8}_\\d{6}_\\d{3})_[a-z0-9]{4}\\.bak$"));

        QList<OrphanedBackupInfo> orphans;
        for (const auto &[scanDir, storType] : dirsToScan) {
            QDir d(scanDir);
            if (!d.exists())
                continue;

            QStringList entries = d.entryList({QStringLiteral("qtcloudbackup_*.bak")},
                                              QDir::Files, QDir::Name);
            for (const QString &entry : entries) {
                OrphanedBackupInfo info;
                info.filename = entry;
                info.originStorageType = storType;
                info.originPath = scanDir;

                QString metaPath = scanDir + QLatin1Char('/') + backupStem(entry)
                                   + QStringLiteral(".meta");
                QFile metaFile(metaPath);
                if (metaFile.open(QIODevice::ReadOnly)) {
                    QJsonObject meta = QJsonDocument::fromJson(metaFile.read(MaxMetaFileSize)).object();
                    info.sourceId = meta[QStringLiteral("sourceId")].toString();
                    info.timestamp = QDateTime::fromString(
                        meta[QStringLiteral("timestamp")].toString(), Qt::ISODateWithMs);
                    info.metadata = meta[QStringLiteral("metadata")].toObject().toVariantMap();
                }

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
        }

        QMetaObject::invokeMethod(qApp, [self, orphans] {
            if (!self) return;
            emit self->orphanScanCompleted(orphans);
        }, Qt::QueuedConnection);
    });
}

void WindowsOneDriveBackend::migrateOrphanedBackups(const QList<OrphanedBackupInfo> &orphans)
{
    QString destDir = backupDir();
    QPointer<WindowsOneDriveBackend> self(this);
    (void)QtConcurrent::run([self, destDir, orphans] {
        int total = orphans.size();
        int migrated = 0;
        bool allSucceeded = true;

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

            // Copy .bak
            if (!QFile::copy(srcBak, destBak)) {
                allSucceeded = false;
                continue;
            }

            // Copy .meta (best effort)
            if (QFile::exists(srcMeta))
                QFile::copy(srcMeta, destMeta);

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
            : WindowsOneDriveBackend::tr("Some files could not be migrated");
        QMetaObject::invokeMethod(qApp, [self, count, err, msg] {
            if (!self) return;
            emit self->migrationCompleted(count, err, msg);
        }, Qt::QueuedConnection);
    });
}

QString WindowsOneDriveBackend::backupDir() const
{
    const QString relPath = QStringLiteral(QTCLOUDBACKUP_WINDOWS_BACKUP_PATH);
    if (relPath.isEmpty())
        return m_backupRoot;
    return m_backupRoot + QLatin1Char('/') + relPath;
}

QString WindowsOneDriveBackend::localFallbackDir() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
           + QStringLiteral("/Backups");
}
