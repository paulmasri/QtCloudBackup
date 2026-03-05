#ifndef QTCLOUDBACKUP_WINDOWSONEDRIVEBACKEND_H
#define QTCLOUDBACKUP_WINDOWSONEDRIVEBACKEND_H

#include "../../cloudbackupbackend.h"

class WindowsOneDriveBackend : public CloudBackupBackend {
    Q_OBJECT
public:
    using CloudBackupBackend::CloudBackupBackend;

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

    QString m_backupRoot;
    QtCloudBackup::StorageStatus m_status = QtCloudBackup::StorageStatus::Unknown;
    QString m_statusDetail;
    QtCloudBackup::StorageType m_storageType = QtCloudBackup::StorageType::None;
};

#endif // QTCLOUDBACKUP_WINDOWSONEDRIVEBACKEND_H
