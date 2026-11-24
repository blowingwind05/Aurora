// SPDX-License-Identifier: GPL-3.0-only
#import <AppKit/AppKit.h>

#include <QVariant>
#include <QWindow>
#include <QtMath>

bool installMacTitleBar(QWindow &window)
{
    @autoreleasepool {
        // On Cocoa QWindow::winId() is the content NSView.
        auto *view = reinterpret_cast<NSView *>(window.winId());
        NSWindow *native = view.window;
        if (!(native.styleMask & NSWindowStyleMaskTitled))
            return false;

        const NSSize contentSize = view.bounds.size;
        native.styleMask |= NSWindowStyleMaskFullSizeContentView;
        native.titleVisibility = NSWindowTitleHidden;

        // A unified toolbar gives the extended band native traffic lights and
        // lets AppKit handle dragging, fullscreen and titlebar double-clicks.
        auto *toolbar = [[NSToolbar alloc] initWithIdentifier:@"AuroraMainWindow"];
        if (@available(macOS 11.0, *)) {
            native.toolbarStyle = NSWindowToolbarStyleUnified;
            native.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;
        }
        native.toolbar = toolbar;
        [toolbar release];
        native.titlebarAppearsTransparent = YES;

        if (!NSEqualSizes(view.bounds.size, contentSize))
            [native setContentSize:contentSize];

        [view.superview layoutSubtreeIfNeeded];
        const NSRect content = [view convertRect:view.bounds toView:nil];
        const int inset = qCeil(NSMaxY(content) - NSMaxY(native.contentLayoutRect));
        if (inset > 0)
            window.setProperty("macTitleBarHeight", inset);
        return (native.styleMask & NSWindowStyleMaskFullSizeContentView) &&
            native.titlebarAppearsTransparent &&
            [native standardWindowButton:NSWindowCloseButton] != nil &&
            [native standardWindowButton:NSWindowMiniaturizeButton] != nil &&
            [native standardWindowButton:NSWindowZoomButton] != nil;
    }
}
