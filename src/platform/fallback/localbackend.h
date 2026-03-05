#ifndef QTCLOUDBACKUP_LOCALBACKEND_H
#define QTCLOUDBACKUP_LOCALBACKEND_H

#include "../../cloudbackupbackend.h"

class LocalBackend : public CloudBackupBackend {
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

    QtCloudBackup::StorageStatus m_status = QtCloudBackup::StorageStatus::Unknown;
    QString m_statusDetail;
};

#endif // QTCLOUDBACKUP_LOCALBACKEND_H
