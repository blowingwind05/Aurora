// SPDX-License-Identifier: GPL-3.0-only
#include "desktop/NativeDesktop.h"
#include "platform/NativeInstance.h"
#include "ui/AuroraMore.h"
#include "ui/DesktopBridge.h"
#include "ui/StreamSessionWindow.h"

#include <QApplication>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QNetworkAccessManager>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTimer>
#include <QWindow>

#include <memory>

#ifdef Q_OS_MACOS
bool installMacTitleBar(QWindow &window);
#endif

namespace {

bool verifyNativeUi(QWindow &window, StreamSessionWindow &sessionWindow, DesktopBridge &bridge)
{
    if (window.isVisible() || !window.property("managedClose").toBool())
        return false;
#ifdef Q_OS_WIN
    Aurora::Session::SessionSupervisor session(QString{});
    Aurora::Session::SessionView view;
    const auto attempt = QUuid::createUuid();
    const auto error = sessionWindow.prepare(attempt, QStringLiteral("Aurora"), session, view);
    const bool ready = error.isEmpty() && view.owner && !view.owner->isVisible() && view.viewport.y() > 0;
    sessionWindow.close(attempt);
    if (!ready)
        return false;
#else
    Q_UNUSED(sessionWindow);
#endif
    bridge.sendPreference(QStringLiteral("read-general-settings"), 1, {});
    const auto settings = bridge.state().value(QStringLiteral("generalSettings")).toMap();
    return settings.value(QStringLiteral("acknowledgedRequestId")).toInt() == 1;
}

} // namespace

int main(int argc, char *argv[])
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Aurora"));
    const auto arguments = app.arguments();
    const auto option = arguments.value(1);
    const bool checkNative = option == QLatin1String("--check-native");
    if (arguments.size() > 3 || (arguments.size() > 1 && !checkNative && option != QLatin1String("--native"))) {
        qWarning("Usage: Aurora [--native [<Main.qml>] | --check-native [<Main.qml>]]");
        return 2;
    }
#ifdef Q_OS_WIN
    QIcon icon(QStringLiteral(":/aurora/brand/Aurora.ico"));
    if (icon.availableSizes().isEmpty())
        icon = QIcon(QStringLiteral(":/aurora/brand/Aurora-Windows-32.png"));
    app.setWindowIcon(icon);
#endif

    const auto directory = checkNative ? QString() : Aurora::Preferences::defaultDirectory();
    const auto entry = arguments.value(2, NativeDesktop::bundledEntryPoint());
    const auto sessionSource = QUrl::fromLocalFile(QFileInfo(entry).dir().absoluteFilePath(
        QStringLiteral("stream/NativeSessionWindow.qml")));
    QNetworkAccessManager network;
    AuroraMore more(network, &QDesktopServices::openUrl);
    QQmlApplicationEngine engine;
    StreamSessionWindow sessionWindow(engine, sessionSource);
    NativeDesktop desktop(QStringLiteral(AURORA_DESKTOP_VERSION), directory, sessionWindow);
    std::unique_ptr<NativeInstance> instance;
    if (!checkNative) {
        instance = std::make_unique<NativeInstance>(directory);
        const auto result = instance->start();
        if (result == NativeInstance::Result::Activated)
            return 0;
        if (result == NativeInstance::Result::Error) {
            qWarning("Aurora could not start or activate the desktop instance.");
            return 1;
        }
        QObject::connect(instance.get(), &NativeInstance::activationRequested, &desktop, [&desktop] { desktop.activate(); });
    }

    DesktopBridge bridge(desktop);
    more.setApplicationVersion(desktop.state().value(QStringLiteral("applicationVersion")).toString());
    engine.rootContext()->setContextProperty("auroraBridge", &bridge);
    engine.rootContext()->setContextProperty("auroraMore", &more);
    engine.load(QUrl::fromLocalFile(entry));
    if (engine.rootObjects().isEmpty())
        return 1;
    const auto window = qobject_cast<QWindow *>(engine.rootObjects().constFirst());
    if (!window)
        return 1;
#ifdef Q_OS_MACOS
    const bool cocoaWindow = QGuiApplication::platformName() == QLatin1String("cocoa") &&
        window->metaObject()->indexOfProperty("macTitleBarHeight") >= 0;
    if (cocoaWindow && !installMacTitleBar(*window)) {
        qWarning("Aurora could not install the native macOS title bar.");
        if (checkNative)
            return 1;
    }
#endif

    if (checkNative) {
        if (!verifyNativeUi(*window, sessionWindow, bridge))
            return 1;
        QTimer::singleShot(0, &bridge, [&bridge] { bridge.send(QStringLiteral("window-close")); });
        QTimer::singleShot(2000, &app, [&app] { app.exit(1); });
    } else {
        app.setQuitOnLastWindowClosed(false);
        desktop.start();
    }
    return app.exec();
}
