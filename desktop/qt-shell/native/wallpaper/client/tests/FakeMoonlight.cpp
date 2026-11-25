// SPDX-License-Identifier: GPL-3.0-only
#include <QCoreApplication>
#include <QFile>
#include <QThread>

#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QFile launched(qEnvironmentVariable("WALLPAPER_TEST_LAUNCHED"));
    if (!launched.open(QIODevice::WriteOnly) || launched.write("started") != 7)
        return 10;
    launched.close();

    const auto args = app.arguments();
    if (args.size() != 6 || args.at(1) != "--aurora-runtime" || args.at(2) != "wallpaper"
        || args.at(3) != qEnvironmentVariable("WALLPAPER_TEST_ENDPOINT")
        || args.at(4) != qEnvironmentVariable("WALLPAPER_TEST_HOST")
        || args.at(5) != qEnvironmentVariable("WALLPAPER_TEST_REVISION")
        || qgetenv("AURORA_GAMESTREAM_RELAY_BASE") != qgetenv("WALLPAPER_TEST_RELAY_BASE")
        || qgetenv("AURORA_GAMESTREAM_HOST_UUID") != qgetenv("WALLPAPER_TEST_RELAY_HOST")) {
        return 11;
    }

    const auto mode = qgetenv("WALLPAPER_TEST_MODE");
    if (mode == "overflow") {
        const QByteArray block(8192, 'A');
        for (int i = 0; i < 128; ++i) {
            fwrite(block.constData(), 1, block.size(), stdout);
            fflush(stdout);
        }
        QThread::msleep(10000);
        return 0;
    }

    if (mode == "hold") {
        const auto gate = qEnvironmentVariable("WALLPAPER_TEST_GATE");
        for (int i = 0; i < 1000 && !QFile::exists(gate); ++i)
            QThread::msleep(10);
    }

    QFile payload(qEnvironmentVariable("WALLPAPER_TEST_PAYLOAD"));
    if (!payload.open(QIODevice::ReadOnly))
        return 12;
    const auto bytes = payload.readAll();
    if (fwrite(bytes.constData(), 1, bytes.size(), stdout) != size_t(bytes.size()) || fflush(stdout) != 0)
        return 13;
    return mode == "fail" ? 4 : 0;
}
