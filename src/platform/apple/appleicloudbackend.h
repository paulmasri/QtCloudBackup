#ifndef QTCLOUDBACKUP_APPLEICLOUDBACKEND_H
#define QTCLOUDBACKUP_APPLEICLOUDBACKEND_H

#include "../../cloudbackupbackend.h"

#include <QPointer>
#include <QUrl>

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

private:
    QString backupDir() const;
    void startMetadataQuery();
    void stopMetadataQuery();
    void handleQueryResults();

    QUrl m_containerUrl;
    QtCloudBackup::StorageStatus m_status = QtCloudBackup::StorageStatus::Unknown;
    QString m_statusDetail;
    void *m_metadataQuery = nullptr; // NSMetadataQuery*
    void *m_notificationObserver = nullptr; // id for NSUbiquityIdentityDidChangeNotification
};

#endif // QTCLOUDBACKUP_APPLEICLOUDBACKEND_H
