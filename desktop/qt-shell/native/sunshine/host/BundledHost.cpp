// SPDX-License-Identifier: GPL-3.0-only
#include "BundledHost.h"
#include "SunshineConfig.h"

#include <QRandomGenerator>
#include <QUuid>

#include <utility>

namespace Aurora::Sunshine {

std::unique_ptr<HostProcess> makeNativeHostProcess(HostPlatform platform);

namespace {

constexpr int gracefulWaitMs = 11000;
constexpr int forcedWaitMs = 3000;

HostLaunch launchFor(const HostLayout &layout)
{
    HostLaunch launch{layout.platform(), layout.executable(), layout.payloadDirectory(), {}, {}};
    launch.environment.insert(QStringLiteral("AURORA_SUNSHINE_PROFILE"), layout.profileDirectory());
    if (layout.platform() == HostPlatform::Windows) {
        auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        id.remove(QLatin1Char('-'));
        launch.environment.insert(QStringLiteral("AURORA_SUNSHINE_STOP_EVENT"),
                                  QStringLiteral("Local\\AuroraSunshineStop-") + id);
    }

    const auto &fixed = managedOptions();
    for (auto it = fixed.cbegin(); it != fixed.cend(); ++it)
        launch.arguments.append(it.key() + QLatin1Char('=') + it.value());
    return launch;
}

QByteArray newPassword()
{
    constexpr char digits[] = "0123456789ABCDEF";
    QByteArray password(64, '\0');
    for (int offset = 0; offset < password.size(); offset += 8) {
        auto word = QRandomGenerator::system()->generate();
        for (int digit = 0; digit < 8; ++digit) {
            password[offset + digit] = digits[word & 0xf];
            word >>= 4;
        }
    }
    return password;
}

} // namespace

BundledHost::BundledHost(HostLayout layout)
    : layout_(std::move(layout)), process_(makeNativeHostProcess(layout_.platform()))
{
}

BundledHost::BundledHost(HostLayout layout, std::unique_ptr<HostProcess> process)
    : layout_(std::move(layout)), process_(std::move(process))
{
}

BundledHost::~BundledHost()
{
    QString unused;
    stop(unused);
}

bool BundledHost::start(QString &error)
{
    error.clear();
    if (!process_) {
        error = QStringLiteral("当前系统不支持启动内置 Sunshine。");
        return false;
    }
    if (observeRunning()) {
        if (!password_.isEmpty())
            return true;
        error = QStringLiteral("上一次内置 Sunshine 进程仍在退出。");
        return false;
    }
    if (!layout_.hasPayload()) {
        error = QStringLiteral("此版本缺少完整的 Sunshine 组件。");
        return false;
    }

    profile_ = prepareBundledProfile(layout_, error);
    if (!profile_)
        return false;

    auto password = newPassword();
    if (!process_->start(launchFor(layout_), error) ||
        !process_->writePasswordFrame(password, error) || !process_->running()) {
        password.fill('\0');
        if (error.isEmpty())
            error = QStringLiteral("内置 Sunshine 在启动握手时退出。");
        if (!forceAndReap())
            error += QStringLiteral(" 内置 Sunshine 进程仍在退出。");
        return false;
    }

    if (layout_.platform() == HostPlatform::Windows)
        process_->closeInput();

    password_ = std::move(password);
    return true;
}

bool BundledHost::stop(QString &error)
{
    error.clear();
    const bool handshaken = !password_.isEmpty();
    clearPassword();
    if (!observeRunning())
        return true;

    if (handshaken) {
        bool requested = true;
        if (layout_.platform() == HostPlatform::Windows)
            requested = process_->signalStopEvent();
        else
            process_->closeInput();

        if (requested) {
            process_->waitForExit(gracefulWaitMs);
            if (!observeRunning())
                return true;
        }
    }
    if (forceAndReap())
        return true;

    error = QStringLiteral("内置 Sunshine 进程仍在退出。");
    return false;
}

bool BundledHost::forceAndReap()
{
    if (!observeRunning())
        return true;
    process_->forceStop();
    process_->waitForExit(forcedWaitMs);
    return !observeRunning();
}

bool BundledHost::observeRunning()
{
    if (process_ && process_->running())
        return true;

    clearPassword();
    profile_.reset();
    return false;
}

bool BundledHost::running()
{
    return observeRunning() && !password_.isEmpty();
}

std::optional<int> BundledHost::exitCode()
{
    if (observeRunning() || !process_)
        return std::nullopt;
    return process_->exitCode();
}

QByteArray BundledHost::launchPassword()
{
    return running() ? password_ : QByteArray{};
}

void BundledHost::clearPassword()
{
    password_.fill('\0');
    password_.clear();
}

} // namespace Aurora::Sunshine
