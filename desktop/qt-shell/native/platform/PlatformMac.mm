// SPDX-License-Identifier: GPL-3.0-only
#include "PlatformBackends.h"

#include <QDebug>

#import <Foundation/Foundation.h>
#import <ServiceManagement/SMAppService.h>

#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/pwr_mgt/IOPMLib.h>

namespace Aurora::Platform {
namespace {

bool hasAuroraLauncher()
{
    NSBundle *bundle = NSBundle.mainBundle;
    if (![bundle.bundleIdentifier isEqualToString:@"org.aurora-remote.desktop"]
        || ![bundle.bundleURL.pathExtension.lowercaseString isEqualToString:@"app"]
        || ![[bundle objectForInfoDictionaryKey:@"CFBundlePackageType"] isEqual:@"APPL"]) {
        return false;
    }

    // The bundle's launcher may exec the Qt desktop executable in Contents/MacOS.
    // Service Management registers the bundle's declared launcher in either layout.
    NSString *name = [bundle objectForInfoDictionaryKey:@"CFBundleExecutable"];
    if (![name isKindOfClass:NSString.class] || name.length == 0 || ![name.lastPathComponent isEqualToString:name])
        return false;
    NSString *macOS = [bundle.bundlePath stringByAppendingPathComponent:@"Contents/MacOS"];
    NSString *launcher = [macOS stringByAppendingPathComponent:name];
    NSFileManager *files = NSFileManager.defaultManager;
    BOOL directory = NO;
    return [files fileExistsAtPath:launcher isDirectory:&directory] && !directory
        && [files isExecutableFileAtPath:launcher];
}

class MacStartupBackend final : public StartupBackend
{
public:
    bool available() const override
    {
        if (@available(macOS 13.0, *)) {
            @autoreleasepool {
                return hasAuroraLauncher();
            }
        }
        return false;
    }

    StartupState read() const override
    {
        if (@available(macOS 13.0, *)) {
            @autoreleasepool {
                SMAppService *service = SMAppService.mainAppService;
                if (!service)
                    return StartupState::ReadFailed;
                switch (service.status) {
                case SMAppServiceStatusNotRegistered: return StartupState::Off;
                case SMAppServiceStatusEnabled: return StartupState::On;
                case SMAppServiceStatusRequiresApproval: return StartupState::NeedsApproval;
                case SMAppServiceStatusNotFound: return StartupState::Unavailable;
                }
            }
        }
        return StartupState::Unavailable;
    }

    StartupResult setEnabled(bool enabled) override
    {
        if (@available(macOS 13.0, *)) {
            @autoreleasepool {
                SMAppService *service = SMAppService.mainAppService;
                if (!service)
                    return StartupResult::WriteFailed;

                const BOOL success = enabled ? [service registerAndReturnError:nullptr]
                                             : [service unregisterAndReturnError:nullptr];
                if (success)
                    return StartupResult::Ok;

                const auto status = service.status;
                const bool reached = enabled
                    ? status == SMAppServiceStatusEnabled || status == SMAppServiceStatusRequiresApproval
                    : status == SMAppServiceStatusNotRegistered;
                return reached ? StartupResult::Ok : StartupResult::WriteFailed;
            }
        }
        return StartupResult::Unavailable;
    }
};

class MacHostSleepBackend final : public HostSleepBackend
{
public:
    bool onAcPower() const override
    {
        const CFTypeRef info = IOPSCopyPowerSourcesInfo();
        if (!info)
            return false;
        const CFStringRef source = IOPSGetProvidingPowerSourceType(info);
        const bool ac = source && CFEqual(source, CFSTR(kIOPMACPowerKey));
        CFRelease(info);
        return ac;
    }

    bool acquireAssertion() override
    {
        IOPMAssertionID assertion = kIOPMNullAssertionID;
        const IOReturn result = IOPMAssertionCreateWithName(kIOPMAssertPreventUserIdleSystemSleep,
            kIOPMAssertionLevelOn, CFSTR("Aurora 正在允许本机被控"), &assertion);
        if (result != kIOReturnSuccess || assertion == kIOPMNullAssertionID) {
            qDebug() << "Aurora could not request macOS idle sleep prevention:" << result;
            return false;
        }

        assertion_ = assertion;
        return true;
    }

    void releaseAssertion() override
    {
        IOPMAssertionRelease(assertion_);
        assertion_ = kIOPMNullAssertionID;
    }

private:
    IOPMAssertionID assertion_ = kIOPMNullAssertionID;
};

} // namespace

std::unique_ptr<StartupBackend> makeStartupBackend(const QString &)
{
    return std::make_unique<MacStartupBackend>();
}

std::unique_ptr<HostSleepBackend> makeHostSleepBackend()
{
    return std::make_unique<MacHostSleepBackend>();
}

} // namespace Aurora::Platform
