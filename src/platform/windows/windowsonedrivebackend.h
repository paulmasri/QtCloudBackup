#pragma once

#include "../../cloudbackupbackend.h"

#include <QMap>

class WindowsOneDriveBackend : public CloudBackupBackend {
    Q_OBJECT
public:
    using CloudBackupBackend::CloudBackupBackend;

    void initialise() override;
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
    QString localFallbackDir() const;
    void applyDetectionResult(QList<DetectedAccount> accounts,
                              QMap<QString, QString> userFolders,
                              bool deviceBlocked,
                              QString deviceDetail);

    QString m_backupRoot;                        // UserFolder of the selected account
    QtCloudBackup::StorageStatus m_status = QtCloudBackup::StorageStatus::Unknown;
    QString m_statusDetail;
    QtCloudBackup::StorageType m_storageType = QtCloudBackup::StorageType::None;
    QList<DetectedAccount> m_lastDetection;
    AccountId m_selectedId;
    // accountKey → UserFolder. Populated by detect(); consumed by select() so
    // it can resolve the on-disk path for the chosen account without
    // re-reading the registry.
    QMap<QString, QString> m_userFolders;
    // Bumped on every detect() entry. Async detect()/select() workers capture
    // the value at start and re-check at completion on the main thread: a
    // stale completion (older gen) is dropped rather than allowed to clobber
    // the current state with an outdated result.
    int m_detectionGeneration = 0;
};
