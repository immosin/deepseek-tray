#pragma once

#include <QString>

// Каталог данных приложения: %APPDATA%\DeepSeekTray
// (переопределяется переменной DSBAL_DATA_DIR — удобно для тестов).
QString appDataDir();

QString configFilePath();
QString uiStateFilePath();
QString logPath();
QString cacheFilePath(const QString &providerId);

void logLine(const QString &text);
