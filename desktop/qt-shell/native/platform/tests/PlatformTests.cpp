// SPDX-License-Identifier: GPL-3.0-only
#include "PlatformBackends.h"

#include <QCoreApplication>
#include <QEventLoop>

#include <utility>

using namespace Aurora::Platform;

#define CHECK(condition) do { \
    if (!(condition)) qFatal("%s:%d: %s", __FILE__, __LINE__, #condition); \
} while (false)

namespace {

class FakeStartup final : public StartupBackend
{
public:
    bool available() const override { return supported; }
    StartupState read() const override { return state; }

    StartupResult setEnabled(bool enabled) override
    {
        ++writes;
        if (writeResult == StartupResult::Ok)
            state = enabled ? StartupState::On : StartupState::Off;
        return writeResult;
    }

    StartupState state = StartupState::Off;
    StartupResult writeResult = StartupResult::Ok;
    bool supported = true;
    int writes = 0;
};

void startupDefault()
{
    auto fake = std::make_unique<FakeStartup>();
    auto &os = *fake;
    int claims = 0;
    bool initialized = false;
    StartupRegistration startup([&] {
        ++claims;
        return std::exchange(initialized, true)
            ? StartupInitialization::AlreadyClaimed : StartupInitialization::Claimed;
    }, std::move(fake));

    os.supported = false;
    CHECK(startup.read() == StartupState::Unavailable);
    CHECK(startup.initializeDefault() == StartupResult::Unavailable);
    CHECK(startup.change(true) == StartupResult::Unavailable);
    CHECK(claims == 0 && os.writes == 0);

    os.supported = true;
    CHECK(startup.initializeDefault() == StartupResult::Ok);
    CHECK(startup.read() == StartupState::On && os.writes == 1);
    CHECK(startup.change(false) == StartupResult::Ok);
    CHECK(startup.initializeDefault() == StartupResult::Ok);
    CHECK(startup.read() == StartupState::Off && os.writes == 2);

    // A clean startup marker with an already-configured login entry preserves it.
    for (const auto state : {StartupState::On, StartupState::OtherLocation, StartupState::NeedsApproval,
                             StartupState::Unavailable}) {
        initialized = false;
        os.state = state;
        CHECK(startup.initializeDefault() == StartupResult::Ok);
        CHECK(initialized && os.writes == 2 && startup.read() == state);
    }

    initialized = false;
    CHECK(startup.change(false) == StartupResult::Ok);
    CHECK(initialized);
    CHECK(startup.initializeDefault() == StartupResult::Ok);
    CHECK(startup.read() == StartupState::Off && os.writes == 3);
}

void startupFailures()
{
    auto fake = std::make_unique<FakeStartup>();
    auto &os = *fake;
    auto markerResult = StartupInitialization::Failed;
    StartupRegistration startup([&] { return markerResult; }, std::move(fake));
    CHECK(startup.initializeDefault() == StartupResult::MarkerFailed);
    CHECK(startup.change(true) == StartupResult::MarkerFailed);
    CHECK(os.writes == 0 && startup.initializationFailed());

    markerResult = StartupInitialization::Claimed;
    os.state = StartupState::ReadFailed;
    CHECK(startup.initializeDefault() == StartupResult::ReadFailed);
    CHECK(os.writes == 0);

    os.state = StartupState::Off;
    os.writeResult = StartupResult::WriteFailed;
    CHECK(startup.initializeDefault() == StartupResult::WriteFailed);
    markerResult = StartupInitialization::AlreadyClaimed;
    CHECK(startup.initializeDefault() == StartupResult::Ok);
    CHECK(os.writes == 1 && startup.initializationFailed());

    os.writeResult = StartupResult::Ok;
    CHECK(startup.change(true) == StartupResult::Ok);
    CHECK(startup.read() == StartupState::On && !startup.initializationFailed());
}

#ifdef Q_OS_WIN
void windowsLaunchers()
{
    CHECK(windowsStartupCommand(QStringLiteral("C:/Aurora 测试/Aurora.exe"))
          == QStringLiteral("\"C:\\Aurora 测试\\Aurora.exe\""));
    CHECK(windowsStartupCommand(QStringLiteral("//server/share/Aurora.exe"))
          == QStringLiteral("\"\\\\server\\share\\Aurora.exe\""));
    CHECK(windowsStartupCommand(QStringLiteral("\\\\?\\C:\\Aurora\\Aurora.exe"))
          == QStringLiteral("\"\\\\?\\C:\\Aurora\\Aurora.exe\""));
    CHECK(windowsStartupCommand(QStringLiteral("\\\\?\\UNC\\server\\share\\Aurora.exe"))
          == QStringLiteral("\"\\\\?\\UNC\\server\\share\\Aurora.exe\""));
    for (const auto &invalid : {QStringLiteral("Aurora.exe"), QStringLiteral("C:Aurora.exe"),
                               QStringLiteral("\\Aurora.exe"), QStringLiteral("\\\\server\\Aurora.exe"),
                               QStringLiteral("\\\\.\\C:\\Aurora.exe"), QStringLiteral("C:\\DOTNET.EXE"),
                               QStringLiteral("C:\\bad\"path\\Aurora.exe"), QStringLiteral("C:\\Aurora.ps1")}) {
        CHECK(windowsStartupCommand(invalid).isEmpty());
    }
    const auto longest = QStringLiteral("C:\\") + QString(251, QLatin1Char('a')) + QStringLiteral(".exe");
    CHECK(windowsStartupCommand(longest).size() == 260);
    CHECK(windowsStartupCommand(longest + QStringLiteral(".exe")).isEmpty());
}
#endif

struct PowerState
{
    bool ac = false;
    bool resumePending = false;
    bool canAcquire = true;
    bool held = false;
    bool monitoring = false;
    int acquired = 0;
    int released = 0;
};

class FakePower final : public HostSleepBackend
{
public:
    explicit FakePower(PowerState &state) : state_(state) {}

    bool onAcPower() const override { return state_.ac; }

    bool acquireAssertion() override
    {
        CHECK(!state_.held);
        if (!state_.canAcquire)
            return false;
        ++state_.acquired;
        state_.held = true;
        return true;
    }

    void releaseAssertion() override
    {
        CHECK(state_.held);
        ++state_.released;
        state_.held = false;
    }

    bool startResumeMonitoring() override
    {
        CHECK(!state_.monitoring);
        state_.monitoring = true;
        return true;
    }

    void stopResumeMonitoring() override
    {
        CHECK(state_.monitoring);
        state_.monitoring = false;
    }

    bool takeResume() override { return std::exchange(state_.resumePending, false); }

private:
    PowerState &state_;
};

template <typename Condition>
void waitFor(Condition ready)
{
    QEventLoop loop;
    QTimer probe;
    QObject::connect(&probe, &QTimer::timeout, &loop, [&] {
        if (ready())
            loop.quit();
    });
    QTimer::singleShot(6000, &loop, &QEventLoop::quit);
    probe.start(10);
    if (!ready())
        loop.exec();
    CHECK(ready());
}

void sleepPolicy()
{
    for (auto mode : {HostSleepMode::System, HostSleepMode::OnAc, HostSleepMode::Always})
        CHECK(parseHostSleepMode(hostSleepModeToken(mode)) == mode);
    CHECK(!parseHostSleepMode(QStringLiteral("always")));
    CHECK(!parseHostSleepMode(QStringLiteral(" OnAc ")));
    CHECK(!parseHostSleepMode(QString()));

    PowerState power;
    {
        HostSleepPrevention sleep(HostSleepMode::System, std::make_unique<FakePower>(power));
        sleep.setHostActive(true);
        CHECK(!power.held && !power.monitoring);
        sleep.applyMode(HostSleepMode::OnAc);
        CHECK(power.monitoring && !power.held);
        power.ac = true;
        waitFor([&] { return power.held; });
        CHECK(power.acquired == 1);
        sleep.setHostActive(true);
        CHECK(power.acquired == 1);
        power.ac = false;
        waitFor([&] { return !power.held; });
        CHECK(power.released == 1);

        power.canAcquire = false;
        sleep.applyMode(HostSleepMode::Always);
        CHECK(!power.held);
        power.canAcquire = true;
        sleep.setHostActive(true);
        CHECK(power.held && power.acquired == 2);

        power.resumePending = true;
        waitFor([&] { return power.acquired == 3; });
        CHECK(power.held && power.released == 2);

        sleep.applyMode(HostSleepMode::System);
        CHECK(!power.held && !power.monitoring);
        sleep.applyMode(HostSleepMode::Always);
        sleep.setHostActive(false);
        CHECK(!power.held && !power.monitoring);
        CHECK(sleep.mode() == HostSleepMode::Always);
        sleep.setHostActive(true);
        CHECK(power.held && power.monitoring);
    }
    CHECK(!power.held && !power.monitoring && power.acquired == power.released);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    startupDefault();
    startupFailures();
#ifdef Q_OS_WIN
    windowsLaunchers();
#endif
    sleepPolicy();
}
