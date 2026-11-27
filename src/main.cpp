// DeepSeekTray — баланс API в системном трее Windows.
//
// Источники данных описываются в config.ini секциями [providers.<id>].
// Пока поддержан один тип — deepseek; архитектура рассчитана на несколько.
//
// Режимы:
//   DeepSeekTray.exe                        — иконка в трее и виджет
//   DeepSeekTray.exe --check                — один запрос активного провайдера
//   DeepSeekTray.exe --check --provider id  — проверка конкретного провайдера
//   DeepSeekTray.exe --check --url URL      — запрос на произвольный URL (отладка)
//   DeepSeekTray.exe --render DIR           — сохранить иконки и виджет в PNG

#include <QApplication>
#include <QGuiApplication>
#include <QCoreApplication>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>
#include <QTimer>
#include <QFrame>
#include <QLabel>
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QScreen>
#include <QIcon>
#include <QPixmap>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QFont>
#include <QFontDatabase>
#include <QSet>
#include <QHash>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSettings>
#include <QDateTime>
#include <QSharedMemory>
#include <QDesktopServices>
#include <QUrl>
#include <QThreadPool>
#include <QRunnable>
#include <QPointer>
#include <QDebug>

#include <cstdio>
#include <functional>
#include <string>

#include "appdirs.h"
#include "provider.h"
#include "settings.h"

namespace {

constexpr int kRetryMs = 60 * 1000;

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
        for (const QString &family : families)
            set.insert(family);
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

    const qreal inset = qMax(0.0, size * 0.02);
    const qreal radius = size * 0.20;
    p.setPen(Qt::NoPen);
    p.setBrush(problem ? QColor(150, 32, 32) : QColor(24, 24, 28));
    p.drawRoundedRect(QRectF(inset, inset, size - 2 * inset, size - 2 * inset), radius, radius);

    QFont f = QApplication::font();
    const QString condensed = condensedFamily();
    if (!condensed.isEmpty()) {
        f.setFamily(condensed);
        f.setBold(false); // SemiBold Condensed уже полужирный
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
    for (int size : {16, 20, 24, 32, 48, 64})
        icon.addPixmap(renderIcon(text, size, problem));
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

// Валюта из настроек; auto — первая в ответе.
const BalanceInfo *pickInfo(const BalanceResult &result, const QString &currency)
{
    if (result.infos.isEmpty())
        return nullptr;
    if (currency.compare(QLatin1String("auto"), Qt::CaseInsensitive) != 0) {
        for (const BalanceInfo &info : result.infos) {
            if (info.currency.compare(currency, Qt::CaseInsensitive) == 0)
                return &info;
        }
    }
    return &result.infos.first();
}

} // namespace

// ---------------------------------------------------------------------------
// Виджет «часы»: плавающая плашка с балансом.
// ---------------------------------------------------------------------------
class BalanceWidget : public QFrame
{
    Q_OBJECT
public:
    explicit BalanceWidget(QWidget *parent = nullptr);

    void setMenu(QMenu *menu) { m_menu = menu; }
    void setBalance(const QString &value, const QString &caption, bool problem);
    void restorePosition();
    void placeNearClock();

signals:
    void refreshRequested();
    void positionMoved(const QPoint &pos);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    QLabel *m_value = nullptr;
    QLabel *m_caption = nullptr;
    QMenu *m_menu = nullptr;
    QColor m_plate = QColor(18, 18, 22, 235);
    QPoint m_pressGlobal;
    QPoint m_pressWindow;
    bool m_dragging = false;
};

BalanceWidget::BalanceWidget(QWidget *parent)
    : QFrame(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
{
    setObjectName(QStringLiteral("balanceWidget"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_StyledBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setWindowFlag(Qt::WindowDoesNotAcceptFocus, true);
    setWindowTitle(QStringLiteral("DeepSeekTray"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(15, 6, 15, 8);
    layout->setSpacing(0);

    m_value = new QLabel(this);
    m_value->setObjectName(QStringLiteral("value"));
    m_value->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    m_caption = new QLabel(this);
    m_caption->setObjectName(QStringLiteral("caption"));
    m_caption->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    layout->addWidget(m_value);
    layout->addWidget(m_caption);

    setFixedSize(132, 58);
    setBalance(QStringLiteral("—"), QStringLiteral("DeepSeek"), false);
}

void BalanceWidget::setBalance(const QString &value, const QString &caption, bool problem)
{
    if (m_value->text() != value)
        m_value->setText(value);
    if (m_caption->text() != caption)
        m_caption->setText(caption);

    m_plate = problem ? QColor(122, 28, 28, 235) : QColor(18, 18, 22, 235);

    const QString sheet =
        QStringLiteral("QLabel#value { color: #ffffff; font-size: 24px; font-weight: bold; }"
                       "QLabel#caption { color: #9aa0a6; font-size: 11px; }");
    if (styleSheet() != sheet)
        setStyleSheet(sheet);

    update();
}

void BalanceWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF plate = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen(QColor(255, 255, 255, 30), 1));
    painter.setBrush(m_plate);
    painter.drawRoundedRect(plate, 10, 10);
}

void BalanceWidget::placeNearClock()
{
    const QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;
    const QRect avail = screen->availableGeometry();
    move(avail.right() - width() - 12, avail.bottom() - height() - 12);
}

void BalanceWidget::restorePosition()
{
    QSettings ui(uiStateFilePath(), QSettings::IniFormat);
    const int x = ui.value(QStringLiteral("ui/widgetX"), -1).toInt();
    const int y = ui.value(QStringLiteral("ui/widgetY"), -1).toInt();
    if (x >= 0 && y >= 0) {
        const QPoint saved(x, y);
        for (const QScreen *screen : QGuiApplication::screens()) {
            if (screen->availableGeometry().adjusted(-30, -30, 30, 30).contains(saved)) {
                move(saved);
                return;
            }
        }
    }
    placeNearClock();
}

void BalanceWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_pressGlobal = event->globalPos();
        m_pressWindow = pos();
        m_dragging = false;
    }
    QFrame::mousePressEvent(event);
}

void BalanceWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (event->buttons() & Qt::LeftButton) {
        const QPoint delta = event->globalPos() - m_pressGlobal;
        if (!m_dragging && delta.manhattanLength() > 3)
            m_dragging = true;
        if (m_dragging)
            move(m_pressWindow + delta);
    }
    QFrame::mouseMoveEvent(event);
}

void BalanceWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        if (m_dragging)
            emit positionMoved(pos());
        else
            emit refreshRequested();
        m_dragging = false;
    }
    QFrame::mouseReleaseEvent(event);
}

void BalanceWidget::contextMenuEvent(QContextMenuEvent *event)
{
    if (m_menu)
        m_menu->popup(event->globalPos());
    event->accept();
}

// ---------------------------------------------------------------------------
class TrayApp : public QObject
{
    Q_OBJECT
public:
    explicit TrayApp(QObject *parent = nullptr);

    void start();

private:
    void reloadSettings();
    void refresh();
    void apply(const QVector<ProviderSnapshot> &snapshots);
    void updateUi();
    void maybeNotify();
    QString iconText() const;
    const ProviderSnapshot *activeSnapshot() const;

    QSystemTrayIcon m_tray;
    QMenu m_menu;
    QAction *m_actRefresh = nullptr;
    QAction *m_actSettings = nullptr;
    QAction *m_actWidget = nullptr;
    QAction *m_actConsole = nullptr;
    BalanceWidget *m_widget = nullptr;
    QTimer m_timer;
    AppSettings m_settings;
    QVector<ProviderSnapshot> m_snapshots;
    QDateTime m_updatedAt;
    QHash<QString, QDateTime> m_lastNotify; // provider/currency -> время уведомления
    bool m_fetching = false;
};

TrayApp::TrayApp(QObject *parent)
    : QObject(parent)
{
    ensureConfigFile();
    reloadSettings();

    m_tray.setContextMenu(&m_menu);
    m_tray.setToolTip(QStringLiteral("DeepSeekTray: загрузка…"));

    m_actRefresh = m_menu.addAction(QStringLiteral("Обновить"));
    connect(m_actRefresh, &QAction::triggered, this, &TrayApp::refresh);

    m_actSettings = m_menu.addAction(QStringLiteral("Открыть настройки"));
    connect(m_actSettings, &QAction::triggered, this, []() {
        ensureConfigFile();
        QDesktopServices::openUrl(QUrl::fromLocalFile(configFilePath()));
    });

    QSettings ui(uiStateFilePath(), QSettings::IniFormat);
    const bool widgetVisible = ui.value(QStringLiteral("ui/widgetVisible"), true).toBool();

    m_actWidget = m_menu.addAction(QStringLiteral("Виджет у часов"));
    m_actWidget->setCheckable(true);
    m_actWidget->setChecked(widgetVisible);

    m_actConsole = m_menu.addAction(QStringLiteral("Личный кабинет"));
    connect(m_actConsole, &QAction::triggered, this, [this]() {
        const ProviderSettings *provider = activeProvider(m_settings);
        if (provider && !provider->console.isEmpty())
            QDesktopServices::openUrl(QUrl(provider->console));
    });

    m_menu.addSeparator();
    QAction *actQuit = m_menu.addAction(QStringLiteral("Выход"));
    connect(actQuit, &QAction::triggered, qApp, &QCoreApplication::quit);

    m_widget = new BalanceWidget();
    m_widget->setMenu(&m_menu);
    connect(m_widget, &BalanceWidget::refreshRequested, this, &TrayApp::refresh);
    connect(m_widget, &BalanceWidget::positionMoved, this, [](const QPoint &pos) {
        QSettings state(uiStateFilePath(), QSettings::IniFormat);
        state.setValue(QStringLiteral("ui/widgetX"), pos.x());
        state.setValue(QStringLiteral("ui/widgetY"), pos.y());
    });
    connect(m_actWidget, &QAction::toggled, this, [this](bool on) {
        QSettings state(uiStateFilePath(), QSettings::IniFormat);
        state.setValue(QStringLiteral("ui/widgetVisible"), on);
        if (on) {
            m_widget->restorePosition();
            m_widget->show();
        } else {
            m_widget->hide();
        }
    });

    connect(&m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
                    refresh();
            });

    m_timer.setSingleShot(false);
    connect(&m_timer, &QTimer::timeout, this, &TrayApp::refresh);

    updateUi();
}

void TrayApp::reloadSettings()
{
    m_settings = loadSettings();
    m_timer.setInterval(m_settings.general.refreshMinutes * 60 * 1000);

    const ProviderSettings *provider = activeProvider(m_settings);
    if (m_actConsole) {
        m_actConsole->setEnabled(provider && !provider->console.isEmpty());
        m_actConsole->setText(provider && !provider->title.isEmpty()
                                  ? QStringLiteral("Личный кабинет %1").arg(provider->title)
                                  : QStringLiteral("Личный кабинет"));
    }
}

void TrayApp::start()
{
    m_tray.show();
    if (m_widget && m_actWidget && m_actWidget->isChecked()) {
        m_widget->restorePosition();
        m_widget->show();
    }
    refresh();
}

const ProviderSnapshot *TrayApp::activeSnapshot() const
{
    const ProviderSettings *provider = activeProvider(m_settings);
    if (!provider)
        return nullptr;
    for (const ProviderSnapshot &snapshot : m_snapshots) {
        if (snapshot.provider.id == provider->id)
            return &snapshot;
    }
    return nullptr;
}

void TrayApp::refresh()
{
    if (m_fetching)
        return;

    reloadSettings();
    const QVector<ProviderSettings> providers = enabledProviders(m_settings);

    if (providers.isEmpty()) {
        m_snapshots.clear();
        m_fetching = false;
        logLine(QStringLiteral("нет включённых провайдеров"));
        m_timer.start(kRetryMs);
        updateUi();
        return;
    }

    m_fetching = true;
    QPointer<TrayApp> self(this);

    auto task = [providers, self]() {
        QVector<ProviderSnapshot> snapshots;
        snapshots.reserve(providers.size());
        for (const ProviderSettings &provider : providers)
            snapshots.append(ProviderSnapshot{provider, fetchProvider(provider)});

        if (!self)
            return;
        QMetaObject::invokeMethod(self.data(), [self, snapshots]() {
            if (self)
                self->apply(snapshots);
        }, Qt::QueuedConnection);
    };

    QThreadPool::globalInstance()->start(QRunnable::create(std::move(task)));
}

void TrayApp::apply(const QVector<ProviderSnapshot> &snapshots)
{
    m_fetching = false;
    m_snapshots = snapshots;
    m_updatedAt = QDateTime::currentDateTime();

    const ProviderSnapshot *active = activeSnapshot();
    const bool ok = active && active->result.ok;
    m_timer.start(ok ? m_settings.general.refreshMinutes * 60 * 1000 : kRetryMs);

    for (const ProviderSnapshot &snapshot : m_snapshots) {
        if (snapshot.result.ok) {
            QStringList parts;
            for (const BalanceInfo &info : snapshot.result.infos)
                parts << QStringLiteral("%1 %2").arg(info.total, info.currency);
            logLine(QStringLiteral("%1: ok %2").arg(snapshot.provider.title, parts.join(QStringLiteral(", "))));
        } else {
            logLine(QStringLiteral("%1: %2").arg(snapshot.provider.title, snapshot.result.error));
        }
    }

    updateUi();
    maybeNotify();
}

void TrayApp::updateUi()
{
    const ProviderSettings *provider = activeProvider(m_settings);
    const ProviderSnapshot *active = activeSnapshot();

    const bool haveData = active && active->result.ok;
    const bool problem = !haveData || !active->result.available;
    m_tray.setIcon(buildIcon(iconText(), problem));

    QString tip;
    if (m_snapshots.isEmpty()) {
        tip = QStringLiteral("DeepSeekTray: нет данных");
        if (!provider)
            tip = QStringLiteral("DeepSeekTray: нет включённых провайдеров");
    } else {
        QStringList lines;
        for (const ProviderSnapshot &snapshot : m_snapshots) {
            if (snapshot.result.ok) {
                const BalanceInfo *info = pickInfo(snapshot.result, m_settings.general.currency);
                if (info)
                    lines << QStringLiteral("%1: %2 %3")
                                 .arg(snapshot.provider.title, info->total, info->currency);
            } else {
                lines << QStringLiteral("%1: %2")
                             .arg(snapshot.provider.title, snapshot.result.error.left(40));
            }
        }
        tip = lines.join(QStringLiteral("\n"));
        if (m_updatedAt.isValid())
            tip += QStringLiteral("\n") + m_updatedAt.toString(QStringLiteral("HH:mm"));
        if (haveData && !active->result.available)
            tip += QStringLiteral("\nбаланс недостаточен");
        if (tip.size() > 127)
            tip = tip.left(124) + QStringLiteral("...");
    }
    m_tray.setToolTip(tip);

    if (m_widget) {
        QString value = QStringLiteral("—");
        QString caption = provider ? provider->title : QStringLiteral("DeepSeekTray");
        if (haveData) {
            const BalanceInfo *info = pickInfo(active->result, m_settings.general.currency);
            if (info) {
                value = info->total;
                caption = info->currency;
                if (m_updatedAt.isValid())
                    caption += QStringLiteral(" · ") + m_updatedAt.toString(QStringLiteral("HH:mm"));
            }
        } else if (active) {
            caption = QStringLiteral("нет связи");
        }
        m_widget->setBalance(value, caption, problem);
        m_widget->setToolTip(provider ? provider->title : QString());
    }

    if (m_actRefresh)
        m_actRefresh->setEnabled(!m_fetching);
}

QString TrayApp::iconText() const
{
    const ProviderSnapshot *active = activeSnapshot();
    if (!active)
        return QStringLiteral("?");
    if (!active->result.ok)
        return active->result.error.isEmpty() ? QStringLiteral("…") : QStringLiteral("!");
    const BalanceInfo *info = pickInfo(active->result, m_settings.general.currency);
    if (!info)
        return QStringLiteral("?");
    return shortNumber(info->total);
}

namespace {

// Порог для валюты: свой в [thresholds.<id>], иначе default=, иначе общий из [settings].
// Если у провайдера заданы только конкретные валюты, остальные не контролируются.
double thresholdFor(const ProviderSettings &provider, const QString &currency, double global)
{
    const QString code = currency.toUpper();
    if (provider.thresholds.contains(code))
        return provider.thresholds.value(code);
    if (provider.thresholds.contains(QStringLiteral("default")))
        return provider.thresholds.value(QStringLiteral("default"));
    if (!provider.thresholds.isEmpty())
        return 0.0;
    return global;
}

} // namespace

void TrayApp::maybeNotify()
{
    const QDateTime now = QDateTime::currentDateTime();
    const double global = m_settings.general.lowBalanceThreshold;

    for (const ProviderSnapshot &snapshot : m_snapshots) {
        if (!snapshot.result.ok)
            continue;

        for (const BalanceInfo &info : snapshot.result.infos) {
            bool ok = false;
            const double value = info.total.toDouble(&ok);
            if (!ok)
                continue;

            const QString currency = info.currency.toUpper();
            const double threshold = thresholdFor(snapshot.provider, currency, global);
            const QString key = snapshot.provider.id + QLatin1Char('/') + currency;

            if (threshold <= 0 || value > threshold) {
                m_lastNotify.remove(key);
                continue;
            }
            if (m_lastNotify.contains(key) &&
                m_lastNotify.value(key).secsTo(now) < 6 * 3600)
                continue;

            m_lastNotify.insert(key, now);
            logLine(QStringLiteral("notify: %1 %2 %3 <= %4")
                        .arg(snapshot.provider.id)
                        .arg(info.total)
                        .arg(currency)
                        .arg(threshold));
            m_tray.showMessage(QStringLiteral("Низкий баланс"),
                               QStringLiteral("%1: %2 %3 (порог %4)")
                                   .arg(snapshot.provider.title)
                                   .arg(info.total)
                                   .arg(info.currency)
                                   .arg(threshold),
                               QSystemTrayIcon::Warning, 8000);
        }
    }
}

// ---------------------------------------------------------------------------
static int runCheck(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("DeepSeekTray"));

    QString url;
    QString providerId;
    for (int i = 1; i + 1 < argc; ++i) {
        if (qstrcmp(argv[i], "--url") == 0)
            url = QString::fromLocal8Bit(argv[i + 1]);
        else if (qstrcmp(argv[i], "--provider") == 0)
            providerId = QString::fromLocal8Bit(argv[i + 1]);
    }

    ensureConfigFile();
    const AppSettings settings = loadSettings();


    const ProviderSettings *chosen = nullptr;
    if (!providerId.isEmpty()) {
        for (const ProviderSettings &provider : settings.providers) {
            if (provider.id == providerId) {
                chosen = &provider;
                break;
            }
        }
        if (!chosen) {
            std::printf("ERROR: провайдер \"%s\" не найден\n", qPrintable(providerId));
            return 1;
        }
    } else {
        chosen = activeProvider(settings);
    }

    if (!chosen) {
        std::printf("ERROR: нет включённых провайдеров\n");
        return 1;
    }

    ProviderSettings provider = *chosen;
    if (!url.isEmpty())
        provider.url = url;

    const BalanceResult result = fetchProvider(provider);
    if (!result.ok) {
        std::printf("ERROR: %s\n", qPrintable(result.error));
        logLine(QStringLiteral("check ERR ") + result.error);
        return result.httpStatus == 401 ? 4 : 1;
    }

    QStringList parts;
    for (const BalanceInfo &info : result.infos) {
        parts << QStringLiteral("%1 %2 (granted %3, topped up %4)")
                     .arg(info.total, info.currency, info.granted, info.toppedUp);
    }
    std::printf("OK available=%d\n%s\n", result.available ? 1 : 0,
                qPrintable(parts.join(QStringLiteral("\n"))));
    logLine(QStringLiteral("check OK ") + provider.title + QStringLiteral(": ") +
            parts.join(QStringLiteral("; ")));
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

    for (const Sample &sample : samples) {
        for (int size : {16, 24, 32, 64}) {
            const QPixmap pm = renderIcon(sample.text, size, sample.problem);
            pm.save(QStringLiteral("%1/%2_%3.png").arg(dir, sample.name).arg(size));
        }
    }

    const struct { const char *name; const char *value; const char *caption; bool problem; } widgets[] = {
        {"widget_normal", "44.10", "CNY \xc2\xb7 23:52", false},
        {"widget_problem", "\xe2\x80\x94", "\xd0\xbd\xd0\xb5\xd1\x82 \xd1\x81\xd0\xb2\xd1\x8f\xd0\xb7\xd0\xb8", true},
    };
    for (const auto &spec : widgets) {
        BalanceWidget widget;
        widget.setBalance(QString::fromUtf8(spec.value), QString::fromUtf8(spec.caption), spec.problem);
        widget.ensurePolished();
        widget.show();
        QApplication::processEvents();

        QPixmap shot(widget.size());
        shot.fill(Qt::transparent);
        widget.render(&shot);

        const int scale = 3;
        const QImage scaled = shot.toImage().scaled(widget.width() * scale, widget.height() * scale,
                                                    Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        scaled.save(QStringLiteral("%1/%2.png").arg(dir, QLatin1String(spec.name)));

        QImage canvas(scaled.size(), QImage::Format_ARGB32);
        canvas.fill(QColor(58, 62, 68));
        QPainter painter(&canvas);
        painter.drawImage(0, 0, scaled);
        painter.end();
        canvas.save(QStringLiteral("%1/%2_on_bg.png").arg(dir, QLatin1String(spec.name)));
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

    const QString instanceKey =
        qEnvironmentVariable("DSBAL_INSTANCE", QStringLiteral("DeepSeekTray.SingleInstance"));
    QSharedMemory guard(instanceKey);
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
