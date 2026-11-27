#include "provider.h"

#include "appdirs.h"
#include "http.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

BalanceResult parseDeepSeek(const std::string &payload, int status)
{
    BalanceResult result;
    result.httpStatus = status;

    QJsonParseError error{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(QByteArray::fromStdString(payload), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        result.error = QStringLiteral("некорректный JSON: ") + error.errorString();
        return result;
    }

    const QJsonObject root = doc.object();
    result.available = root.value(QStringLiteral("is_available")).toBool(false);
    const QJsonArray infos = root.value(QStringLiteral("balance_infos")).toArray();
    for (const QJsonValue &value : infos) {
        const QJsonObject object = value.toObject();
        BalanceInfo info;
        info.currency = object.value(QStringLiteral("currency")).toString();
        info.total = object.value(QStringLiteral("total_balance")).toString();
        info.granted = object.value(QStringLiteral("granted_balance")).toString();
        info.toppedUp = object.value(QStringLiteral("topped_up_balance")).toString();
        result.infos.append(info);
    }
    result.ok = true;
    return result;
}

void saveCache(const QString &providerId, const std::string &payload)
{
    QFile file(cacheFilePath(providerId));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(payload.data(), static_cast<qint64>(payload.size()));
}

} // namespace

QString defaultProviderType()
{
    return QStringLiteral("deepseek");
}

QString defaultProviderTitle(const QString &type)
{
    if (type == QLatin1String("deepseek"))
        return QStringLiteral("DeepSeek");
    if (type == QLatin1String("moonshot"))
        return QStringLiteral("Kimi");
    return type;
}

QString defaultProviderUrl(const QString &type)
{
    if (type == QLatin1String("deepseek"))
        return QStringLiteral("https://api.deepseek.com/user/balance");
    if (type == QLatin1String("moonshot"))
        return QStringLiteral("https://api.moonshot.ai/v1/users/me/balance");
    return QString();
}

QString defaultProviderConsole(const QString &type)
{
    if (type == QLatin1String("deepseek"))
        return QStringLiteral("https://platform.deepseek.com/usage");
    if (type == QLatin1String("moonshot"))
        return QStringLiteral("https://platform.kimi.ai/console");
    return QString();
}

QString providerEnvVar(const QString &type)
{
    if (type == QLatin1String("deepseek"))
        return QStringLiteral("DEEPSEEK_API_KEY");
    if (type == QLatin1String("moonshot"))
        return QStringLiteral("MOONSHOT_API_KEY");
    return QString();
}

bool isKnownProviderType(const QString &type)
{
    // Kimi (moonshot) появится вместе с пунктом из «Планов».
    return type == QLatin1String("deepseek");
}

BalanceResult fetchProvider(const ProviderSettings &provider)
{
    BalanceResult result;

    if (!isKnownProviderType(provider.type)) {
        result.error = QStringLiteral("провайдер типа \"%1\" пока не поддерживается")
                           .arg(provider.type);
        return result;
    }
    if (provider.key.trimmed().isEmpty()) {
        result.error = QStringLiteral("не задан ключ API для ") + provider.title;
        return result;
    }
    if (provider.url.trimmed().isEmpty()) {
        result.error = QStringLiteral("не задан адрес API для ") + provider.title;
        return result;
    }

    const HttpResult http = httpGet(provider.url.toStdString(), provider.key.toStdString());
    if (http.status == 0) {
        result.error = QString::fromStdString(http.error);
        return result;
    }
    if (http.status == 401) {
        result.httpStatus = 401;
        result.error = QStringLiteral("HTTP 401: ключ не принят");
        return result;
    }
    if (!http.ok) {
        result.httpStatus = http.status;
        result.error = QStringLiteral("HTTP %1: %2")
                           .arg(http.status)
                           .arg(QString::fromStdString(http.body).left(200));
        return result;
    }

    saveCache(provider.id, http.body);
    return parseDeepSeek(http.body, http.status);
}
