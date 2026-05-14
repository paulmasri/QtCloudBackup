#include "cloudbackupmanager.h"
#include "cloudbackupbackend.h"
#include "backupvalidation.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <algorithm>

std::unique_ptr<CloudBackupBackend> createPlatformBackend();

CloudBackupManager::CloudBackupManager(QObject *parent)
    : QObject(parent)
    , m_backend(createPlatformBackend())
{
    connect(m_backend.get(), &CloudBackupBackend::statusChanged, this,
            [this](QtCloudBackup::StorageStatus status, const QString &detail) {
                emit storageStatusChanged();
                emit statusDetailChanged();
                emit storageTypeChanged();
                emit statusChanged(status, detail);
            });

    connect(m_backend.get(), &CloudBackupBackend::writeCompleted, this,
            [this](const QString &filename, int error, const QString &message) {
                m_backupInProgress = false;
                emit backupInProgressChanged();
                if (error == int(QtCloudBackup::BackupError::NoError)) {
                    emit backupSucceeded(filename, m_currentBackupTimestamp);
                    pruneBackups(m_currentBackupSourceId);
                } else {
                    emit backupFailed(error, message);
                }
            });

    connect(m_backend.get(), &CloudBackupBackend::scanCompleted, this,
            &CloudBackupManager::backupsListed);

    connect(m_backend.get(), &CloudBackupBackend::readCompleted, this,
            [this](const QString &filename, const QByteArray &data, const QJsonObject &meta,
                   int error, const QString &message) {
                if (error == int(QtCloudBackup::BackupError::NoError)) {
                    m_pendingRestoreFilename.clear();
                    m_backupInProgress = false;
                    emit backupInProgressChanged();
                    emit restoreUpdated(filename, QtCloudBackup::RestoreStatus::RestoreSucceeded,
                                        data, meta.toVariantMap(),
                                        int(QtCloudBackup::BackupError::NoError), QString());
                } else {
                    handleReadFailed(filename, error, message);
                }
            });

    connect(m_backend.get(), &CloudBackupBackend::deleteCompleted, this,
            [this](const QString &filename, int error, const QString &message) {
                if (error == int(QtCloudBackup::BackupError::NoError))
                    emit deleteSucceeded(filename);
                else
                    emit deleteFailed(filename, error, message);
            });

    connect(m_backend.get(), &CloudBackupBackend::downloadProgress, this,
            &CloudBackupManager::downloadProgressChanged);

    connect(m_backend.get(), &CloudBackupBackend::downloadCompleted, this,
            [this](const QString &filename, int error, const QString &message) {
                bool success = (error == int(QtCloudBackup::BackupError::NoError));
                if (!m_pendingRestoreFilename.isEmpty() && filename == m_pendingRestoreFilename) {
                    if (success) {
                        // Auto-retry the restore
                        emit restoreUpdated(filename, QtCloudBackup::RestoreStatus::RestoreInProgress,
                                            {}, {}, int(QtCloudBackup::BackupError::NoError), QString());
                        m_backend->readBackup(filename);
                    } else {
                        m_pendingRestoreFilename.clear();
                        m_backupInProgress = false;
                        emit backupInProgressChanged();
                        emit restoreUpdated(filename, QtCloudBackup::RestoreStatus::RestoreFailed,
                                            {}, {}, error, message);
                    }
                } else {
                    auto status = success ? QtCloudBackup::DownloadStatus::DownloadSucceeded
                                          : QtCloudBackup::DownloadStatus::DownloadFailed;
                    emit downloadUpdated(filename, status, error, message);
                }
            });

    connect(m_backend.get(), &CloudBackupBackend::remoteChangeDetected, this,
            &CloudBackupManager::remoteBackupDetected);

    connect(m_backend.get(), &CloudBackupBackend::orphanScanCompleted, this,
            [this](const QList<OrphanedBackupInfo> &orphans) {
                m_orphanedBackups = orphans;
                emit hasOrphanedBackupsChanged();
                QVariantList list;
                list.reserve(orphans.size());
                for (const auto &o : orphans)
                    list.append(QVariant::fromValue(o));
                emit orphanedBackupsDetected(list);
            });

    connect(m_backend.get(), &CloudBackupBackend::migrationProgress, this,
            [this](int completed, int total) {
                emit migrationUpdated(QtCloudBackup::MigrationStatus::MigrationInProgress,
                                      completed, total,
                                      int(QtCloudBackup::BackupError::NoError), QString());
            });

    connect(m_backend.get(), &CloudBackupBackend::migrationCompleted, this,
            [this](int migratedCount, int error, const QString &message) {
                auto status = (error == int(QtCloudBackup::BackupError::NoError))
                    ? QtCloudBackup::MigrationStatus::MigrationSucceeded
                    : QtCloudBackup::MigrationStatus::MigrationFailed;
                int total = m_orphanedBackups.size();
                m_orphanedBackups.clear();
                emit hasOrphanedBackupsChanged();
                emit migrationUpdated(status, migratedCount, total, error, message);
            });

    m_backend->initialise();
}

CloudBackupManager::~CloudBackupManager() = default;

QtCloudBackup::StorageStatus CloudBackupManager::storageStatus() const
{
    return m_backend->storageStatus();
}

QString CloudBackupManager::statusDetail() const
{
    return m_backend->statusDetail();
}

QtCloudBackup::StorageType CloudBackupManager::storageType() const
{
    return m_backend->storageType();
}

bool CloudBackupManager::backupInProgress() const
{
    return m_backupInProgress;
}

QtCloudBackup::RetentionPolicy CloudBackupManager::retentionPolicy() const
{
    return m_retentionPolicy;
}

void CloudBackupManager::setRetentionPolicy(const QtCloudBackup::RetentionPolicy &policy)
{
    if (m_retentionPolicy == policy)
        return;
    m_retentionPolicy = policy;
    emit retentionPolicyChanged();
}

bool CloudBackupManager::hasOrphanedBackups() const
{
    return !m_orphanedBackups.isEmpty();
}

void CloudBackupManager::createBackup(const QString &sourceId, const QByteArray &data,
                                       const QVariantMap &metadata)
{
    if (m_backupInProgress)
        return;

    // Sanitize sourceId
    QString sanitized;
    for (const QChar &c : sourceId) {
        if ((c >= QLatin1Char('a') && c <= QLatin1Char('z'))
            || (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
            || c.isDigit() || c == QLatin1Char('-') || c == QLatin1Char('_'))
            sanitized.append(c);
    }
    if (sanitized.isEmpty()) {
        emit backupFailed(int(QtCloudBackup::BackupError::InvalidArgument),
                          tr("Source ID is empty after sanitization"));
        return;
    }
    if (sanitized.length() > 64) {
        sanitized.truncate(64);
    }

    m_backupInProgress = true;
    emit backupInProgressChanged();

    m_currentBackupSourceId = sanitized;
    m_currentBackupTimestamp = QDateTime::currentDateTimeUtc();

    // Generate filename
    const QString timestamp = m_currentBackupTimestamp.toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    const QString random = QString::number(QRandomGenerator::global()->bounded(0, 0xFFFF), 16)
                               .rightJustified(4, QLatin1Char('0'));
    const QString filename = QStringLiteral("qtcloudbackup_%1_%2_%3.bak")
                                 .arg(sanitized, timestamp, random);

    // Build metadata JSON
    QJsonObject meta;
    meta[QStringLiteral("sourceId")] = sanitized;
    meta[QStringLiteral("timestamp")] = m_currentBackupTimestamp.toString(Qt::ISODateWithMs);
    meta[QStringLiteral("metadata")] = QJsonObject::fromVariantMap(metadata);
    meta[QStringLiteral("libraryVersion")] = QStringLiteral(QTCLOUDBACKUP_VERSION);

    m_backend->writeBackup(filename, data, meta);
}

void CloudBackupManager::listBackups()
{
    m_backend->scanBackups();
}

void CloudBackupManager::requestDownload(const QString &filename)
{
    if (!isValidBackupFilename(filename)) {
        emit downloadUpdated(filename, QtCloudBackup::DownloadStatus::DownloadFailed,
                             int(QtCloudBackup::BackupError::InvalidArgument),
                             tr("Invalid backup filename"));
        return;
    }
    emit downloadUpdated(filename, QtCloudBackup::DownloadStatus::DownloadInProgress,
                         int(QtCloudBackup::BackupError::NoError), QString());
    m_backend->triggerDownload(filename);
}

void CloudBackupManager::restoreBackup(const QString &filename)
{
    if (!isValidBackupFilename(filename)) {
        emit restoreUpdated(filename, QtCloudBackup::RestoreStatus::RestoreFailed, {}, {},
                            int(QtCloudBackup::BackupError::InvalidArgument),
                            tr("Invalid backup filename"));
        return;
    }
    if (m_backupInProgress)
        return;

    m_pendingRestoreFilename.clear();
    m_backupInProgress = true;
    emit backupInProgressChanged();
    emit restoreUpdated(filename, QtCloudBackup::RestoreStatus::RestoreInProgress, {}, {},
                        int(QtCloudBackup::BackupError::NoError), QString());
    m_backend->readBackup(filename);
}

void CloudBackupManager::deleteBackup(const QString &filename)
{
    if (!isValidBackupFilename(filename)) {
        emit deleteFailed(filename, int(QtCloudBackup::BackupError::InvalidArgument),
                          tr("Invalid backup filename"));
        return;
    }
    m_backend->deleteBackup(filename);
}

void CloudBackupManager::refresh()
{
    m_backend->initialise();
}

void CloudBackupManager::prune(const QString &sourceId)
{
    if (sourceId.isEmpty() || !m_backend)
        return;
    pruneBackups(sourceId);
}

void CloudBackupManager::checkForOrphanedBackups()
{
    m_backend->scanOrphanedBackups();
}

void CloudBackupManager::migrateOrphanedBackups()
{
    if (m_orphanedBackups.isEmpty())
        return;
    emit migrationUpdated(QtCloudBackup::MigrationStatus::MigrationInProgress,
                          0, m_orphanedBackups.size(),
                          int(QtCloudBackup::BackupError::NoError), QString());
    m_backend->migrateOrphanedBackups(m_orphanedBackups);
}

QtCloudBackup::RetentionPolicy CloudBackupManager::makeRetentionPolicy(
    int keepLast, int keepDaily, int keepWeekly, int keepMonthly, int keepYearly) const
{
    return { keepLast, keepDaily, keepWeekly, keepMonthly, keepYearly };
}

void CloudBackupManager::pruneBackups(const QString &sourceId)
{
    // Interim implementation: honours keepLast only. The full union-of-keeps
    // evaluator (keepDaily/Weekly/Monthly/Yearly, local-time bucketing,
    // metadataAvailable handling, min-keep safety invariant) lands in the
    // next phase.
    const int keepLast = m_retentionPolicy.keepLast;
    if (keepLast <= 0)
        return;

    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(m_backend.get(), &CloudBackupBackend::scanCompleted, this,
                    [this, sourceId, conn, keepLast](const QList<BackupInfo> &backups) {
                        disconnect(*conn);

                        QList<BackupInfo> matching;
                        for (const auto &b : backups) {
                            if (b.sourceId == sourceId)
                                matching.append(b);
                        }

                        if (matching.size() <= keepLast)
                            return;

                        std::sort(matching.begin(), matching.end(),
                                  [](const BackupInfo &a, const BackupInfo &b) {
                                      return a.timestamp > b.timestamp;
                                  });

                        for (int i = keepLast; i < matching.size(); ++i) {
                            m_backend->deleteBackup(matching[i].filename);
                        }
                    });

    m_backend->scanBackups();
}

void CloudBackupManager::handleReadFailed(const QString &filename, int error,
                                           const QString &message)
{
    // If the file is cloud-only and we haven't already tried downloading,
    // auto-trigger the download and retry on completion.
    if (m_pendingRestoreFilename.isEmpty()
        && error == int(QtCloudBackup::BackupError::FileNotLocal)) {
        m_pendingRestoreFilename = filename;
        emit restoreUpdated(filename, QtCloudBackup::RestoreStatus::RestoreDownloading, {}, {},
                            int(QtCloudBackup::BackupError::NoError), QString());
        m_backend->triggerDownload(filename);
        return;
    }

    // Either the retry failed or it's a different error — give up
    m_pendingRestoreFilename.clear();
    m_backupInProgress = false;
    emit backupInProgressChanged();
    emit restoreUpdated(filename, QtCloudBackup::RestoreStatus::RestoreFailed, {}, {},
                        error, message);
}
