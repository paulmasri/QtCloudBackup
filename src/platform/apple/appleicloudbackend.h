#ifndef QTCLOUDBACKUP_APPLEICLOUDBACKEND_H
#define QTCLOUDBACKUP_APPLEICLOUDBACKEND_H

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

    void initialise() override;
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
    QString localFallbackDir() const;
    void startMetadataQuery();
    void stopMetadataQuery();
    void handleQueryResults();

    QUrl m_containerUrl;
    QtCloudBackup::StorageStatus m_status = QtCloudBackup::StorageStatus::Unknown;
    QString m_statusDetail;
    std::shared_ptr<AppleQueryGuard> m_queryGuard = std::make_shared<AppleQueryGuard>();
    void *m_notificationObserver = nullptr; // id for NSUbiquityIdentityDidChangeNotification
};

#endif // QTCLOUDBACKUP_APPLEICLOUDBACKEND_H
