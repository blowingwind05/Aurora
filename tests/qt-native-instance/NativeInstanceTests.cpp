// SPDX-License-Identifier: GPL-3.0-only
#include "NativeInstance.h"

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
