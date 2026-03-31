#pragma once

#include <QDir>
#include <QRegularExpression>
#include <QString>

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
