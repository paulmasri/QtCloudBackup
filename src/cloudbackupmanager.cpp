#include "cloudbackupmanager.h"
#include "cloudbackupbackend.h"

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

    connect(m_backend.get(), &CloudBackupBackend::writeSucceeded, this,
            [this](const QString &filename) {
                m_backupInProgress = false;
                emit backupInProgressChanged();
                emit backupSucceeded(filename, m_currentBackupTimestamp);
                pruneBackups(m_currentBackupSourceId);
            });

    connect(m_backend.get(), &CloudBackupBackend::writeFailed, this,
            [this](const QString &reason) {
                m_backupInProgress = false;
                emit backupInProgressChanged();
                emit backupFailed(reason);
            });

    connect(m_backend.get(), &CloudBackupBackend::scanCompleted, this,
            &CloudBackupManager::backupsListed);

    connect(m_backend.get(), &CloudBackupBackend::readSucceeded, this,
            [this](const QString &, const QByteArray &data, const QJsonObject &meta) {
                m_backupInProgress = false;
                emit backupInProgressChanged();
                emit restoreSucceeded(data, meta.toVariantMap());
            });

    connect(m_backend.get(), &CloudBackupBackend::readFailed, this,
            [this](const QString &, const QString &reason) {
                m_backupInProgress = false;
                emit backupInProgressChanged();
                emit restoreFailed(reason);
            });

    connect(m_backend.get(), &CloudBackupBackend::deleteCompleted, this,
            [this](const QString &filename, bool success, const QString &reason) {
                if (success)
                    emit deleteSucceeded(filename);
                else
                    emit deleteFailed(filename, reason);
            });

    connect(m_backend.get(), &CloudBackupBackend::downloadProgress, this,
            &CloudBackupManager::downloadProgressChanged);

    connect(m_backend.get(), &CloudBackupBackend::downloadCompleted, this,
            [this](const QString &filename, bool success, const QString &reason) {
                if (success)
                    emit downloadReady(filename);
                else
                    emit downloadFailed(filename, reason);
            });

    connect(m_backend.get(), &CloudBackupBackend::remoteChangeDetected, this,
            &CloudBackupManager::remoteBackupDetected);

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

int CloudBackupManager::maxBackupsPerSource() const
{
    return m_maxBackupsPerSource;
}

void CloudBackupManager::setMaxBackupsPerSource(int n)
{
    if (m_maxBackupsPerSource == n)
        return;
    m_maxBackupsPerSource = n;
    emit maxBackupsPerSourceChanged();
}

void CloudBackupManager::createBackup(const QString &sourceId, const QByteArray &data,
                                       const QVariantMap &metadata)
{
    if (m_backupInProgress)
        return;

    // Sanitize sourceId
    QString sanitized;
    for (const QChar &c : sourceId) {
        if (c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_'))
            sanitized.append(c);
    }
    if (sanitized.isEmpty()) {
        emit backupFailed(tr("Source ID is empty after sanitization"));
        return;
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
    meta[QStringLiteral("libraryVersion")] = QStringLiteral("1.0.0");

    m_backend->writeBackup(filename, data, meta);
}

void CloudBackupManager::listBackups()
{
    m_backend->scanBackups();
}

void CloudBackupManager::requestDownload(const QString &filename)
{
    m_backend->triggerDownload(filename);
}

void CloudBackupManager::restoreBackup(const QString &filename)
{
    if (m_backupInProgress)
        return;

    m_backupInProgress = true;
    emit backupInProgressChanged();
    m_backend->readBackup(filename);
}

void CloudBackupManager::deleteBackup(const QString &filename)
{
    m_backend->deleteBackup(filename);
}

void CloudBackupManager::refresh()
{
    m_backend->initialise();
}

void CloudBackupManager::pruneBackups(const QString &sourceId)
{
    // Connect a one-shot to the scan result to perform pruning
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(m_backend.get(), &CloudBackupBackend::scanCompleted, this,
                    [this, sourceId, conn](const QList<BackupInfo> &backups) {
                        disconnect(*conn);

                        // Filter to this sourceId
                        QList<BackupInfo> matching;
                        for (const auto &b : backups) {
                            if (b.sourceId == sourceId)
                                matching.append(b);
                        }

                        if (matching.size() <= m_maxBackupsPerSource)
                            return;

                        // Sort newest first
                        std::sort(matching.begin(), matching.end(),
                                  [](const BackupInfo &a, const BackupInfo &b) {
                                      return a.timestamp > b.timestamp;
                                  });

                        // Delete oldest
                        for (int i = m_maxBackupsPerSource; i < matching.size(); ++i) {
                            m_backend->deleteBackup(matching[i].filename);
                        }
                    });

    m_backend->scanBackups();
}
