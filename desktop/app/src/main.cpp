// SPDX-License-Identifier: GPL-3.0-only
#include "desktop/NativeDesktop.h"
#include "platform/NativeInstance.h"
#include "ui/AuroraMore.h"

#include <QApplication>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QTimer>
#include <QWindow>

#include <memory>

#ifdef Q_OS_MACOS
bool installMacTitleBar(QWindow &window);
#endif

namespace {

class DesktopBridge final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap state READ state NOTIFY stateChanged)

public:
    explicit DesktopBridge(NativeDesktop &desktop) : desktop_(desktop)
    {
        connect(&desktop_, &NativeDesktop::stateChanged, this, &DesktopBridge::stateChanged);
    }

    QVariantMap state() const { return desktop_.state(); }

    Q_INVOKABLE void send(const QString &action, const QString &deviceId = {}, int deviceEpoch = 0)
    {
        desktop_.handleRequest({{"action", action}, {"deviceId", deviceId}, {"deviceEpoch", deviceEpoch}});
    }

    Q_INVOKABLE void sendPreference(const QString &action, int requestId, const QVariant &value)
    {
        desktop_.handleRequest({{"action", action}, {"preferenceRequestId", requestId},
                                {"preferenceValue", QJsonValue::fromVariant(value)}});
    }

    Q_INVOKABLE void sendTailscale(const QString &action, const QString &target, int revision)
    {
        desktop_.handleRequest({{"action", action}, {"accountTarget", target}, {"accountRevision", revision}});
    }

    Q_INVOKABLE void sendSunshine(const QString &action, const QVariantMap &data)
    {
        desktop_.handleRequest({{"action", action}, {"sunshine", QJsonObject::fromVariantMap(data)}});
    }

signals:
    void stateChanged();

private:
    NativeDesktop &desktop_;
};

bool verifyNativeUi(QQmlEngine &engine, QWindow &window, const QUrl &sessionSource, DesktopBridge &bridge)
{
    if (window.isVisible() || !window.property("managedClose").toBool())
        return false;
#ifdef Q_OS_WIN
    QQmlComponent component(&engine, sessionSource);
    std::unique_ptr<QObject> session(component.isReady() ? component.create() : nullptr);
    const auto sessionWindow = qobject_cast<QWindow *>(session.get());
    if (!sessionWindow || sessionWindow->isVisible() || sessionWindow->property("videoTop").toInt() <= 0) {
        if (component.isError())
            qWarning().noquote() << component.errorString();
        return false;
    }
#else
    Q_UNUSED(engine);
    Q_UNUSED(sessionSource);
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
    QNetworkAccessManager network;
    AuroraMore more(network, &QDesktopServices::openUrl);
    QQmlApplicationEngine engine;
    NativeDesktop desktop(QStringLiteral(AURORA_DESKTOP_VERSION), directory);
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
    const auto entry = arguments.value(2, NativeDesktop::bundledEntryPoint());
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

    const auto sessionSource = QUrl::fromLocalFile(QFileInfo(entry).dir().absoluteFilePath(
        QStringLiteral("stream/NativeSessionWindow.qml")));
    if (checkNative) {
        if (!verifyNativeUi(engine, *window, sessionSource, bridge))
            return 1;
        QTimer::singleShot(0, &bridge, [&bridge] { bridge.send(QStringLiteral("window-close")); });
        QTimer::singleShot(2000, &app, [&app] { app.exit(1); });
    } else {
        app.setQuitOnLastWindowClosed(false);
        desktop.setSessionWindowSource(engine, sessionSource);
        desktop.start();
    }
    return app.exec();
}

#include "main.moc"
