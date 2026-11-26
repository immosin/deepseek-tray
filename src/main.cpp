// DeepSeekTray — баланс DeepSeek API в системном трее Windows.
//
// Транспорт: WinHTTP (системный стек TLS), чтобы не зависеть от сборки Qt с OpenSSL.
//
// Ключ API берётся из (в порядке приоритета):
//   1) переменной окружения DEEPSEEK_API_KEY
//   2) %APPDATA%\DeepSeekTray\config.ini  ->  [api] key=sk-...
//   3) ~/.dsh/.credentials.yaml (refs -> DEEPSEEK_API_KEY), т.е. профиль DSH
//
// Режимы:
//   DeepSeekTray.exe                     — иконка в трее, опрос раз в 5 минут
//   DeepSeekTray.exe --check             — один запрос, результат в stdout/лог
//   DeepSeekTray.exe --check --url URL   — запрос на произвольный URL (отладка)
//   DeepSeekTray.exe --render DIR        — сохранить иконки в PNG (проверка отрисовки)

#include <QApplication>
#include <QGuiApplication>
#include <QCoreApplication>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>
#include <QTimer>
#include <QIcon>
#include <QPixmap>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QFont>
#include <QFontDatabase>
#include <QSet>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSettings>
#include <QRegularExpression>
#include <QDateTime>
#include <QStandardPaths>
#include <QSharedMemory>
#include <QDesktopServices>
#include <QUrl>
#include <QThreadPool>
#include <QRunnable>
#include <QPointer>
#include <QDebug>

#include <windows.h>
#include <winhttp.h>

#include <cstdio>
#include <functional>
#include <string>

namespace {

constexpr int kRefreshMs = 5 * 60 * 1000;
constexpr int kRetryMs = 60 * 1000;

QString defaultBalanceUrl()
{
    const QByteArray env = qgetenv("DSBAL_URL");
    if (!env.trimmed().isEmpty())
        return QString::fromUtf8(env).trimmed();
    return QStringLiteral("https://api.deepseek.com/user/balance");
}

const QString kUsageUrl = QStringLiteral("https://platform.deepseek.com/usage");

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

struct HttpResult {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string error;
};

QString appDataDir()
{
    const QByteArray override = qgetenv("DSBAL_DATA_DIR");
    if (!override.trimmed().isEmpty()) {
        const QString dir = QString::fromUtf8(override).trimmed();
        QDir().mkpath(dir);
        return dir;
    }
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir;
}

QString logPath() { return appDataDir() + QStringLiteral("/tray.log"); }
QString lastBalancePath() { return appDataDir() + QStringLiteral("/last-balance.json"); }

void logLine(const QString &text)
{
    const QString path = logPath();
    const QFileInfo fi(path);
    if (fi.exists() && fi.size() > 512 * 1024)
        QFile::remove(path);
    QFile f(path);
    if (!f.open(QIODevice::Append | QIODevice::Text))
        return;
    f.write(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss ")).toUtf8());
    f.write(text.toUtf8());
    f.write("\n");
}

QString resolveApiKey()
{
    const QByteArray envKey = qgetenv("DEEPSEEK_API_KEY");
    if (!envKey.trimmed().isEmpty())
        return QString::fromUtf8(envKey).trimmed();

    QSettings cfg(QSettings::IniFormat, QSettings::UserScope,
                  QStringLiteral("DeepSeekTray"), QStringLiteral("config"));
    const QString cfgKey = cfg.value(QStringLiteral("api/key")).toString().trimmed();
    if (!cfgKey.isEmpty())
        return cfgKey;

    QFile dsh(QDir::homePath() + QStringLiteral("/.dsh/.credentials.yaml"));
    if (dsh.open(QIODevice::ReadOnly)) {
        const QString text = QString::fromUtf8(dsh.readAll());
        static const QRegularExpression re(QStringLiteral("DEEPSEEK_API_KEY\\s*:\\s*(sk-[A-Za-z0-9]+)"));
        const QRegularExpressionMatch m = re.match(text);
        if (m.hasMatch())
            return m.captured(1);
    }
    return QString();
}

// ---------------------------------------------------------------------------
// WinHTTP: синхронный GET, вызывается из рабочего потока пула.
// ---------------------------------------------------------------------------
std::string lastWinError(DWORD code)
{
    wchar_t *buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::string out;
    if (buf) {
        const int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
        if (len > 1) {
            out.resize(static_cast<size_t>(len - 1));
            WideCharToMultiByte(CP_UTF8, 0, buf, -1, &out[0], len, nullptr, nullptr);
        }
        LocalFree(buf);
    }
    if (out.empty())
        out = "error " + std::to_string(code);
    return out;
}

std::wstring toWide(const std::string &utf8)
{
    if (utf8.empty())
        return std::wstring();
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring out(static_cast<size_t>(len > 0 ? len - 1 : 0), L'\0');
    if (len > 1)
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &out[0], len);
    return out;
}

HttpResult httpGet(const std::string &url, const std::string &bearer)
{
    HttpResult res;

    const std::wstring wurl = toWide(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]{};
    wchar_t path[2048]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;

    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        res.error = "WinHttpCrackUrl: " + lastWinError(GetLastError());
        return res;
    }

    HINTERNET session = WinHttpOpen(L"DeepSeekTray/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        session = WinHttpOpen(L"DeepSeekTray/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        res.error = "WinHttpOpen: " + lastWinError(GetLastError());
        return res;
    }

    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
    WinHttpSetTimeouts(session, 5000, 5000, 10000, 15000);

    HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0);
    if (!connect) {
        res.error = "WinHttpConnect: " + lastWinError(GetLastError());
        WinHttpCloseHandle(session);
        return res;
    }

    const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        res.error = "WinHttpOpenRequest: " + lastWinError(GetLastError());
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return res;
    }

    std::wstring headers = L"Accept: application/json\r\n";
    if (!bearer.empty())
        headers += L"Authorization: Bearer " + toWide(bearer) + L"\r\n";
    WinHttpAddRequestHeaders(request, headers.c_str(), static_cast<DWORD>(-1),
                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        res.error = "WinHttpSendRequest: " + lastWinError(GetLastError());
    } else if (!WinHttpReceiveResponse(request, nullptr)) {
        res.error = "WinHttpReceiveResponse: " + lastWinError(GetLastError());
    } else {
        DWORD status = 0;
        DWORD size = sizeof(status);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                 WINHTTP_NO_HEADER_INDEX)) {
            res.error = "WinHttpQueryHeaders: " + lastWinError(GetLastError());
        } else {
            res.status = static_cast<int>(status);
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(request, &avail) && avail > 0) {
                std::string chunk(avail, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(request, &chunk[0], avail, &read))
                    break;
                chunk.resize(read);
                res.body += chunk;
                if (res.body.size() > (1u << 20))
                    break;
            }
            res.ok = (status >= 200 && status < 300);
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return res;
}

BalanceResult parseBalance(const std::string &payload, int status)
{
    BalanceResult r;
    r.httpStatus = status;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(payload), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        r.error = QStringLiteral("bad json: ") + err.errorString();
        return r;
    }
    const QJsonObject root = doc.object();
    r.available = root.value(QStringLiteral("is_available")).toBool(false);
    const QJsonArray infos = root.value(QStringLiteral("balance_infos")).toArray();
    for (const QJsonValue &v : infos) {
        const QJsonObject o = v.toObject();
        BalanceInfo bi;
        bi.currency = o.value(QStringLiteral("currency")).toString();
        bi.total = o.value(QStringLiteral("total_balance")).toString();
        bi.granted = o.value(QStringLiteral("granted_balance")).toString();
        bi.toppedUp = o.value(QStringLiteral("topped_up_balance")).toString();
        r.infos.append(bi);
    }
    r.ok = true;
    return r;
}

void saveLastBalance(const std::string &payload)
{
    QFile f(lastBalancePath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(payload.data(), static_cast<qint64>(payload.size()));
}

// Возвращает разобранный результат для любого исхода запроса.
BalanceResult fetchBalance(const QString &url, const QString &key)
{
    BalanceResult r;
    bool hasBearer = !key.isEmpty();
    if (!hasBearer) {
        // без ключа запрос бессмысленен, но пусть попробует — вернёт 401
        r.error = QStringLiteral("API key not found");
    }

    const HttpResult http = httpGet(url.toStdString(), key.toStdString());
    if (http.status == 0) {
        r.error = QString::fromStdString(http.error);
        return r;
    }
    if (http.status == 401) {
        r.error = QStringLiteral("HTTP 401: ключ не принят");
        r.httpStatus = 401;
        return r;
    }
    if (!http.ok) {
        r.error = QStringLiteral("HTTP %1: %2").arg(http.status).arg(
            QString::fromStdString(http.body).left(200));
        r.httpStatus = http.status;
        return r;
    }

    saveLastBalance(http.body);
    return parseBalance(http.body, http.status);
}

// ---------------------------------------------------------------------------
// Иконка: число белым с тёмной обводкой — читается на светлой и тёмной панели.
// ---------------------------------------------------------------------------
// Узкий шрифт позволяет сделать цифры выше при той же ширине иконки.
QString condensedFamily()
{
    static const QStringList candidates = {
        QStringLiteral("Bahnschrift SemiBold Condensed"),
        QStringLiteral("Bahnschrift Condensed"),
        QStringLiteral("Arial Narrow"),
    };
    static const QSet<QString> available = [] {
        QSet<QString> set;
        const QStringList families = QFontDatabase().families();
        for (const QString &f : families)
            set.insert(f);
        return set;
    }();
    for (const QString &candidate : candidates) {
        if (available.contains(candidate))
            return candidate;
    }
    return QString();
}

QPixmap renderIcon(const QString &text, int size, bool problem)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);

    // Тёмная «таблетка» — цифры читаются и на светлой, и на тёмной панели.
    const qreal inset = qMax(0.0, size * 0.02);
    const qreal radius = size * 0.20;
    p.setPen(Qt::NoPen);
    p.setBrush(problem ? QColor(150, 32, 32) : QColor(24, 24, 28));
    p.drawRoundedRect(QRectF(inset, inset, size - 2 * inset, size - 2 * inset), radius, radius);

    QFont f = QApplication::font();
    const QString condensed = condensedFamily();
    if (!condensed.isEmpty()) {
        f.setFamily(condensed);
        f.setBold(false);   // SemiBold Condensed уже полужирный
    } else {
        f.setBold(true);
    }

    QPainterPath path;
    const qreal maxW = size * 0.90;
    const qreal maxH = size * 0.82;
    int px = qMax(6, qRound(size * 1.35));
    for (; px >= 5; --px) {
        f.setPixelSize(px);
        path = QPainterPath();
        path.addText(QPointF(0, 0), f, text);
        const QRectF br = path.boundingRect();
        if (br.width() <= maxW && br.height() <= maxH)
            break;
    }

    const QRectF br = path.boundingRect();
    path.translate((size - br.width()) / 2.0 - br.left(),
                   (size - br.height()) / 2.0 - br.top());

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255));
    p.drawPath(path);
    p.end();
    return pm;
}

QIcon buildIcon(const QString &text, bool problem)
{
    QIcon icon;
    for (int s : {16, 20, 24, 32, 48, 64})
        icon.addPixmap(renderIcon(text, s, problem));
    return icon;
}

QString shortNumber(const QString &value)
{
    bool ok = false;
    const double v = value.toDouble(&ok);
    if (!ok)
        return QStringLiteral("?");
    if (v >= 1000.0)
        return QString::number(qRound(v / 100.0) / 10.0, 'f', 1) + QStringLiteral("k");
    if (v >= 100.0)
        return QString::number(qRound(v));
    if (v >= 10.0)
        return QString::number(v, 'f', 0);
    return QString::number(v, 'f', 1);
}

} // namespace

// ---------------------------------------------------------------------------
class TrayApp : public QObject
{
    Q_OBJECT
public:
    explicit TrayApp(QObject *parent = nullptr);

    void start();

private:
    void refresh();
    void apply(const BalanceResult &r);
    void updateUi();
    QString iconText() const;

    QSystemTrayIcon m_tray;
    QMenu m_menu;
    QAction *m_actRefresh = nullptr;
    QTimer m_timer;
    QString m_key;
    QString m_url;
    BalanceResult m_last;
    QDateTime m_updatedAt;
    bool m_fetching = false;
};

TrayApp::TrayApp(QObject *parent)
    : QObject(parent), m_url(defaultBalanceUrl())
{
    m_key = resolveApiKey();

    m_tray.setContextMenu(&m_menu);
    m_tray.setToolTip(QStringLiteral("DeepSeek: загрузка…"));

    m_actRefresh = m_menu.addAction(QStringLiteral("Обновить"));
    connect(m_actRefresh, &QAction::triggered, this, &TrayApp::refresh);

    QAction *actUsage = m_menu.addAction(QStringLiteral("Открыть platform.deepseek.com"));
    connect(actUsage, &QAction::triggered, this, []() {
        QDesktopServices::openUrl(QUrl(kUsageUrl));
    });

    m_menu.addSeparator();
    QAction *actQuit = m_menu.addAction(QStringLiteral("Выход"));
    connect(actQuit, &QAction::triggered, qApp, &QCoreApplication::quit);

    connect(&m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
                    refresh();
            });

    m_timer.setSingleShot(false);
    connect(&m_timer, &QTimer::timeout, this, &TrayApp::refresh);

    updateUi();
}

void TrayApp::start()
{
    m_tray.show();
    if (m_key.isEmpty())
        logLine(QStringLiteral("WARN: API key not found"));
    refresh();
}

void TrayApp::refresh()
{
    if (m_fetching)
        return;
    m_fetching = true;

    const QString url = m_url;
    const QString key = m_key;
    QPointer<TrayApp> self(this);

    auto task = [url, key, self]() {
        const BalanceResult r = fetchBalance(url, key);
        if (!self)
            return;
        QMetaObject::invokeMethod(self.data(), [self, r]() {
            if (self)
                self->apply(r);
        }, Qt::QueuedConnection);
    };

    QThreadPool::globalInstance()->start(QRunnable::create(std::move(task)));
}

void TrayApp::apply(const BalanceResult &r)
{
    m_fetching = false;
    m_last = r;
    if (r.ok) {
        m_updatedAt = QDateTime::currentDateTime();
        QStringList parts;
        for (const BalanceInfo &bi : r.infos)
            parts << QStringLiteral("%1 %2").arg(bi.total, bi.currency);
        logLine(QStringLiteral("OK available=%1 %2")
                    .arg(r.available ? QStringLiteral("yes") : QStringLiteral("no"),
                         parts.join(QStringLiteral(", "))));
        m_timer.start(kRefreshMs);
    } else {
        logLine(QStringLiteral("ERR ") + r.error);
        m_timer.start(kRetryMs);
    }
    updateUi();
}

QString TrayApp::iconText() const
{
    if (!m_last.ok)
        return m_last.error.isEmpty() ? QStringLiteral("…") : QStringLiteral("!");
    if (m_last.infos.isEmpty())
        return QStringLiteral("?");
    return shortNumber(m_last.infos.first().total);
}

void TrayApp::updateUi()
{
    const bool problem = m_last.ok ? !m_last.available : !m_last.error.isEmpty();
    m_tray.setIcon(buildIcon(iconText(), problem));

    QString tip = QStringLiteral("DeepSeek API\n");
    if (m_last.ok) {
        QStringList lines;
        for (const BalanceInfo &bi : m_last.infos)
            lines << QStringLiteral("%1 %2").arg(bi.total, bi.currency);
        tip += lines.join(QStringLiteral(" · "));
        tip += m_updatedAt.isValid()
                   ? QStringLiteral("\nобновлено ") + m_updatedAt.toString(QStringLiteral("HH:mm"))
                   : QStringLiteral("\nобновлено только что");
        if (!m_last.available)
            tip += QStringLiteral("\nбаланс недостаточен");
    } else {
        tip += m_last.error.isEmpty() ? QStringLiteral("нет данных") : m_last.error.left(100);
    }
    m_tray.setToolTip(tip);
    if (m_actRefresh)
        m_actRefresh->setEnabled(!m_fetching);
}

// ---------------------------------------------------------------------------
static int runCheck(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("DeepSeekTray"));

    QString url = defaultBalanceUrl();
    for (int i = 1; i + 1 < argc; ++i) {
        if (qstrcmp(argv[i], "--url") == 0)
            url = QString::fromLocal8Bit(argv[i + 1]);
    }

    const BalanceResult r = fetchBalance(url, resolveApiKey());
    if (!r.ok) {
        std::printf("ERROR: %s\n", qPrintable(r.error));
        logLine(QStringLiteral("check ERR ") + r.error);
        return r.httpStatus == 401 ? 4 : 1;
    }

    QStringList parts;
    for (const BalanceInfo &bi : r.infos)
        parts << QStringLiteral("%1 %2 (granted %3, topped up %4)")
                     .arg(bi.total, bi.currency, bi.granted, bi.toppedUp);
    std::printf("OK available=%d\n%s\n", r.available ? 1 : 0,
                qPrintable(parts.join(QStringLiteral("\n"))));
    logLine(QStringLiteral("check OK ") + parts.join(QStringLiteral("; ")));
    return 0;
}

static int runRender(int argc, char **argv)
{
    QApplication app(argc, argv);

    QString dir = QDir::tempPath();
    for (int i = 1; i + 1 < argc; ++i) {
        if (qstrcmp(argv[i], "--render") == 0)
            dir = QString::fromLocal8Bit(argv[i + 1]);
    }
    QDir().mkpath(dir);

    struct Sample { QString text; bool problem; QString name; };
    const QVector<Sample> samples = {
        {QStringLiteral("44"), false, QStringLiteral("normal_44")},
        {QStringLiteral("2.2"), false, QStringLiteral("normal_2_2")},
        {QStringLiteral("!"), true, QStringLiteral("error")},
    };

    for (const Sample &s : samples) {
        for (int size : {16, 24, 32, 64}) {
            const QPixmap pm = renderIcon(s.text, size, s.problem);
            pm.save(QStringLiteral("%1/%2_%3.png").arg(dir, s.name).arg(size));
        }
    }
    std::printf("rendered to %s\n", qPrintable(dir));
    return 0;
}

int main(int argc, char **argv)
{
    bool check = false;
    bool render = false;
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--check") == 0)
            check = true;
        if (qstrcmp(argv[i], "--render") == 0)
            render = true;
    }

    if (check)
        return runCheck(argc, argv);
    if (render)
        return runRender(argc, argv);

    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("DeepSeekTray"));
    QApplication::setQuitOnLastWindowClosed(false);

    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        logLine(QStringLiteral("FATAL: system tray is not available"));
        return 3;
    }

    QSharedMemory guard(QStringLiteral("DeepSeekTray.SingleInstance"));
    if (!guard.create(1)) {
        logLine(QStringLiteral("already running, exiting"));
        return 0;
    }

    TrayApp tray;
    tray.start();
    logLine(QStringLiteral("started"));
    const int rc = app.exec();
    QThreadPool::globalInstance()->waitForDone(2000);
    return rc;
}

#include "main.moc"
