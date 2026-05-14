#include "retentionevaluator.h"

#include <QDate>
#include <QHash>
#include <QLoggingCategory>
#include <algorithm>

Q_LOGGING_CATEGORY(retentionLog, "qtcloudbackup.retention")

namespace QtCloudBackup::RetentionEvaluator {

namespace {

// A bucket key encodes the local-time slot a backup falls into for a given
// rule (day, week, month, year). Buckets are sortable so we can pick the N
// most recent occupied ones.
struct BucketKey {
    int a = 0; // year (or ISO-week year)
    int b = 0; // day-of-year, week number, month, or 0 for yearly
    bool operator==(const BucketKey &o) const { return a == o.a && b == o.b; }
    bool operator<(const BucketKey &o) const  { return a == o.a ? b < o.b : a < o.a; }
};

inline size_t qHash(const BucketKey &k, size_t seed = 0) noexcept
{
    return qHashMulti(seed, k.a, k.b);
}

using BucketFn = BucketKey (*)(const QDateTime &);

BucketKey dailyBucket(const QDateTime &ts)
{
    const QDate d = ts.toLocalTime().date();
    return { d.year(), d.dayOfYear() };
}

BucketKey weeklyBucket(const QDateTime &ts)
{
    int isoYear = 0;
    const int weekNumber = ts.toLocalTime().date().weekNumber(&isoYear);
    return { isoYear, weekNumber };
}

BucketKey monthlyBucket(const QDateTime &ts)
{
    const QDate d = ts.toLocalTime().date();
    return { d.year(), d.month() };
}

BucketKey yearlyBucket(const QDateTime &ts)
{
    const QDate d = ts.toLocalTime().date();
    return { d.year(), 0 };
}

// Adds to `keep` the filenames selected by a bucketed rule. Buckets are
// computed across ALL entries (so a pending-only bucket counts toward N),
// but the per-bucket "latest" selection only considers confirmed entries.
void selectBucketed(const QList<BackupInfo> &all, int n, BucketFn bucketOf,
                    QSet<QString> &keep)
{
    if (n <= 0)
        return;

    // Group confirmed entries by bucket, recording the latest in each.
    QHash<BucketKey, const BackupInfo *> latestConfirmedPerBucket;
    // Track which buckets have any backup at all (confirmed or pending).
    QSet<BucketKey> occupiedBuckets;

    for (const BackupInfo &b : all) {
        const BucketKey key = bucketOf(b.timestamp);
        occupiedBuckets.insert(key);
        if (!b.metadataAvailable)
            continue;
        auto it = latestConfirmedPerBucket.find(key);
        if (it == latestConfirmedPerBucket.end() || b.timestamp > it.value()->timestamp)
            latestConfirmedPerBucket.insert(key, &b);
    }

    // Take the N most recent occupied buckets and keep their confirmed leader.
    QList<BucketKey> sorted(occupiedBuckets.constBegin(), occupiedBuckets.constEnd());
    std::sort(sorted.begin(), sorted.end(), [](const BucketKey &a, const BucketKey &b) {
        return b < a; // newest first
    });
    const int take = std::min(n, int(sorted.size()));
    for (int i = 0; i < take; ++i) {
        auto it = latestConfirmedPerBucket.find(sorted[i]);
        if (it != latestConfirmedPerBucket.end())
            keep.insert(it.value()->filename);
    }
}

} // namespace

Result evaluate(const QList<BackupInfo> &backupsForSource,
                const QtCloudBackup::RetentionPolicy &policy)
{
    Result result;

    // Partition: confirmed candidates are eligible for prune; pending are
    // never pruned but participate in bucket occupancy.
    QList<BackupInfo> confirmed;
    QList<BackupInfo> pending;
    confirmed.reserve(backupsForSource.size());
    for (const BackupInfo &b : backupsForSource) {
        if (b.metadataAvailable)
            confirmed.append(b);
        else
            pending.append(b);
    }

    // keepLast: N most recent confirmed entries overall by timestamp.
    if (policy.keepLast > 0) {
        QList<BackupInfo> sorted = confirmed;
        std::sort(sorted.begin(), sorted.end(),
                  [](const BackupInfo &a, const BackupInfo &b) {
                      return a.timestamp > b.timestamp;
                  });
        const int take = std::min(policy.keepLast, int(sorted.size()));
        for (int i = 0; i < take; ++i)
            result.toKeep.insert(sorted[i].filename);
    }

    // Bucketed rules see all entries for occupancy, confirmed for selection.
    selectBucketed(backupsForSource, policy.keepDaily,   dailyBucket,   result.toKeep);
    selectBucketed(backupsForSource, policy.keepWeekly,  weeklyBucket,  result.toKeep);
    selectBucketed(backupsForSource, policy.keepMonthly, monthlyBucket, result.toKeep);
    selectBucketed(backupsForSource, policy.keepYearly,  yearlyBucket,  result.toKeep);

    // Confirmed entries not selected by any rule are candidates for deletion.
    for (const BackupInfo &b : confirmed) {
        if (!result.toKeep.contains(b.filename))
            result.toDelete.insert(b.filename);
    }

    // Min-keep safety invariant. Pending entries already preserve "we have
    // backups", so they satisfy the safety net implicitly. Engage only when
    // the policy would delete all confirmed AND there are no pending.
    if (result.toKeep.isEmpty() && !confirmed.isEmpty() && pending.isEmpty()) {
        auto latest = std::max_element(confirmed.cbegin(), confirmed.cend(),
                                       [](const BackupInfo &a, const BackupInfo &b) {
                                           return a.timestamp < b.timestamp;
                                       });
        result.toKeep.insert(latest->filename);
        result.toDelete.remove(latest->filename);
        result.safetyNetEngaged = true;
        qCWarning(retentionLog,
                  "RetentionPolicy would prune all backups for sourceId %s; "
                  "retaining latest as min-keep safety. Check the configured policy.",
                  qPrintable(latest->sourceId));
    }

    return result;
}

} // namespace QtCloudBackup::RetentionEvaluator
