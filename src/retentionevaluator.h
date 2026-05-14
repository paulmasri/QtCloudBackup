#pragma once

#include "backupinfo.h"
#include "retentionpolicy.h"

#include <QList>
#include <QSet>
#include <QString>

namespace QtCloudBackup::RetentionEvaluator {

struct Result {
    QSet<QString> toKeep;   // filenames the policy keeps (confirmed entries only)
    QSet<QString> toDelete; // filenames the policy prunes (confirmed entries only)
    bool safetyNetEngaged = false; // true when the min-keep invariant overrode an all-pruning policy
};

// Evaluates the union-of-keeps retention policy against a single source's
// backup list. The caller is responsible for filtering to one sourceId.
//
// Semantics (full design rationale in the README):
// - Each keepX rule independently selects a set to retain; the union is kept.
// - Bucketing (Daily/Weekly/Monthly/Yearly) counts buckets-that-contain-backups
//   in local time, not calendar buckets. A holiday gap doesn't shrink the
//   retained set.
// - keepLast is unbucketed: N most recent by timestamp.
// - Entries with metadataAvailable=false (cloud-syncing / evicted / orphan
//   .bak) are never pruned, but DO count toward bucket occupancy and toward
//   the min-keep safety net.
// - Minimum-keep safety invariant: if the policy would prune every confirmed
//   backup AND there are no pending entries preserving "we have backups",
//   force-keep the most recent confirmed and set safetyNetEngaged=true.
Result evaluate(const QList<BackupInfo> &backupsForSource,
                const QtCloudBackup::RetentionPolicy &policy);

} // namespace QtCloudBackup::RetentionEvaluator
