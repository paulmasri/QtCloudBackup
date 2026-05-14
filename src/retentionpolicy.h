#pragma once

#include <QObject>
#include <QtQml/qqmlregistration.h>

namespace QtCloudBackup {

// Union-of-keeps retention policy, modelled on Borg / restic / sanoid.
// Each keepX field is an independent rule selecting a set of backups to
// retain; the union of all rules' selections is kept, everything else is
// pruned. A field set to 0 disables that rule.
//
// Bucketing (other than keepLast) counts buckets-that-contain-backups, not
// calendar buckets — a holiday gap doesn't shrink the retained set. All
// bucketing is done in local time.
//
// See the README for worked examples and the full design rationale.
class RetentionPolicy {
    Q_GADGET
    QML_VALUE_TYPE(retentionPolicy)

    Q_PROPERTY(int keepLast    MEMBER keepLast)
    Q_PROPERTY(int keepDaily   MEMBER keepDaily)
    Q_PROPERTY(int keepWeekly  MEMBER keepWeekly)
    Q_PROPERTY(int keepMonthly MEMBER keepMonthly)
    Q_PROPERTY(int keepYearly  MEMBER keepYearly)

public:
    int keepLast    = 0; // N most recent backups overall, no time-bucketing
    int keepDaily   = 0; // latest of each of the N most recent days-with-backups
    int keepWeekly  = 0; // latest of each of the N most recent ISO-weeks-with-backups
    int keepMonthly = 0; // latest of each of the N most recent calendar-months-with-backups
    int keepYearly  = 0; // latest of each of the N most recent calendar-years-with-backups

    bool operator==(const RetentionPolicy &other) const
    {
        return keepLast == other.keepLast
            && keepDaily == other.keepDaily
            && keepWeekly == other.keepWeekly
            && keepMonthly == other.keepMonthly
            && keepYearly == other.keepYearly;
    }
    bool operator!=(const RetentionPolicy &other) const { return !(*this == other); }
};

} // namespace QtCloudBackup

Q_DECLARE_METATYPE(QtCloudBackup::RetentionPolicy)
