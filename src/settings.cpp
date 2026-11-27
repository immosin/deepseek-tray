#include "settings.h"

#include "appdirs.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <cstdio>

namespace {

QString templateKey()
{
    // Старый формат: ключ лежал в [api] key=, либо в переменной окружения, либо в профиле DSH.
    QSettings legacy(configFilePath(), QSettings::IniFormat);
    // Переносим только ключ из старого [api] key=; ключи из окружения и профиля DSH
    // в файл не копируем — они подхватываются при загрузке настроек.
    return legacy.value(QStringLiteral("api/key")).toString().trimmed();
}

QString configTemplate(const QString &key)
{
    return QStringLiteral(
               "; DeepSeekTray — настройки.\n"
               "; Файл перечитывается перед каждым обновлением, перезапуск не нужен.\n"
               "\n"
               "[settings]\n"
               "; Период опроса баланса, минуты (1–1440)\n"
               "refresh_minutes=5\n"
               "; Уведомление, когда остаток ниже этого значения (0 — не уведомлять)\n"
               "low_balance_threshold=10\n"
               "; Валюта в иконке и виджете: auto, CNY или USD\n"
               "currency=auto\n"
               "; Активный провайдер: id из секций [providers.*]\n"
               "provider=deepseek\n"
               "\n"
               "[providers.deepseek]\n"
               "type=deepseek\n"
               "title=DeepSeek\n"
               "url=https://api.deepseek.com/user/balance\n"
               "console=https://platform.deepseek.com/usage\n"
               "key=%1\n"
               "enabled=true\n"
               "\n"
               "; Пороги уведомлений по валютам: секция [thresholds.<id провайдера>].\n"
               "; default= задаёт порог для всех валют провайдера.\n"
               "[thresholds.deepseek]\n"
               "CNY=10\n"
               "USD=1\n"
               "\n"
               "; Второй источник — появится вместе с поддержкой Kimi:\n"
               "; [providers.kimi]\n"
               "; type=moonshot\n"
               "; title=Kimi\n"
               "; url=https://api.moonshot.ai/v1/users/me/balance\n"
               "; console=https://platform.kimi.ai/console\n"
               "; key=\n"
               "; enabled=false\n")
        .arg(key);
}

QString resolveKey(const ProviderSettings &provider)
{
    if (!provider.key.trimmed().isEmpty())
        return provider.key.trimmed();

    const QString envName = providerEnvVar(provider.type);
    if (!envName.isEmpty()) {
        const QString fromEnv = QString::fromLocal8Bit(qgetenv(envName.toLocal8Bit().constData())).trimmed();
        if (!fromEnv.isEmpty())
            return fromEnv;
    }

    if (provider.type == QLatin1String("deepseek")) {
        QFile dsh(QDir::homePath() + QStringLiteral("/.dsh/.credentials.yaml"));
        if (dsh.open(QIODevice::ReadOnly)) {
            const QString text = QString::fromUtf8(dsh.readAll());
            static const QRegularExpression re(
                QStringLiteral("DEEPSEEK_API_KEY\\s*:\\s*(sk-[A-Za-z0-9]+)"));
            const QRegularExpressionMatch match = re.match(text);
            if (match.hasMatch())
                return match.captured(1);
        }
    }
    return QString();
}

} // namespace

void ensureConfigFile()
{
    const QString path = configFilePath();
    QFile existing(path);
    bool needsWrite = !existing.exists();

    if (!needsWrite && existing.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString text = QString::fromUtf8(existing.readAll());
        existing.close();
        const bool hasGeneral = text.contains(QStringLiteral("[settings]"), Qt::CaseInsensitive) ||
                                text.contains(QStringLiteral("[general]"), Qt::CaseInsensitive);
        const bool hasProviders = text.contains(QStringLiteral("[providers."), Qt::CaseInsensitive);
        if (!hasGeneral && !hasProviders)
            needsWrite = true;
    }

    if (!needsWrite)
        return;

    QFile out(path);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        out.write(configTemplate(templateKey()).toUtf8());
}

AppSettings loadSettings()
{
    AppSettings settings;
    QSettings cfg(configFilePath(), QSettings::IniFormat);

    // Qt пишет «корневые» ключи в секцию [General] и читает её как корень,
    // поэтому настройки живут в [settings], а корневые ключи читаем как запасной вариант.
    const auto readSetting = [&cfg](const char *name, const QVariant &fallback) {
        const QString key = QStringLiteral("settings/") + QLatin1String(name);
        if (cfg.contains(key))
            return cfg.value(key, fallback);
        return cfg.value(QLatin1String(name), fallback);
    };

    settings.general.refreshMinutes =
        readSetting("refresh_minutes", 5).toInt();
    if (settings.general.refreshMinutes < 1)
        settings.general.refreshMinutes = 1;
    if (settings.general.refreshMinutes > 1440)
        settings.general.refreshMinutes = 1440;

    settings.general.lowBalanceThreshold =
        readSetting("low_balance_threshold", 10.0).toDouble();
    if (settings.general.lowBalanceThreshold < 0)
        settings.general.lowBalanceThreshold = 0;

    const QString currency =
        readSetting("currency", QStringLiteral("auto")).toString().trimmed();
    if (currency.compare(QLatin1String("CNY"), Qt::CaseInsensitive) == 0)
        settings.general.currency = QStringLiteral("CNY");
    else if (currency.compare(QLatin1String("USD"), Qt::CaseInsensitive) == 0)
        settings.general.currency = QStringLiteral("USD");
    else
        settings.general.currency = QStringLiteral("auto");

    settings.general.activeProvider =
        readSetting("provider", QString()).toString().trimmed();

    const QStringList groups = cfg.childGroups();
    for (const QString &group : groups) {
        if (!group.startsWith(QLatin1String("providers.")))
            continue;

        ProviderSettings provider;
        provider.id = group.mid(int(qstrlen("providers.")));
        if (provider.id.isEmpty())
            continue;

        cfg.beginGroup(group);
        provider.type = cfg.value(QStringLiteral("type"), defaultProviderType())
                            .toString()
                            .trimmed()
                            .toLower();
        if (provider.type.isEmpty())
            provider.type = defaultProviderType();
        provider.title = cfg.value(QStringLiteral("title"), defaultProviderTitle(provider.type))
                             .toString()
                             .trimmed();
        if (provider.title.isEmpty())
            provider.title = defaultProviderTitle(provider.type);
        provider.url = cfg.value(QStringLiteral("url"), defaultProviderUrl(provider.type))
                           .toString()
                           .trimmed();
        provider.console = cfg.value(QStringLiteral("console"), defaultProviderConsole(provider.type))
                               .toString()
                               .trimmed();
        provider.key = cfg.value(QStringLiteral("key")).toString().trimmed();
        provider.enabled = cfg.value(QStringLiteral("enabled"), true).toBool();
        cfg.endGroup();

        const QString thresholdsGroup = QStringLiteral("thresholds.") + provider.id;
        if (cfg.childGroups().contains(thresholdsGroup)) {
            cfg.beginGroup(thresholdsGroup);
            const QStringList thresholdKeys = cfg.childKeys();
            for (const QString &key : thresholdKeys) {
                bool ok = false;
                const double value = cfg.value(key).toDouble(&ok);
                if (!ok)
                    continue;
                const QString name = key.trimmed().compare(QLatin1String("default"), Qt::CaseInsensitive) == 0
                                         ? QStringLiteral("default")
                                         : key.trimmed().toUpper();
                provider.thresholds.insert(name, value);
            }
            cfg.endGroup();
        }

        settings.providers.append(provider);
    }

    // Совместимость: конфиг старого формата с одним [api] key=.
    if (settings.providers.isEmpty()) {
        ProviderSettings provider;
        provider.id = QStringLiteral("deepseek");
        provider.type = defaultProviderType();
        provider.title = defaultProviderTitle(provider.type);
        provider.url = defaultProviderUrl(provider.type);
        provider.console = defaultProviderConsole(provider.type);
        provider.key = cfg.value(QStringLiteral("api/key")).toString().trimmed();
        provider.enabled = true;
        settings.providers.append(provider);
    }

    for (ProviderSettings &provider : settings.providers)
        provider.key = resolveKey(provider);

    return settings;
}

const ProviderSettings *activeProvider(const AppSettings &settings)
{
    for (const ProviderSettings &provider : settings.providers) {
        if (provider.id == settings.general.activeProvider && provider.enabled)
            return &provider;
    }
    for (const ProviderSettings &provider : settings.providers) {
        if (provider.enabled)
            return &provider;
    }
    return nullptr;
}

QVector<ProviderSettings> enabledProviders(const AppSettings &settings)
{
    QVector<ProviderSettings> result;
    for (const ProviderSettings &provider : settings.providers) {
        if (provider.enabled)
            result.append(provider);
    }
    return result;
}
