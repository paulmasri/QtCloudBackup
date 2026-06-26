#pragma once

#include <QDateTime>
#include <QDir>
#include <QRegularExpression>
#include <QString>
#include <QTimeZone>

inline bool isValidBackupFilename(const QString &filename)
{
    if (filename.size() < 5 || !filename.endsWith(QLatin1String(".bak")))
        return false;
    if (filename.contains(QLatin1Char('/')) || filename.contains(QLatin1Char('\\')))
        return false;
    if (QDir::cleanPath(filename) != filename)
        return false;

    static const QRegularExpression re(
        QStringLiteral("^qtcloudbackup_[a-zA-Z0-9_-]{1,64}_\\d{8}_\\d{6}_\\d{3}_[a-z0-9]{4}\\.bak$"));
    return re.match(filename).hasMatch();
}

inline QString backupStem(const QString &filename)
{
    if (filename.size() < 5 || !filename.endsWith(QLatin1String(".bak")))
        return {};
    return filename.chopped(4);
}

// Derive sourceId and a UTC timestamp from a backup filename alone, at the
// same millisecond precision as the .meta sidecar. Returns false (leaving the
// out-params untouched) if the name doesn't match the canonical pattern. This
// is the no-open, no-hydration path: everything it needs is encoded in the
// filename, so callers that only want identity + timestamp never touch the
// sidecar.
inline bool parseBackupFilename(const QString &filename, QString &sourceId, QDateTime &timestamp)
{
    static const QRegularExpression re(
        QStringLiteral("^qtcloudbackup_([a-zA-Z0-9_-]{1,64})_(\\d{8}_\\d{6}_\\d{3})_[a-z0-9]{4}\\.bak$"));
    const auto match = re.match(filename);
    if (!match.hasMatch())
        return false;
    sourceId = match.captured(1);
    timestamp = QDateTime::fromString(match.captured(2), QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    timestamp.setTimeZone(QTimeZone::utc());
    return true;
}
