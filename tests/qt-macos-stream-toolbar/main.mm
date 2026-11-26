// SPDX-License-Identifier: GPL-3.0-only
#define SDL_MAIN_HANDLED
#include "streaming/aurora-session-toolbar.h"
#include "streaming/aurora-session-privacy.h"
#include "qt-session/AuroraMacTitleBarView.h"
#include "qt-session/AuroraMacControlsView.h"
#include "qt-session/AuroraMacPrivacyAppearanceView.h"

#import <AppKit/AppKit.h>
#include <SDL_metal.h>
#include <SDL_syswm.h>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQuickStyle>
#include <QPointer>
#include <QTimer>
#include <cstdio>
#include <cstdlib>

AuroraPrivacyState runAuroraPrivacyCommand(const AuroraPrivacyTarget& target, AuroraPrivacyOperation action,
                                           const std::atomic<bool>&)
{
    if (target.program == QStringLiteral("toolbar-fixture") && action == AuroraPrivacyOperation::Status)
        return AuroraPrivacyState::Off;
    std::abort();
}

AuroraPrivacyAppearanceReply runAuroraPrivacyAppearance(const AuroraPrivacyTarget& target,
    AuroraPrivacyAppearanceAction action, const QString& file, const std::atomic<bool>& sessionLive,
    const std::atomic<bool>& editorLive)
{
    if (target.program == QStringLiteral("toolbar-fixture") && action == AuroraPrivacyAppearanceAction::Read &&
        file.isEmpty() && sessionLive && editorLive)
        return {AuroraPrivacyAppearanceState::Default, {}};
    std::abort();
}

QByteArray runAuroraDisplayCommand(const AuroraPrivacyTarget& target, const QString& operation, const QString& id,
                                   const std::atomic<bool>& live)
{
    if (target.program == QStringLiteral("toolbar-fixture") && operation == QStringLiteral("list") && id.isEmpty() && live)
        return QByteArrayLiteral("unsupported");
    std::abort();
}

QByteArray runAuroraBitrateCommand(const AuroraPrivacyTarget&, const AuroraBitrate::Request&,
                                  const std::atomic<bool>&)
{
    std::abort();
}

namespace {

bool check(bool value, const char* message)
{
    if (!value) std::fprintf(stderr, "FAIL %s\n", message);
    return value;
}

void postClick(NSView* view, const QRectF& bounds)
{
    const CGPoint anchor = CGPointMake(bounds.center().x(),
        view.isFlipped ? bounds.center().y() : NSHeight(view.bounds) - bounds.center().y());
    const NSPoint point = [view convertPoint:anchor toView:nil];
    const NSTimeInterval now = NSProcessInfo.processInfo.systemUptime;
    for (NSEventType type : {NSEventTypeLeftMouseDown, NSEventTypeLeftMouseUp}) {
        [NSApp postEvent:[NSEvent mouseEventWithType:type location:point
            modifierFlags:0 timestamp:now windowNumber:view.window.windowNumber
            context:nil eventNumber:0 clickCount:1 pressure:type == NSEventTypeLeftMouseDown ? 1 : 0] atStart:NO];
    }
}

void postKey(NSWindow* window, unsigned short code, NSString* text)
{
    const NSTimeInterval now = NSProcessInfo.processInfo.systemUptime;
    for (NSEventType type : {NSEventTypeKeyDown, NSEventTypeKeyUp}) {
        [NSApp postEvent:[NSEvent keyEventWithType:type location:NSZeroPoint
            modifierFlags:0 timestamp:now windowNumber:window.windowNumber
            context:nil characters:text charactersIgnoringModifiers:text isARepeat:NO keyCode:code] atStart:NO];
    }
}

struct Viewer {
    SDL_Window* sdl = nullptr;
    SDL_MetalView metal = nullptr;
    NSWindow* window = nil;
    NSView* video = nil;
    NSSize videoSize = {};
    NSRect metalFrame = {};
    int releaseEvents = 0;
    int localKeys = 0;
    int forwardedKeys = 0;

    Viewer()
    {
        sdl = SDL_CreateWindow("Aurora toolbar fixture", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               850, 440, SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_METAL);
        if (!sdl) return;
        SDL_SysWMinfo info{};
        SDL_VERSION(&info.version);
        if (!SDL_GetWindowWMInfo(sdl, &info) || info.subsystem != SDL_SYSWM_COCOA) return;
        window = info.info.cocoa.window;
        // Production starts with a visible SDL window. Capture the baseline
        // after Cocoa applies its initial on-screen frame and backing scale.
        SDL_ShowWindow(sdl);
        SDL_PumpEvents();
        metal = SDL_Metal_CreateView(sdl);
        video = window.contentView;
        videoSize = video.bounds.size;
        metalFrame = reinterpret_cast<NSView*>(metal).frame;
    }

    ~Viewer()
    {
        if (!sdl) return;
        closeAuroraFullscreenControls(sdl);
        if (metal) SDL_Metal_DestroyView(metal);
        SDL_DestroyWindow(sdl);
    }

    bool install(const std::shared_ptr<AuroraSessionPrivacy>& privacy = {})
    {
        if (!window || !metal ||
            !installAuroraSessionToolbar(sdl, QStringLiteral("Fixture"), false, privacy, 850, 440)) return false;
        SDL_ShowWindow(sdl);
        [NSApp activateIgnoringOtherApps:YES];
        [window makeKeyAndOrderFront:nil];
        return true;
    }

    template<class Predicate>
    bool wait(Predicate done)
    {
        QElapsedTimer deadline;
        deadline.start();
        while (!done() && !deadline.hasExpired(2500)) {
            @autoreleasepool {
                serviceAuroraQtChrome(sdl);
                SDL_Event event{};
                if (!SDL_WaitEventTimeout(&event, 16)) continue;
                if (event.type == SDL_USEREVENT && event.user.code == static_cast<Sint32>(AuroraToolbarAction::ReleaseMouse))
                    ++releaseEvents;
                if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
                    if (auroraRouteLocalSessionKey(sdl, event.key, false))
                        ++localKeys;
                    else
                        ++forwardedKeys;
                }
            }
        }
        return done();
    }
};

NSButton* appearanceButton(NSView* root)
{
    if ([root isKindOfClass:NSButton.class] && [root.identifier isEqualToString:@"auroraPrivacyAppearance"])
        return (NSButton*)root;
    for (NSView* child in root.subviews) {
        if (auto* button = appearanceButton(child)) return button;
    }
    return nil;
}

bool testAppearanceEditor(Viewer& viewer)
{
    QPointer<AuroraMacControlsView> home;
    for (auto* window : QGuiApplication::allWindows()) {
        if (auto* controls = qobject_cast<AuroraMacControlsView*>(window)) home = controls;
    }
    if (!check(home, "find the Qt controls panel")) return false;
    auto* homeView = reinterpret_cast<NSView*>(home->winId());
    auto* panel = homeView.window;
    auto* safety = home->rootObject()->findChild<QQuickItem*>(QStringLiteral("controlPage:safety"));
    if (!check(panel && safety, "locate the safety page button")) return false;
    postClick(homeView, safety->mapRectToScene(QRectF(0, 0, safety->width(), safety->height())));
    if (!check(viewer.wait([&] { return appearanceButton(panel.contentView).enabled; }), "open the AppKit safety page"))
        return false;
    auto* button = appearanceButton(panel.contentView);
    postClick(button, QRectF(0, 0, NSWidth(button.bounds), NSHeight(button.bounds)));

    QPointer<AuroraMacPrivacyAppearanceView> editor;
    if (!check(viewer.wait([&] {
        for (auto* window : QGuiApplication::allWindows()) {
            if (auto* view = qobject_cast<AuroraMacPrivacyAppearanceView*>(window)) editor = view;
        }
        return editor && editor->rootObject()->property("available").toBool();
    }), "load the appearance editor and read the fixture default")) return false;
    auto* view = reinterpret_cast<NSView*>(editor->winId());
    if (!check(view.window == panel && panel.parentWindow == viewer.window &&
               (panel.collectionBehavior & NSWindowCollectionBehaviorFullScreenAuxiliary) && auroraPrivacyEditorOwnsInput(),
        "reuse the playback auxiliary panel and route input locally")) return false;
    auto* close = editor->rootObject()->findChild<QQuickItem*>(QStringLiteral("privacyEditorClose"));
    if (!check(close, "locate the appearance editor close button")) return false;
    postClick(view, close->mapRectToScene(QRectF(0, 0, close->width(), close->height())));
    return check(viewer.wait([&] { return !auroraPrivacyEditorOwnsInput() &&
                       appearanceButton(panel.contentView).enabled; }), "close editor and restore the safety page");
}

bool testQtToolbar()
{
    Viewer viewer;
    auto privacy = std::make_shared<AuroraSessionPrivacy>(
        AuroraPrivacyTarget{QStringLiteral("toolbar-fixture"), {}, {}, {}, {}});
    privacy->state = AuroraPrivacyState::Off;
    privacy->lastStatusCheck.start();
    if (!check(viewer.install(privacy), "create SDL/Metal window and install toolbar")) return false;

    QPointer<AuroraMacTitleBarView> bar;
    for (auto* window : QGuiApplication::allWindows()) {
        if (auto* view = qobject_cast<AuroraMacTitleBarView*>(window)) bar = view;
    }
    if (!check(bar, "Qt titlebar exists")) return false;
    bool frame = false;
    QObject frameObserver;
    QObject::connect(bar.data(), &QQuickWindow::frameSwapped, &frameObserver,
                     [&] { frame = true; }, Qt::QueuedConnection);
    bar->requestUpdate();
    if (!check(viewer.wait([&] { return frame && viewer.window.keyWindow; }), "draw through the SDL Cocoa pump")) return false;

    auto* view = reinterpret_cast<NSView*>(bar->winId());
    const auto preservesVideoGeometry = [&] {
        if (!bar) return false;
        const NSRect toolbar = [view convertRect:view.bounds toView:nil];
        const NSRect video = [viewer.video convertRect:viewer.video.bounds toView:nil];
        return view.window == viewer.window && NSHeight(toolbar) == 44 &&
               NSMinY(toolbar) >= NSMaxY(video) - 1 &&
               viewer.video == viewer.window.contentView &&
               NSEqualSizes(viewer.videoSize, viewer.video.bounds.size) &&
               NSEqualRects(viewer.metalFrame, reinterpret_cast<NSView*>(viewer.metal).frame);
    };
    if (!check(viewer.wait(preservesVideoGeometry), "44-point toolbar outside the SDL/Metal view")) {
        const auto height = bar ? NSHeight(view.bounds) : 0;
        const auto title = bar ? [view convertRect:view.bounds toView:nil] : NSZeroRect;
        const auto video = [viewer.video convertRect:viewer.video.bounds toView:nil];
        const auto metal = reinterpret_cast<NSView*>(viewer.metal).frame;
        std::fprintf(stderr,
            "Cocoa layout: title=%s, height=%.2f, video=%s, expected video=%s, metal=%s, expected metal=%s, same content=%d, same window=%d\n",
            NSStringFromRect(title).UTF8String, double(height), NSStringFromRect(video).UTF8String,
            NSStringFromSize(viewer.videoSize).UTF8String, NSStringFromRect(metal).UTF8String,
            NSStringFromRect(viewer.metalFrame).UTF8String, viewer.video == viewer.window.contentView,
            bar && view.window == viewer.window);
        return false;
    }
    if (!check([viewer.window standardWindowButton:NSWindowCloseButton] &&
               [viewer.window standardWindowButton:NSWindowMiniaturizeButton] &&
               [viewer.window standardWindowButton:NSWindowZoomButton], "three native window buttons")) return false;

    postClick(view, bar->connectionRect());
    if (!check(viewer.wait([&] {
        return bar && bar->rootObject()->property("panelKind").toString() == QStringLiteral("connection");
    }), "Cocoa coordinate click opens connection details")) return false;
    postClick(view, bar->controlsRect());
    if (!check(viewer.wait([&] {
        return bar && bar->rootObject()->property("panelKind").toString() == QStringLiteral("controls");
    }), "Cocoa coordinate click reaches the existing Qt control center")) return false;

    postKey(NSApp.keyWindow, 53, @"\033");
    if (!check(viewer.wait([&] {
        return bar && bar->rootObject()->property("panelKind").toString().isEmpty() && viewer.window.keyWindow;
    }), "Escape restores parent focus")) return false;

    auroraRestoreSessionVideoFocus(viewer.sdl);
    const int alreadyLocal = viewer.localKeys;
    const int forwarded = viewer.forwardedKeys;
    postKey(viewer.window, 97, [NSString stringWithFormat:@"%C", (unichar)NSF6FunctionKey]);
    if (!check(viewer.wait([&] {
        return bar && viewer.localKeys >= alreadyLocal + 2 &&
            bar->activeFocusItem() && bar->activeFocusItem()->objectName() == QStringLiteral("controlCenter");
    }) && viewer.forwardedKeys == forwarded, "F6 reaches the Qt titlebar and stays local")) return false;
    postKey(viewer.window, 49, @" ");
    if (!check(viewer.wait([&] {
        return bar && bar->rootObject()->property("panelKind").toString() == QStringLiteral("controls");
    }) && viewer.forwardedKeys == forwarded, "Space opens controls without forwarding a video key")) return false;

    if (!testAppearanceEditor(viewer)) return false;
    postKey(NSApp.keyWindow, 53, @"\033");
    if (!check(viewer.wait([&] {
        for (auto* window : QGuiApplication::allWindows()) {
            if (auto* view = qobject_cast<AuroraMacControlsView*>(window)) return view->isVisible();
        }
        return false;
    }), "return to the control center home")) return false;
    postKey(NSApp.keyWindow, 53, @"\033");
    if (!check(viewer.wait([&] { return bar && bar->rootObject()->property("panelKind").toString().isEmpty(); }),
               "close the menu before resizing")) return false;

    SDL_SetWindowSize(viewer.sdl, 420, 250);
    if (!check(viewer.wait([&] { return bar && bar->rootObject()->property("tight").toBool(); }),
               "Qt titlebar follows an SDL window resize")) return false;
    if (!check(bar->hasButtonAt(bar->controlsRect().center()) &&
               bar->hasButtonAt(bar->connectionRect().center()) && viewer.releaseEvents >= 2,
               "narrow hit targets and existing SDL release actions")) return false;

    [view setHidden:YES];
    return check(viewer.wait([&] {
        return !bar && [viewer.window.toolbar.items.firstObject.itemIdentifier isEqualToString:@"AuroraSessionContext"];
    }), "loss of the Cocoa host selects the native fallback");
}

bool testNativeFallback()
{
    qputenv("AURORA_QT_SESSION_TOOLBAR", "0");
    Viewer viewer;
    if (!check(viewer.install(), "install fallback toolbar")) return false;
    return check([viewer.window.toolbar.items.firstObject.itemIdentifier isEqualToString:@"AuroraSessionContext"] &&
                 viewer.video == viewer.window.contentView, "native toolbar opt-out preserves SDL content");
}

} // namespace

int main(int argc, char** argv)
{
    qputenv("AURORA_QT_SESSION", "1");
    qputenv("AURORA_QT_SESSION_TOOLBAR", "1");
    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QQuickStyle::setStyle(QStringLiteral("Fusion"));
    if (!check(QGuiApplication::platformName() == QStringLiteral("cocoa"), "Cocoa is required")) return 1;
    SDL_SetMainReady();
    if (!check(SDL_Init(SDL_INIT_VIDEO) == 0, SDL_GetError())) return 1;
    QTimer::singleShot(0, &app, [&] {
        const bool passed = testQtToolbar() && testNativeFallback();
        if (passed) std::puts("PASS windowed Qt titlebar, Cocoa delivery, local focus, and native fallback");
        app.exit(passed ? 0 : 1);
    });
    const int result = app.exec();
    SDL_Quit();
    return result;
}
