// SPDX-License-Identifier: GPL-3.0-only
#include "AuroraMore.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>
#include <QWindow>

#include <cstdio>
#include <cstring>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#endif

class AuroraBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap state READ state NOTIFY stateChanged)

public:
    explicit AuroraBridge(bool windowsSession) : windowsSession(windowsSession)
    {
        // Anonymous pipes on Windows cannot be watched by QSocketNotifier.
        connect(&pollTimer, &QTimer::timeout, this, &AuroraBridge::readInput);
        pollTimer.start(25);
    }

    QVariantMap state() const { return snapshot; }

    Q_INVOKABLE void send(const QString &action, const QString &deviceId = {}, int deviceEpoch = 0)
    {
        sendRequest({{"action", action}, {"deviceId", deviceId}, {"deviceEpoch", deviceEpoch}});
    }

    Q_INVOKABLE void sendPreference(const QString &action, int requestId, const QVariant &value)
    {
        sendRequest({{"action", action}, {"deviceId", QString()}, {"deviceEpoch", 0},
            {"preferenceRequestId", requestId}, {"preferenceValue", QJsonValue::fromVariant(value)}});
    }

    Q_INVOKABLE void requestFullScreen(bool enabled)
    {
        if (windowsSession)
            sendRequest(QJsonObject{{"action", "session-fullscreen"}, {"fullScreen", enabled}});
    }

    Q_INVOKABLE void sendTailscale(const QString &action, const QString &target, int revision)
    {
        sendRequest({{"action", action}, {"accountTarget", target}, {"accountRevision", revision}});
    }

    Q_INVOKABLE void sendSunshine(const QString &action, const QVariantMap &data)
    {
        sendRequest({{"action", action}, {"sunshine", QJsonObject::fromVariantMap(data)}});
    }

signals:
    void stateChanged();

private:
    static void sendRequest(const QJsonObject &request)
    {
        const auto line = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
        const auto size = static_cast<size_t>(line.size());
        if (std::fwrite(line.constData(), 1, size, stdout) != size || std::fflush(stdout) != 0)
            QCoreApplication::quit();
    }

    void readInput()
    {
        char bytes[16384];
#ifdef Q_OS_WIN
        const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        DWORD available = 0;
        if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) {
            QCoreApplication::quit();
            return;
        }
        if (!available)
            return;
        DWORD count = 0;
        if (!ReadFile(input, bytes, qMin(available, DWORD(sizeof bytes)), &count, nullptr))
            count = 0;
#else
        pollfd input{STDIN_FILENO, POLLIN, 0};
        if (poll(&input, 1, 0) <= 0)
            return;
        const auto count = read(STDIN_FILENO, bytes, sizeof bytes);
        if (count < 0 && errno == EINTR)
            return;
#endif
        if (count <= 0) {
            QCoreApplication::quit();
            return;
        }
        pending.append(bytes, static_cast<int>(count));
        while (true) {
            const auto newline = pending.indexOf('\n');
            if (newline < 0)
                break;
            const auto document = QJsonDocument::fromJson(pending.left(newline));
            pending.remove(0, newline + 1);
            if (document.isObject() && document.object().contains(windowsSession ? "panelKind" : "deviceAccess")) {
                const auto next = document.object().toVariantMap();
                if (next != snapshot) {
                    snapshot = next;
                    emit stateChanged();
                }
            }
        }
        if (pending.size() > 8 * 1024 * 1024)
            QCoreApplication::quit();
    }

    const bool windowsSession;
    QTimer pollTimer;
    QByteArray pending;
    QVariantMap snapshot;
};

#ifdef Q_OS_WIN
static bool ownsSessionPopup(HWND popup, HWND owner)
{
    if (!popup || (GetWindowLongPtrW(popup, GWL_STYLE) & WS_CHILD))
        return false;
    if (GetWindow(popup, GW_OWNER) != owner)
        SetWindowLongPtrW(popup, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
    return GetWindow(popup, GW_OWNER) == owner;
}

static void connectSessionPopup(QGuiApplication &app, QWindow &popup, AuroraBridge &bridge, HWND owner)
{
    QObject::connect(&popup, &QWindow::visibleChanged, &app, [&app](bool visible) {
        if (!visible)
            app.quit();
    });
    QObject::connect(&bridge, &AuroraBridge::stateChanged, &popup,
                     [&app, &popup, &bridge, owner, shown = false]() mutable {
        if (shown)
            return;
        if (!IsWindowVisible(owner) || IsIconic(owner)) {
            app.exit(1);
            return;
        }
        const auto state = bridge.state();
        const int x = state.value("pixelX").toInt();
        const int y = state.value("pixelY").toInt();
        // Qt's show path can reset GW_OWNER. Set the native owner after showing
        // the transparent window, then reveal it at its final screen position.
        popup.setOpacity(0);
        popup.show();
        const auto hwnd = reinterpret_cast<HWND>(popup.winId());
        if (!ownsSessionPopup(hwnd, owner) ||
                !SetWindowPos(hwnd, HWND_TOP, x, y, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE)) {
            qWarning("Qt session popup ownership or position failed: %lu", GetLastError());
            app.exit(1);
            return;
        }
        shown = true;
        popup.setOpacity(1);
        QTimer::singleShot(0, &popup, [&popup, &bridge] {
            popup.setProperty("nativeReady", true);
            popup.requestActivate();
            bridge.send("session-shown");
        });
    });
}
#endif

int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    const bool windowsSession = argc > 1 && std::strcmp(argv[1], "--windows-session") == 0;
#else
    const bool windowsSession = false;
#endif
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QGuiApplication app(argc, argv);
#ifdef Q_OS_WIN
    QIcon icon(QStringLiteral(":/aurora/brand/Aurora.ico"));
    if (icon.availableSizes().isEmpty())
        icon = QIcon(QStringLiteral(":/aurora/brand/Aurora-Windows-32.png"));
    app.setWindowIcon(icon);
#endif
    const auto arguments = app.arguments();
    if (arguments.size() != (windowsSession ? 5 : 2)) {
        qWarning("Usage: AuroraQtShell <Live.qml> or --windows-session <popup.qml> <owner> <owner-pid>");
        return 2;
    }

#ifdef Q_OS_WIN
    HWND ownerHandle = nullptr;
    if (windowsSession) {
        bool windowOk = false;
        bool processOk = false;
        ownerHandle = reinterpret_cast<HWND>(static_cast<quintptr>(arguments.at(3).toLongLong(&windowOk)));
        const auto expectedPid = arguments.at(4).toUInt(&processOk);
        DWORD ownerPid = 0;
        if (!windowOk || !processOk || !GetWindowThreadProcessId(ownerHandle, &ownerPid) ||
                ownerPid != expectedPid || (GetWindowLongPtrW(ownerHandle, GWL_STYLE) & WS_CHILD)) {
            qWarning("Invalid native session popup owner.");
            return 2;
        }
    }
#endif

    AuroraBridge bridge(windowsSession);
    QNetworkAccessManager network;
    AuroraMore more(network, &QDesktopServices::openUrl);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("auroraBridge", &bridge);
    if (!windowsSession) {
        engine.rootContext()->setContextProperty("auroraMore", &more);
        QObject::connect(&bridge, &AuroraBridge::stateChanged, &more, [&bridge, &more] {
            more.setApplicationVersion(bridge.state().value(QStringLiteral("applicationVersion")).toString());
        });
    }
    engine.load(QUrl::fromLocalFile(arguments.at(windowsSession ? 2 : 1)));
    if (engine.rootObjects().isEmpty())
        return 1;
#ifdef Q_OS_WIN
    if (windowsSession) {
        auto popup = qobject_cast<QWindow *>(engine.rootObjects().constFirst());
        if (!popup)
            return 1;
        connectSessionPopup(app, *popup, bridge, ownerHandle);
    }
#endif
    return app.exec();
}

#include "main.moc"
