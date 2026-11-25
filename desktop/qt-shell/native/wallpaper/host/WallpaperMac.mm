// SPDX-License-Identifier: GPL-3.0-only
#include "HostWallpaper.h"

#import <AppKit/AppKit.h>
#import <ImageIO/ImageIO.h>

#include <type_traits>

namespace Aurora::Wallpaper {
namespace {

template<typename Ref>
using ScopedCF = std::unique_ptr<std::remove_pointer_t<Ref>, decltype(&CFRelease)>;

qint64 imageDimension(CFDictionaryRef properties, CFStringRef key)
{
    const auto value = static_cast<CFNumberRef>(CFDictionaryGetValue(properties, key));
    qint64 dimension = 0;
    if (!value || CFGetTypeID(value) != CFNumberGetTypeID() || !CFNumberGetValue(value, kCFNumberSInt64Type, &dimension))
        return 0;
    return dimension;
}

} // namespace

QString currentDesktopWallpaperFile()
{
    if (![NSThread isMainThread])
        return {};
    @autoreleasepool {
        NSScreen *screen = NSScreen.screens.firstObject;
        if (!screen)
            return {};
        NSURL *url = [NSWorkspace.sharedWorkspace desktopImageURLForScreen:screen];
        id local = nil;
        if (!url.isFileURL || ![url getResourceValue:&local forKey:NSURLVolumeIsLocalKey error:nullptr] || ![local boolValue])
            return {};
        return QString::fromUtf8(url.path.UTF8String);
    }
}

HostWallpaperResult encodePlatformPreview(const QByteArray &input, QByteArray &jpeg)
{
    using Result = HostWallpaperResult;
    using Cache = HostWallpaperCache;

    // ImageIO also supports still HEIC, which the legacy fallback read through sips.
    @autoreleasepool {
        const ScopedCF<CFDataRef> bytes(
            CFDataCreateWithBytesNoCopy(nullptr, reinterpret_cast<const UInt8 *>(input.constData()),
                                        input.size(), kCFAllocatorNull), CFRelease);
        if (!bytes)
            return Result::InvalidImage;
        const ScopedCF<CGImageSourceRef> source(CGImageSourceCreateWithData(bytes.get(), nullptr), CFRelease);
        if (!source || CGImageSourceGetCount(source.get()) == 0)
            return Result::InvalidImage;

        const size_t index = CGImageSourceGetPrimaryImageIndex(source.get());
        const ScopedCF<CFDictionaryRef> properties(CGImageSourceCopyPropertiesAtIndex(source.get(), index, nullptr), CFRelease);
        if (!properties)
            return Result::InvalidImage;
        const auto width = imageDimension(properties.get(), kCGImagePropertyPixelWidth);
        const auto height = imageDimension(properties.get(), kCGImagePropertyPixelHeight);
        if (width <= 0 || height <= 0)
            return Result::InvalidImage;
        if (width > Cache::maxSourcePixels / height)
            return Result::InputTooLarge;

        NSDictionary *options = @{
            (__bridge NSString *)kCGImageSourceCreateThumbnailFromImageAlways: @YES,
            (__bridge NSString *)kCGImageSourceThumbnailMaxPixelSize: @(Cache::maxPreviewEdge)
        };
        const std::unique_ptr<std::remove_pointer_t<CGImageRef>, decltype(&CGImageRelease)> image(
            CGImageSourceCreateThumbnailAtIndex(source.get(), index, (__bridge CFDictionaryRef)options), CGImageRelease);
        if (!image || CGImageGetWidth(image.get()) == 0 || CGImageGetHeight(image.get()) == 0
            || CGImageGetWidth(image.get()) > Cache::maxPreviewEdge || CGImageGetHeight(image.get()) > Cache::maxPreviewEdge
            || CGImageGetWidth(image.get()) > size_t(width) || CGImageGetHeight(image.get()) > size_t(height)) {
            return Result::InvalidImage;
        }

        const ScopedCF<CFMutableDataRef> output(CFDataCreateMutable(nullptr, 0), CFRelease);
        if (!output)
            return Result::InvalidImage;
        const ScopedCF<CGImageDestinationRef> destination(
            CGImageDestinationCreateWithData(output.get(), CFSTR("public.jpeg"), 1, nullptr), CFRelease);
        if (!destination)
            return Result::InvalidImage;
        NSDictionary *settings = @{
            (__bridge NSString *)kCGImageDestinationLossyCompressionQuality: @(Cache::jpegQuality / 100.0)
        };
        CGImageDestinationAddImage(destination.get(), image.get(), (__bridge CFDictionaryRef)settings);
        if (!CGImageDestinationFinalize(destination.get()))
            return Result::InvalidImage;

        const auto length = CFDataGetLength(output.get());
        if (length > Cache::maxPreviewBytes)
            return Result::OutputTooLarge;
        if (length <= 0)
            return Result::InvalidImage;
        jpeg = QByteArray(reinterpret_cast<const char *>(CFDataGetBytePtr(output.get())), int(length));
        return Result::Updated;
    }
}

} // namespace Aurora::Wallpaper
