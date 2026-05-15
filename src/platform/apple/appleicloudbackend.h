#pragma once

#include "../../cloudbackupbackend.h"

#include <QMutex>
#include <QPointer>
#include <QUrl>
#include <memory>

// Thread-safe guard for the NSMetadataQuery pointer.
// Shared via std::shared_ptr so worker threads can safely access
// the query even if the backend object is destroyed mid-flight.
struct AppleQueryGuard {
    QMutex mutex;
    void *query = nullptr; // NSMetadataQuery*
};

class AppleICloudBackend : public CloudBackupBackend {
    Q_OBJECT
public:
    using CloudBackupBackend::CloudBackupBackend;
    ~AppleICloudBackend() override;

    void detect() override;
    void select(const AccountId &id) override;
    std::optional<AccountId> resolveAccount(QtCloudBackup::StorageType type,
                                            const QString &tenantId,
                                            const QString &email) const override;
    QtCloudBackup::StorageStatus storageStatus() const override;
    QString statusDetail() const override;
    QtCloudBackup::StorageType storageType() const override;

    void writeBackup(const QString &filename, const QByteArray &data, const QJsonObject &meta) override;
    void readBackup(const QString &filename) override;
    void deleteBackup(const QString &filename) override;
    void scanBackups() override;
    void triggerDownload(const QString &filename) override;
    void scanOrphanedBackups() override;
    void migrateOrphanedBackups(const QList<OrphanedBackupInfo> &orphans) override;

private:
    QString backupDir() const;
    void startMetadataQuery();
    void stopMetadataQuery();
    void handleQueryResults();
    void applyDetectionResult(QList<DetectedAccount> accounts, int gen);

    QUrl m_containerUrl;          // final Backups/ path, valid only after select() succeeds
    QUrl m_pendingContainerRoot;  // detect() result — root iCloud container URL, sans Backups/
    QList<DetectedAccount> m_lastDetection;
    AccountId m_selectedId;
    QtCloudBackup::StorageStatus m_status = QtCloudBackup::StorageStatus::Unknown;
    QString m_statusDetail;
    // Bumped on every detect() entry. Async detect()/select() workers capture
    // the value at start and re-check at completion on the main thread: a
    // stale completion (older gen) is dropped rather than allowed to clobber
    // the current state with an outdated result.
    int m_detectionGeneration = 0;
    std::shared_ptr<AppleQueryGuard> m_queryGuard = std::make_shared<AppleQueryGuard>();
    void *m_notificationObserver = nullptr; // id for NSUbiquityIdentityDidChangeNotification
};
