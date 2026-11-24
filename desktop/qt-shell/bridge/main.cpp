// SPDX-License-Identifier: GPL-3.0-only
#include <QByteArray>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include <cstdio>

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
    explicit AuroraBridge(QObject *parent = nullptr) : QObject(parent)
    {
        // Anonymous pipes on Windows cannot be watched by QSocketNotifier.
        connect(&pollTimer, &QTimer::timeout, this, &AuroraBridge::readInput);
        pollTimer.start(25);
    }

    QVariantMap state() const { return snapshot; }

    Q_INVOKABLE void send(const QString &action, const QString &deviceId = {}, int deviceEpoch = 0)
    {
        const QJsonObject request{{"action", action}, {"deviceId", deviceId}, {"deviceEpoch", deviceEpoch}};
        const auto line = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
        const auto size = static_cast<size_t>(line.size());
        if (std::fwrite(line.constData(), 1, size, stdout) != size || std::fflush(stdout) != 0)
            QCoreApplication::quit();
    }

signals:
    void stateChanged();

private:
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
            if (document.isObject() && document.object().contains("deviceAccess")) {
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

    QTimer pollTimer;
    QByteArray pending;
    QVariantMap snapshot;
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    const auto arguments = app.arguments();
    if (arguments.size() != 2) {
        qWarning("Usage: AuroraQtShell <path to Live.qml> (stdin/stdout are redirected by Aurora)");
        return 2;
    }

    AuroraBridge bridge;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("auroraBridge", &bridge);
    engine.load(QUrl::fromLocalFile(arguments.at(1)));
    if (engine.rootObjects().isEmpty())
        return 1;
    return app.exec();
}

#include "main.moc"