#include "appdirs.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

QString appDataDir()
{
    const QByteArray override = qgetenv("DSBAL_DATA_DIR");
    QString dir;
    if (!override.trimmed().isEmpty())
        dir = QString::fromUtf8(override).trimmed();
    else
        dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir;
}

QString configFilePath()
{
    return appDataDir() + QStringLiteral("/config.ini");
}

QString uiStateFilePath()
{
    return appDataDir() + QStringLiteral("/ui.ini");
}

QString logPath()
{
    return appDataDir() + QStringLiteral("/tray.log");
}

QString cacheFilePath(const QString &providerId)
{
    return appDataDir() + QStringLiteral("/last-balance-") + providerId + QStringLiteral(".json");
}

void logLine(const QString &text)
{
    const QString path = logPath();
    const QFileInfo info(path);
    if (info.exists() && info.size() > 512 * 1024)
        QFile::remove(path);

    QFile file(path);
    if (!file.open(QIODevice::Append | QIODevice::Text))
        return;
    file.write(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss ")).toUtf8());
    file.write(text.toUtf8());
    file.write("\n");
}
