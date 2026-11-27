#pragma once

#include <QHash>
#include <QString>
#include <QVector>

struct BalanceInfo {
    QString currency;
    QString total;
    QString granted;
    QString toppedUp;
};

struct BalanceResult {
    bool ok = false;
    int httpStatus = 0;
    bool available = false;
    QVector<BalanceInfo> infos;
    QString error;
};

// Один источник баланса. Провайдеров может быть несколько: они описываются
// секциями [providers.<id>] в config.ini, тип определяет формат ответа API.
struct ProviderSettings {
    QString id;      // "deepseek" — используется в [general] provider=
    QString type;    // "deepseek" — определяет парсер ответа
    QString title;   // "DeepSeek" — подписи в интерфейсе
    QString url;
    QString key;
    QString console; // ссылка «личный кабинет»
    bool enabled = true;
    // Пороги уведомлений по валютам: "CNY" -> 10, "USD" -> 1.
    // Особый ключ "default" задаёт порог для всех валют провайдера.
    QHash<QString, double> thresholds;
};

struct ProviderSnapshot {
    ProviderSettings provider;
    BalanceResult result;
};

QString defaultProviderType();
QString defaultProviderTitle(const QString &type);
QString defaultProviderUrl(const QString &type);
QString defaultProviderConsole(const QString &type);
QString providerEnvVar(const QString &type);
bool isKnownProviderType(const QString &type);

BalanceResult fetchProvider(const ProviderSettings &provider);
