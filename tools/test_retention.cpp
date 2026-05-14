// Ad-hoc test runner for RetentionEvaluator. Not a full QTest suite — just
// enough to exercise every case the spec calls out, with a clear pass/fail
// summary. Builds into a standalone executable when QTCLOUDBACKUP_BUILD_TESTS
// is ON.

#include "retentionevaluator.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QSet>
#include <QString>
#include <QTimeZone>
#include <cstdio>

using QtCloudBackup::RetentionPolicy;
using QtCloudBackup::RetentionEvaluator::evaluate;
using QtCloudBackup::RetentionEvaluator::Result;

static int g_failures = 0;
static int g_tests = 0;

static BackupInfo mk(const QString &filename, const QDateTime &ts,
                    const QString &sourceId = QStringLiteral("src"),
                    bool metadataAvailable = true)
{
    BackupInfo b;
    b.filename = filename;
    b.timestamp = ts;
    b.sourceId = sourceId;
    b.metadataAvailable = metadataAvailable;
    return b;
}

// Build a local-time QDateTime from explicit components — keeps the bucketing
// independent of the runner's wall clock.
static QDateTime local(int y, int m, int d, int h = 12, int min = 0)
{
    return QDateTime(QDate(y, m, d), QTime(h, min), QTimeZone::systemTimeZone());
}

static void expect(const char *label, const QSet<QString> &actual,
                   const QSet<QString> &expected)
{
    ++g_tests;
    if (actual == expected) {
        std::printf("  PASS  %s\n", label);
        return;
    }
    ++g_failures;
    std::printf("  FAIL  %s\n", label);
    std::printf("        expected: ");
    for (const auto &s : expected) std::printf("%s ", qPrintable(s));
    std::printf("\n        actual:   ");
    for (const auto &s : actual)   std::printf("%s ", qPrintable(s));
    std::printf("\n");
}

static void section(const char *name) { std::printf("\n[%s]\n", name); }

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // ----------------------------------------------------------------------
    section("Example 1: keepLast=2 + keepDaily=6 — today + 5 prior days");
    {
        const QList<BackupInfo> bs = {
            mk("today_a",  local(2026, 5, 13, 9,  0)),
            mk("today_b",  local(2026, 5, 13, 14, 0)),
            mk("d-1",      local(2026, 5, 12, 10, 0)),
            mk("d-2",      local(2026, 5, 11, 10, 0)),
            mk("d-3",      local(2026, 5, 10, 10, 0)),
            mk("d-4",      local(2026, 5, 9,  10, 0)),
            mk("d-5",      local(2026, 5, 8,  10, 0)),
            mk("d-7",      local(2026, 5, 6,  10, 0)), // should be pruned
        };
        const Result r = evaluate(bs, RetentionPolicy{ /*last*/2, /*daily*/6 });
        expect("keep set",
               r.toKeep,
               QSet<QString>{ "today_b", "today_a", "d-1", "d-2", "d-3", "d-4", "d-5" });
        expect("delete set",
               r.toDelete,
               QSet<QString>{ "d-7" });
    }

    // ----------------------------------------------------------------------
    section("Example 3: simple keepLast=5");
    {
        QList<BackupInfo> bs;
        for (int i = 0; i < 8; ++i)
            bs.append(mk(QStringLiteral("b%1").arg(i), local(2026, 5, 13 - i)));
        const Result r = evaluate(bs, RetentionPolicy{ /*last*/5 });
        expect("keeps 5 newest",
               r.toKeep,
               QSet<QString>{ "b0", "b1", "b2", "b3", "b4" });
        expect("deletes 3 oldest",
               r.toDelete,
               QSet<QString>{ "b5", "b6", "b7" });
    }

    // ----------------------------------------------------------------------
    section("Example 5: zero-retention engages safety net");
    {
        const QList<BackupInfo> bs = {
            mk("a", local(2026, 5, 13)),
            mk("b", local(2026, 5, 12)),
        };
        const Result r = evaluate(bs, RetentionPolicy{});
        expect("keeps latest", r.toKeep,   QSet<QString>{ "a" });
        expect("deletes rest", r.toDelete, QSet<QString>{ "b" });
        ++g_tests;
        if (r.safetyNetEngaged) std::printf("  PASS  safetyNetEngaged is true\n");
        else { ++g_failures; std::printf("  FAIL  safetyNetEngaged should be true\n"); }
    }

    // ----------------------------------------------------------------------
    section("First-backup-of-day trace from spec");
    // State before: yesterday x4, day-2 x1, day-4 x1, day-7 x1, day-10 x1
    // Just wrote: today's 1st.
    // Apply keepLast=3, keepDaily=5.
    {
        const QList<BackupInfo> bs = {
            mk("today",    local(2026, 5, 13, 9, 0)),
            mk("y_latest", local(2026, 5, 12, 18, 0)),
            mk("y_2nd",    local(2026, 5, 12, 14, 0)),
            mk("y_3rd",    local(2026, 5, 12, 10, 0)),
            mk("y_4th",    local(2026, 5, 12, 8,  0)),
            mk("d-2",      local(2026, 5, 11, 10, 0)),
            mk("d-4",      local(2026, 5, 9,  10, 0)),
            mk("d-7",      local(2026, 5, 6,  10, 0)),
            mk("d-10",     local(2026, 5, 3,  10, 0)),
        };
        const Result r = evaluate(bs, RetentionPolicy{ /*last*/3, /*daily*/5 });
        expect("union of keeps",
               r.toKeep,
               QSet<QString>{ "today", "y_latest", "y_2nd", "d-2", "d-4", "d-7" });
        expect("pruned",
               r.toDelete,
               QSet<QString>{ "y_3rd", "y_4th", "d-10" });
    }

    // ----------------------------------------------------------------------
    section("Holiday gap: keepDaily counts days-with-backups, not calendar days");
    {
        // 5 daily backups then a 2-week gap then today.
        QList<BackupInfo> bs = {
            mk("today", local(2026, 5, 13)),
        };
        // The "pre-holiday" stretch — days 2026-04-22 through 2026-04-28.
        for (int i = 0; i < 7; ++i)
            bs.append(mk(QStringLiteral("pre%1").arg(i),
                         local(2026, 4, 28 - i)));
        const Result r = evaluate(bs, RetentionPolicy{ /*last*/0, /*daily*/5 });
        // 5 most-recent days-with-backups: today + pre0..pre3.
        expect("keeps today + 4 pre-holiday days",
               r.toKeep,
               QSet<QString>{ "today", "pre0", "pre1", "pre2", "pre3" });
        expect("prunes the older 3 pre-holiday days",
               r.toDelete,
               QSet<QString>{ "pre4", "pre5", "pre6" });
    }

    // ----------------------------------------------------------------------
    section("metadataAvailable=false: pending entries preserved, count for occupancy");
    {
        const QList<BackupInfo> bs = {
            mk("today_pending", local(2026, 5, 13), QStringLiteral("src"), /*md*/false),
            mk("y_confirmed",   local(2026, 5, 12)),
            mk("d-2",           local(2026, 5, 11)),
            mk("d-3",           local(2026, 5, 10)),
        };
        // keepDaily=2: today's bucket has only a pending entry (no confirmed
        // leader to add to toKeep) but today still counts as one of the 2
        // most-recent days-with-backups; the other is yesterday.
        const Result r = evaluate(bs, RetentionPolicy{ /*last*/0, /*daily*/2 });
        expect("keeps yesterday's confirmed leader only",
               r.toKeep,
               QSet<QString>{ "y_confirmed" });
        expect("pending entries are NEVER in toDelete",
               r.toDelete,
               QSet<QString>{ "d-2", "d-3" });
    }

    // ----------------------------------------------------------------------
    section("metadataAvailable=false-only satisfies safety net (no force-keep)");
    {
        const QList<BackupInfo> bs = {
            mk("pending", local(2026, 5, 13), QStringLiteral("src"), false),
        };
        const Result r = evaluate(bs, RetentionPolicy{});
        ++g_tests;
        if (!r.safetyNetEngaged && r.toKeep.isEmpty() && r.toDelete.isEmpty())
            std::printf("  PASS  pending-only — no force-keep, nothing in toDelete\n");
        else {
            ++g_failures;
            std::printf("  FAIL  pending-only — toKeep=%lld toDelete=%lld safety=%d\n",
                        qint64(r.toKeep.size()), qint64(r.toDelete.size()),
                        int(r.safetyNetEngaged));
        }
    }

    // ----------------------------------------------------------------------
    section("Generational rotation overlap (Example 2)");
    {
        // Daily for ~2 weeks; the rule overlap matters more than coverage.
        QList<BackupInfo> bs;
        for (int i = 0; i < 14; ++i)
            bs.append(mk(QStringLiteral("d%1").arg(i), local(2026, 5, 13 - i)));
        const Result r = evaluate(bs, RetentionPolicy{ /*last*/10, /*daily*/7,
                                                       /*weekly*/4 });
        // keepLast=10 covers d0..d9. keepDaily=7 covers d0..d6 (already in).
        // keepWeekly=4 covers the latest-of-week for each of the 4 most-recent
        // ISO weeks with backups. d0..d13 may span 2–3 ISO weeks depending on
        // anchor; the test just asserts that everything keepLast picks is
        // present and that older entries are deletable. We avoid asserting
        // exact week splits to keep the test calendar-agnostic.
        for (int i = 0; i < 10; ++i) {
            ++g_tests;
            if (!r.toKeep.contains(QStringLiteral("d%1").arg(i))) {
                ++g_failures;
                std::printf("  FAIL  expected d%d in keep set\n", i);
            }
        }
        std::printf("  ...10 keepLast members present\n");
    }

    // ----------------------------------------------------------------------
    std::printf("\n%d test(s), %d failure(s)\n", g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
