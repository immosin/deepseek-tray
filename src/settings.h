#pragma once

#include <QString>
#include <QVector>

#include "provider.h"

struct GeneralSettings {
    int refreshMinutes = 5;
    double lowBalanceThreshold = 10.0;
    QString currency = QStringLiteral("auto"); // auto | CNY | USD
    QString activeProvider;                    // id из [providers.*]
};

struct AppSettings {
    GeneralSettings general;
    QVector<ProviderSettings> providers;
};

// Создаёт config.ini с комментариями, если файла нет или он в старом формате [api].
void ensureConfigFile();

AppSettings loadSettings();

const ProviderSettings *activeProvider(const AppSettings &settings);
QVector<ProviderSettings> enabledProviders(const AppSettings &settings);
