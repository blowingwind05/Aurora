// SPDX-License-Identifier: GPL-3.0-only
#include "BundledHost.h"
#include "SunshineConfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QOperatingSystemVersion>
#include <QStandardPaths>
#include <QTemporaryFile>

#include <filesystem>
#include <utility>

#if defined(Q_OS_WIN)
#include <windows.h>
#elif defined(Q_OS_UNIX)
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Aurora::Sunshine {
namespace {

bool localAbsolute(const QString &path)
{
    const auto normalized = QDir::fromNativeSeparators(path);
    return QDir::isAbsolutePath(normalized) && !normalized.startsWith(QStringLiteral("//")) &&
        !normalized.startsWith(QLatin1Char(':'));
}

bool ensureProfileDirectory(const QString &profile)
{
    const auto root = QFileInfo(profile).absolutePath();
    if (!QDir().mkpath(root))
        return false;
#if defined(Q_OS_UNIX)
    if (::mkdir(QFile::encodeName(profile).constData(), S_IRWXU) != 0 && errno != EEXIST)
        return false;
#else
    if (!QDir().mkpath(profile))
        return false;
#endif
    const QFileInfo info(profile);
    return info.isDir() && !info.isSymLink() && localAbsolute(info.canonicalFilePath());
}

std::filesystem::path nativePath(const QString &path)
{
#if defined(Q_OS_WIN)
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(QFile::encodeName(path).constData());
#endif
}

bool separateAccountAndState(const QString &profile)
{
    const QDir dir(profile);
    const auto &fixed = managedOptions();
    const auto accountName = fixed.value(QStringLiteral("credentials_file"));
    const auto stateName = fixed.value(QStringLiteral("file_state"));
    if (accountName.isEmpty() || stateName.isEmpty() || accountName.compare(stateName, Qt::CaseInsensitive) == 0)
        return false;
    const QFileInfo account(dir.filePath(accountName));
    const QFileInfo state(dir.filePath(stateName));
    if (account.isSymLink() || state.isSymLink() ||
        (account.exists() && !account.isFile()) || (state.exists() && !state.isFile()))
        return false;
    if (!account.exists() || !state.exists())
        return true;

    // Check inode/file identity to catch hard links without reading file contents.
    std::error_code error;
    const bool shared = std::filesystem::equivalent(nativePath(account.filePath()), nativePath(state.filePath()), error);
    return !error && !shared;
}

bool initializeConfig(const QString &path)
{
    const QFileInfo existing(path);
    if (existing.exists() || existing.isSymLink())
        return existing.isFile() && !existing.isSymLink();

    QByteArray config;
    const auto &fixed = managedOptions();
    for (auto it = fixed.cbegin(); it != fixed.cend(); ++it)
        config += (it.key() + QStringLiteral(" = ") + it.value()).toUtf8() + '\n';

    QTemporaryFile stage(path + QStringLiteral(".XXXXXX"));
    if (!stage.open() || stage.write(config) != config.size() || !stage.flush())
        return false;
    // Rename within one directory. QTemporaryFile refuses to replace an existing destination.
    if (!stage.rename(path))
        return false;
    stage.setAutoRemove(false);
    return true;
}

} // namespace

struct PreparedProfile::Lock
{
#if defined(Q_OS_WIN)
    HANDLE handle = INVALID_HANDLE_VALUE;

    bool open(const QString &path)
    {
        const auto native = QDir::toNativeSeparators(path).toStdWString();
        handle = CreateFileW(native.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return false;
        BY_HANDLE_FILE_INFORMATION info{};
        return GetFileInformationByHandle(handle, &info) &&
            !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
    }

    ~Lock()
    {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
#elif defined(Q_OS_UNIX)
    int descriptor = -1;

    bool open(const QString &path)
    {
        descriptor = ::open(QFile::encodeName(path).constData(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                            S_IRUSR | S_IWUSR);
        struct stat info {};
        return descriptor != -1 && ::fstat(descriptor, &info) == 0 && S_ISREG(info.st_mode) &&
            ::flock(descriptor, LOCK_EX | LOCK_NB) == 0;
    }

    ~Lock()
    {
        if (descriptor != -1)
            ::close(descriptor);
    }
#else
    bool open(const QString &) { return false; }
#endif
};

HostLayout::HostLayout(HostPlatform platform, QString payload, QString executable, QString profile)
    : platform_(platform), payload_(std::move(payload)), executable_(std::move(executable)), profile_(std::move(profile))
{
}

std::optional<HostLayout> HostLayout::installed()
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    if (!QCoreApplication::instance())
        return std::nullopt;
#if defined(Q_OS_WIN)
    constexpr auto platform = HostPlatform::Windows;
#else
    if (QOperatingSystemVersion::current() < QOperatingSystemVersion(QOperatingSystemVersion::MacOS, 14, 2))
        return std::nullopt;
    constexpr auto platform = HostPlatform::MacOS;
#endif
    return forDirectories(platform, QCoreApplication::applicationDirPath(),
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
#else
    return std::nullopt;
#endif
}

std::optional<HostLayout> HostLayout::forDirectories(HostPlatform platform, const QString &applicationDirectory,
    const QString &localDataDirectory)
{
    if (!localAbsolute(applicationDirectory) || !localAbsolute(localDataDirectory))
        return std::nullopt;
    const QDir app(applicationDirectory);
    const auto profile = QDir(localDataDirectory).absoluteFilePath(QStringLiteral("Aurora/Sunshine/Bundled/config"));
    if (platform == HostPlatform::Windows) {
        const auto payload = app.absoluteFilePath(QStringLiteral("Components/Sunshine"));
        return HostLayout(platform, payload, QDir(payload).absoluteFilePath(QStringLiteral("AuroraHost.exe")), profile);
    }

    const QDir contents(QDir::cleanPath(app.absoluteFilePath(QStringLiteral("../Helpers/AuroraHost.app/Contents"))));
    return HostLayout(platform, contents.absoluteFilePath(QStringLiteral("Resources")),
                      contents.absoluteFilePath(QStringLiteral("MacOS/AuroraHost")), profile);
}

QString HostLayout::configFile() const
{
    return QDir(profile_).filePath(QStringLiteral("sunshine.conf"));
}

QString HostLayout::certificateFile() const
{
    return QDir(profile_).filePath(managedOptions().value(QStringLiteral("cert")));
}

QString HostLayout::logFile() const
{
    return QDir(profile_).filePath(managedOptions().value(QStringLiteral("log_path")));
}

bool HostLayout::hasPayload() const
{
    const QDir payload(payload_);
    return QFileInfo(executable_).isFile() &&
        (platform_ != HostPlatform::Windows || QFileInfo(payload.filePath(QStringLiteral("zlib1.dll"))).isFile()) &&
        QFileInfo(payload.filePath(QStringLiteral("assets/apps.json"))).isFile() &&
        QFileInfo(payload.filePath(QStringLiteral("assets/web/index.html"))).isFile();
}

PreparedProfile::PreparedProfile(std::unique_ptr<Lock> lock) : lock_(std::move(lock)) {}

PreparedProfile::~PreparedProfile() = default;

std::unique_ptr<PreparedProfile> prepareBundledProfile(const HostLayout &layout, QString &error)
{
    error.clear();
    const auto &profile = layout.profileDirectory();
    if (!ensureProfileDirectory(profile)) {
        error = QStringLiteral("无法创建内置 Sunshine 的本机配置目录。");
        return {};
    }

    auto lock = std::make_unique<PreparedProfile::Lock>();
    const QDir root(QFileInfo(profile).absolutePath());
    if (!lock->open(root.filePath(QStringLiteral("running.lock")))) {
        error = QStringLiteral("无法锁定内置 Sunshine 的配置目录。");
        return {};
    }
    if (!separateAccountAndState(profile)) {
        error = QStringLiteral("内置 Sunshine 的管理账号文件必须与配对状态文件分开。");
        return {};
    }
    if (!initializeConfig(layout.configFile())) {
        error = QStringLiteral("无法准备内置 Sunshine 的配置文件。");
        return {};
    }
    return std::unique_ptr<PreparedProfile>(new PreparedProfile(std::move(lock)));
}

} // namespace Aurora::Sunshine
