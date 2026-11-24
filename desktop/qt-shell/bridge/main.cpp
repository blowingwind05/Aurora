// SPDX-License-Identifier: GPL-3.0-only
#include "AuroraMore.h"
#include "../native/DesktopTray.h"
#include "../native/NativeDesktop.h"
#include "../native/NativeInstance.h"

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QFileInfo>
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
#include <memory>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#endif

#ifdef Q_OS_MACOS
bool installMacTitleBar(QWindow &window);
#endif

class AuroraBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap state READ state NOTIFY stateChanged)

public:
    AuroraBridge(bool windowsSession, bool checkUi, bool prepareMacTitleBar, bool native)
        : windowsSession(windowsSession), inProcess(native), pipeEnabled(!checkUi && !native)
    {
        // Let the first host snapshot show the Cocoa window after its titlebar is installed.
        if (checkUi || prepareMacTitleBar)
            snapshot.insert(QStringLiteral("windowVisible"), false);
        if (!pipeEnabled)
            return;
        // Anonymous pipes on Windows cannot be watched by QSocketNotifier.
        connect(&pollTimer, &QTimer::timeout, this, &AuroraBridge::readInput);
        pollTimer.start(25);
    }

    QVariantMap state() const { return snapshot; }

    void setState(const QVariantMap &next)
    {
        if (next == snapshot)
            return;
        snapshot = next;
        emit stateChanged();
    }

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
    void requestReceived(const QJsonObject &request);

private:
    void sendRequest(const QJsonObject &request)
    {
        if (inProcess) {
            emit requestReceived(request);
            return;
        }
        if (!pipeEnabled)
            return;
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
            if (document.isObject() && document.object().contains(windowsSession ? "panelKind" : "deviceAccess"))
                setState(document.object().toVariantMap());
        }
        if (pending.size() > 8 * 1024 * 1024)
            QCoreApplication::quit();
    }

    const bool windowsSession;
    const bool inProcess;
    const bool pipeEnabled;
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
    const bool checkUi = argc > 1 && std::strcmp(argv[1], "--check-ui") == 0;
    const bool checkNative = argc > 1 && std::strcmp(argv[1], "--check-native") == 0;
    const bool namedLauncher = argc == 1 &&
        QFileInfo(QString::fromLocal8Bit(argv[0])).completeBaseName().compare(QStringLiteral("Aurora"), Qt::CaseInsensitive) == 0;
    const bool native = namedLauncher || checkNative || (argc > 1 && std::strcmp(argv[1], "--native") == 0);
#ifdef Q_OS_WIN
    const bool windowsSession = argc > 1 && std::strcmp(argv[1], "--windows-session") == 0;
    const bool useDesktopTray = !native && !checkUi && !windowsSession;
#else
    const bool windowsSession = false;
    constexpr bool useDesktopTray = false;
#endif
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    std::unique_ptr<QGuiApplication> application;
    if (native || useDesktopTray)
        application = std::make_unique<QApplication>(argc, argv);
    else
        application = std::make_unique<QGuiApplication>(argc, argv);
    auto &app = *application;
#ifdef Q_OS_WIN
    QIcon icon(QStringLiteral(":/aurora/brand/Aurora.ico"));
    if (icon.availableSizes().isEmpty())
        icon = QIcon(QStringLiteral(":/aurora/brand/Aurora-Windows-32.png"));
    app.setWindowIcon(icon);
#endif
    const auto arguments = app.arguments();
    const bool validArguments = native ? arguments.size() <= 3
        : arguments.size() == (windowsSession ? 5 : checkUi ? 3 : 2);
    if (!validArguments) {
        qWarning("Usage: AuroraQtShell [--check-ui] <Live.qml>, --native [<Live.qml>], --check-native [<Live.qml>], "
                 "or --windows-session <popup.qml> <owner> <owner-pid>");
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

#ifdef Q_OS_MACOS
    const bool prepareMacTitleBar = QGuiApplication::platformName() == QStringLiteral("cocoa");
#else
    constexpr bool prepareMacTitleBar = false;
#endif
    AuroraBridge bridge(windowsSession, checkUi, prepareMacTitleBar, native);
    std::unique_ptr<DesktopTray> hostedTray;
    if (useDesktopTray) {
        QObject::connect(&bridge, &AuroraBridge::stateChanged, &app, [&bridge, &hostedTray] {
            const auto state = bridge.state();
            if (!state.value(QStringLiteral("trayRequested")).toBool()) {
                hostedTray.reset();
                return;
            }
            if (!hostedTray) {
                auto tray = std::make_unique<DesktopTray>();
                if (!tray->initialize())
                    return;
                QObject::connect(tray.get(), &DesktopTray::openRequested, &bridge,
                                 [&bridge] { bridge.send(QStringLiteral("tray-open")); });
                QObject::connect(tray.get(), &DesktopTray::settingsRequested, &bridge,
                                 [&bridge] { bridge.send(QStringLiteral("tray-settings")); });
                QObject::connect(tray.get(), &DesktopTray::quitRequested, &bridge,
                                 [&bridge] { bridge.send(QStringLiteral("tray-exit")); });
                hostedTray = std::move(tray);
                bridge.send(QStringLiteral("tray-ready"));
            }
            hostedTray->setVisible(state.value(QStringLiteral("trayActive")).toBool() &&
                                   state.value(QStringLiteral("trayVisible")).toBool());
        });
    }
    std::unique_ptr<NativeDesktop> nativeDesktop;
    std::unique_ptr<NativeInstance> instance;
    if (native) {
        QCoreApplication::setApplicationName(QStringLiteral("Aurora"));
        const auto dataDirectory = checkNative ? QString() : Aurora::Preferences::defaultDirectory();
        nativeDesktop = std::make_unique<NativeDesktop>(QStringLiteral(AURORA_DESKTOP_VERSION), dataDirectory);
        QObject::connect(&bridge, &AuroraBridge::requestReceived, nativeDesktop.get(), &NativeDesktop::handleRequest);
        QObject::connect(nativeDesktop.get(), &NativeDesktop::stateChanged, &bridge, &AuroraBridge::setState);
        bridge.setState(nativeDesktop->state());
        if (!checkNative) {
            instance = std::make_unique<NativeInstance>(dataDirectory);
            const auto result = instance->start();
            if (result == NativeInstance::Result::Activated)
                return 0;
            if (result == NativeInstance::Result::Error) {
                qWarning("Aurora could not start or activate the native desktop instance.");
                return 1;
            }
            QObject::connect(instance.get(), &NativeInstance::activationRequested, nativeDesktop.get(),
                             [&nativeDesktop] { nativeDesktop->activate(); });
        }
    }
    QNetworkAccessManager network;
    AuroraMore more(network, &QDesktopServices::openUrl);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("auroraBridge", &bridge);
    if (!windowsSession) {
        engine.rootContext()->setContextProperty("auroraMore", &more);
        QObject::connect(&bridge, &AuroraBridge::stateChanged, &more, [&bridge, &more] {
            more.setApplicationVersion(bridge.state().value(QStringLiteral("applicationVersion")).toString());
        });
        if (native)
            more.setApplicationVersion(nativeDesktop->state().value(QStringLiteral("applicationVersion")).toString());
    }
    const auto entry = native ? (arguments.size() == 3 ? arguments.at(2) : NativeDesktop::bundledEntryPoint())
        : arguments.at(windowsSession || checkUi ? 2 : 1);
    engine.load(QUrl::fromLocalFile(entry));
    if (engine.rootObjects().isEmpty())
        return 1;
    const auto window = qobject_cast<QWindow *>(engine.rootObjects().constFirst());
    bool macTitleBarReady = true;
#ifdef Q_OS_MACOS
    if (prepareMacTitleBar && window && window->metaObject()->indexOfProperty("macTitleBarHeight") >= 0) {
        macTitleBarReady = installMacTitleBar(*window);
        if (!macTitleBarReady)
            qWarning("Aurora could not install the native macOS title bar.");
    }
#endif
    if (checkUi) {
        return window && !window->isVisible() && macTitleBarReady ? 0 : 1;
    }
    if (checkNative) {
        if (!window || window->isVisible() || !window->property("managedClose").toBool() || !macTitleBarReady)
            return 1;
        bridge.sendPreference(QStringLiteral("read-general-settings"), 1, {});
        const auto settings = bridge.state().value(QStringLiteral("generalSettings")).toMap();
        if (settings.value(QStringLiteral("acknowledgedRequestId")).toInt() != 1)
            return 1;
        // Exercise QML's normal close request through the in-process bridge.
        QTimer::singleShot(0, &bridge, [&bridge] { bridge.send(QStringLiteral("window-close")); });
        QTimer::singleShot(2000, &app, [&app] { app.exit(1); });
        return app.exec();
    }
    if (native) {
        if (!window)
            return 1;
        app.setQuitOnLastWindowClosed(false);
        nativeDesktop->start();
    }
#ifdef Q_OS_WIN
    if (windowsSession) {
        if (!window)
            return 1;
        connectSessionPopup(app, *window, bridge, ownerHandle);
    }
#endif
    return app.exec();
}

#include "main.moc"
