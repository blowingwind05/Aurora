// SPDX-License-Identifier: GPL-3.0-only
#include "NativeInstance.h"
#include "TrayMenuPlacement.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc == 2) {
        NativeInstance child(app.arguments().at(1));
        return child.start() == NativeInstance::Result::Activated ? 0 : 1;
    }

    const QSize menu(210, 115);
    if (trayMenuPosition({0, 0, 1920, 1032}, {1840, 1042, 24, 24}, menu) != QPoint(1654, 917) ||
        trayMenuPosition({0, 48, 1920, 1032}, {1840, 12, 24, 24}, menu) != QPoint(1654, 48) ||
        trayMenuPosition({-1920, 0, 1920, 1032}, {-34, 1042, 24, 24}, menu) != QPoint(-220, 917))
        return 6;

    QTemporaryDir profile;
    if (!profile.isValid())
        return 2;

    {
        NativeInstance primary(profile.path());
        if (primary.start() != NativeInstance::Result::Primary)
            return 3;

        int activations = 0;
        QObject::connect(&primary, &NativeInstance::activationRequested, &app, [&activations] { ++activations; });
        for (int expected = 1; expected <= 2; ++expected) {
            QEventLoop loop;
            QProcess child;
            QObject::connect(&child, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                             &loop, &QEventLoop::quit);
            QObject::connect(&child, &QProcess::errorOccurred, &loop, &QEventLoop::quit);
            QTimer::singleShot(5000, &loop, &QEventLoop::quit);
            child.start(QCoreApplication::applicationFilePath(), {profile.path()});
            loop.exec();
            if (child.state() != QProcess::NotRunning || child.exitStatus() != QProcess::NormalExit ||
                child.error() != QProcess::UnknownError || child.exitCode() != 0 || activations != expected)
                return 4;
        }
    }

    NativeInstance replacement(profile.path());
    if (replacement.start() != NativeInstance::Result::Primary)
        return 5;
    qInfo("PASS: repeated launches activate one primary process, and exit releases the instance lock.");
    return 0;
}
