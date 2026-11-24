// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QByteArray>
#include <QMap>
#include <QStringList>

#include <memory>
#include <optional>

namespace Aurora::Sunshine {

enum class HostPlatform { Windows, MacOS };

class HostLayout
{
public:
    static std::optional<HostLayout> installed();
    static std::optional<HostLayout> forDirectories(HostPlatform platform, const QString &applicationDirectory,
        const QString &localDataDirectory);

    HostPlatform platform() const { return platform_; }
    const QString &payloadDirectory() const { return payload_; }
    const QString &executable() const { return executable_; }
    const QString &profileDirectory() const { return profile_; }
    QString configFile() const;
    QString certificateFile() const;
    QString logFile() const;
    bool hasPayload() const;

private:
    HostLayout(HostPlatform platform, QString payload, QString executable, QString profile);

    HostPlatform platform_;
    QString payload_;
    QString executable_;
    QString profile_;
};

class PreparedProfile
{
public:
    ~PreparedProfile();

private:
    struct Lock;
    friend std::unique_ptr<PreparedProfile> prepareBundledProfile(const HostLayout &layout, QString &error);
    explicit PreparedProfile(std::unique_ptr<Lock> lock);

    std::unique_ptr<Lock> lock_;
};

// Holds the bundled profile lock until destroyed. May seed a missing sunshine.conf.
std::unique_ptr<PreparedProfile> prepareBundledProfile(const HostLayout &layout, QString &error);

struct HostLaunch
{
    HostPlatform platform;
    QString executable;
    QString workingDirectory;
    QStringList arguments;
    QMap<QString, QString> environment;
};

// Adapter contract. start returns once child stdin and platform containment are ready.
// It can fail after spawning, in which case the caller will stop and reap the child.
class HostProcess
{
public:
    virtual ~HostProcess() = default;

    virtual bool start(const HostLaunch &launch, QString &error) = 0;
    virtual bool writePasswordFrame(const QByteArray &frame, QString &error) = 0;
    virtual void closeInput() = 0;
    virtual bool signalStopEvent() = 0;
    virtual bool running() = 0;
    virtual void waitForExit(int timeoutMs) = 0;
    virtual void forceStop() = 0;
    virtual std::optional<int> exitCode() const = 0;
};

// Own on a single worker thread. start/stop are synchronous and may wait for subprocess I/O.
class BundledHost
{
public:
    static constexpr int restartRequestedExitCode = 42;

    explicit BundledHost(HostLayout layout);
    BundledHost(HostLayout layout, std::unique_ptr<HostProcess> process);
    ~BundledHost();

    const HostLayout &layout() const { return layout_; }
    bool start(QString &error);
    bool stop(QString &error);
    bool running();
    std::optional<int> exitCode();
    QByteArray launchPassword();

private:
    bool observeRunning();
    bool forceAndReap();
    void clearPassword();

    HostLayout layout_;
    // The adapter is destroyed before the profile lock, even if a forced wait times out.
    std::unique_ptr<PreparedProfile> profile_;
    std::unique_ptr<HostProcess> process_;
    QByteArray password_;
};

} // namespace Aurora::Sunshine
