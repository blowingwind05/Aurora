// SPDX-License-Identifier: GPL-3.0-only
using System;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Threading;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Interactivity;
using Avalonia.Themes.Fluent;
using Avalonia.Threading;
using Aurora.Replay;

[assembly: SupportedOSPlatform("windows")]

namespace AuroraSessionReplay;

internal sealed class ReplayApp : Application
{
    internal static string Companion;
    internal static string Entry;
    public override void Initialize() => Styles.Add(new FluentTheme());

    public override void OnFrameworkInitializationCompleted()
    {
        base.OnFrameworkInitializationCompleted();
        var lifetime = (IClassicDesktopStyleApplicationLifetime)ApplicationLifetime;
        lifetime.ShutdownMode = ShutdownMode.OnExplicitShutdown;
        Dispatcher.UIThread.UnhandledException += (_, args) =>
        {
            args.Handled = true;
            Stop(ReplayFailureBoundary.Report(args.Exception));
        };
        Dispatcher.UIThread.Post(async () =>
        {
            var result = 0;
            try { await Program.Replay(Companion, Entry); }
            catch (Exception error) { result = ReplayFailureBoundary.Report(error); }
            Stop(result);
        });

        void Stop(int exitCode)
        {
            try { lifetime.Shutdown(exitCode); }
            catch (Exception error) { Environment.Exit(ReplayFailureBoundary.Report(error)); }
        }
    }
}

internal static class Program
{
    private const BindingFlags Instance = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
    private const uint ChildStyle = 0x40000000;
    private const uint VisibleStyle = 0x10000000;
    private const uint ClipSiblings = 0x04000000;
    private const uint GwOwner = 4;
    private const uint GwNext = 2;
    private const uint WmMouseDown = 0x0201;
    private const uint WmMouseUp = 0x0202;

    [STAThread]
    private static int Main(string[] args)
    {
        try
        {
            ReplayFailureBoundary.DisableCrashDialogs();
            if (!OperatingSystem.IsWindows())
                throw new PlatformNotSupportedException("Windows session replay requires Windows.");
            if (args.Length > 0 && args[0] == "stream" &&
                Environment.GetEnvironmentVariable("AURORA_WINDOWS_REPLAY_CHILD") == "1" &&
                Environment.GetEnvironmentVariable("AURORA_EMBEDDED_SESSION") is { } invitation)
                return RunVideo(invitation);
            if (args.Length != 2)
                throw new ArgumentException("Expected the Qt executable and WindowsSessionPopup.qml paths.");
            ReplayApp.Companion = Path.GetFullPath(args[0]);
            ReplayApp.Entry = Path.GetFullPath(args[1]);
            if (!File.Exists(ReplayApp.Companion) || !File.Exists(ReplayApp.Entry))
                throw new FileNotFoundException("The Windows replay companion or QML entry was not found.");
            Environment.SetEnvironmentVariable("AURORA_QT_SESSION", "1");
            Environment.SetEnvironmentVariable("AURORA_WINDOWS_REPLAY_CHILD", "1");
            return AppBuilder.Configure<ReplayApp>().UsePlatformDetect().StartWithClassicDesktopLifetime([]);
        }
        catch (Exception error)
        {
            return ReplayFailureBoundary.Report(error);
        }
    }

    internal static async Task Replay(string companion, string entry)
    {
        var aurora = Assembly.Load("Aurora");
        var windowType = aurora.GetType("Aurora.StreamSessionWindow", throwOnError: true);
        var launchType = aurora.GetType("Aurora.QtShellLaunch", throwOnError: true);
        var launch = Activator.CreateInstance(launchType, companion, entry);
        var window = (Window)windowType.GetConstructors(Instance)[0].Invoke(["Offline replay", null, launch]);
        object Field(string name) => windowType.GetField(name, Instance).GetValue(window);
        object Call(string name, params object[] args) => windowType.GetMethod(name, Instance).Invoke(window, args);
        var session = (string)windowType.GetProperty("SessionName", Instance).GetValue(window);
        var routeType = aurora.GetType("Aurora.StreamHost", throwOnError: true);
        var route = Activator.CreateInstance(routeType, new IPEndPoint(IPAddress.Loopback, 47989), "offline", "Offline replay");

        using var sample = new EventWaitHandle(false, EventResetMode.ManualReset, @"Local\" + session + ".replay-sample");
        using var frames = new EventWaitHandle(false, EventResetMode.AutoReset, @"Local\" + session + ".replay-frame");
        using var cancel = new CancellationTokenSource(TimeSpan.FromSeconds(35));
        try { Call("ShowConnecting", cancel); }
        catch { Call("Finish"); throw; }
        window.WindowState = WindowState.Normal;
        window.Activate();
        var playback = (Task)Call("Run", Environment.ProcessPath, route, cancel.Token);
        nint owner = window.TryGetPlatformHandle().Handle;
        var nativeSurface = (Task<nint>)Field("surface").GetType().GetProperty("Handle", Instance).GetValue(Field("surface"));
        nint videoHost = await nativeSurface.WaitAsync(cancel.Token);
        nint video = 0;
        async Task WaitFor(Func<bool> condition, string message)
        {
            var timeout = Stopwatch.StartNew();
            while (!condition())
            {
                if (timeout.Elapsed > TimeSpan.FromSeconds(7))
                    throw new InvalidOperationException(message);
                await Task.Delay(25, cancel.Token);
            }
        }

        void Click(string field) => ((Button)Field(field)).RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        nint OwnedPopup()
        {
            nint result = 0;
            EnumWindows((hwnd, _) =>
            {
                if (GetWindow(hwnd, GwOwner) == owner && IsWindowVisible(hwnd))
                {
                    GetWindowThreadProcessId(hwnd, out var pid);
                    if (pid != Environment.ProcessId) { result = hwnd; return false; }
                }
                return true;
            }, 0);
            return result;
        }

        void VerifyVideoAndLayer(nint popup)
        {
            Require(GetParent(video) == videoHost && IsWindowVisible(video), "SDL video lost its parent or visibility");
            Require(GetWindow(popup, GwOwner) == owner, "Qt popup has the wrong owner");
            GetWindowRect(popup, out var over);
            GetWindowRect(video, out var under);
            Require(over.Left < under.Right && under.Left < over.Right &&
                    over.Top < under.Bottom && under.Top < over.Bottom,
                    "Qt popup does not overlap the embedded child");
            var below = GetWindow(popup, GwNext);
            while (below != 0 && below != owner) below = GetWindow(below, GwNext);
            Require(below == owner, "Qt popup is not above the session in the Win32 Z order");
        }

        static void ClickFullScreen(nint hwnd)
        {
            GetClientRect(hwnd, out var bounds);
            var scale = GetDpiForWindow(hwnd) / 96.0;
            var point = (nint)(((bounds.Bottom - (int)Math.Round(19 * scale)) << 16) | (bounds.Right / 2));
            PostMessage(hwnd, WmMouseDown, 1, point);
            PostMessage(hwnd, WmMouseUp, 0, point);
        }

        var status = (TextBlock)Field("status");
        try
        {
            await WaitFor(() => (video = FindWindowEx(videoHost, 0, "STATIC", "Replay SDL")) != 0 &&
                status.Text == "已连接" && frames.WaitOne(0), "SDL child never presented a frame");
            await Task.Delay(350, cancel.Token);
            Click("statusButton");
            Require(Field("qtSessionPopup") != null, "Qt popup failed to start or did not opt in");
            nint popup = 0;
            await WaitFor(() => (popup = OwnedPopup()) != 0, "Connection popup never appeared");
            VerifyVideoAndLayer(popup);
            await Task.Delay(400, cancel.Token);
            sample.Set();
            await WaitFor(() => status.Text == "网络较差", "Runtime sample did not update the Avalonia session");
            await Task.Delay(300, cancel.Token);
            Require(OwnedPopup() == popup, "Connection popup was replaced on telemetry update");
            VerifyVideoAndLayer(popup);
            Console.WriteLine("PASS native popup ownership, Z order, video overlap and connection status");

            Click("statusButton");
            await WaitFor(() => OwnedPopup() == 0, "Connection popup did not close");
            Click("controls");
            await WaitFor(() => (popup = OwnedPopup()) != 0, "Control center never appeared");
            VerifyVideoAndLayer(popup);
            await Task.Delay(250, cancel.Token);
            ClickFullScreen(popup);
            await WaitFor(() => window.WindowState == WindowState.FullScreen && OwnedPopup() == 0,
                "Qt full-screen request did not change the real Avalonia WindowState");
            frames.WaitOne(0);
            await WaitFor(() => GetParent(video) == videoHost && IsWindowVisible(video) && frames.WaitOne(0),
                "SDL child stopped presenting after full screen");
            var floating = (Avalonia.Controls.Primitives.Popup)Field("fullscreenControls");
            ((Button)floating.Child).RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
            await WaitFor(() => (popup = OwnedPopup()) != 0, "Full-screen control center never appeared");
            VerifyVideoAndLayer(popup);
            await Task.Delay(250, cancel.Token);
            ClickFullScreen(popup);
            await WaitFor(() => window.WindowState == WindowState.Normal && OwnedPopup() == 0,
                "Qt exit-full-screen request did not restore WindowState");
            Console.WriteLine("PASS Qt entered and exited full screen with the same presenting SDL HWND");
        }
        finally
        {
            cancel.Cancel();
            try { await playback.WaitAsync(TimeSpan.FromSeconds(10)); }
            finally { Call("Finish"); }
        }
    }

    private static int RunVideo(string invitation)
    {
        var fields = invitation.Split(':');
        var parent = (nint)long.Parse(fields[0], System.Globalization.CultureInfo.InvariantCulture);
        var prefix = @"Local\Aurora.Embedded." + fields[2];
        using var ready = EventWaitHandle.OpenExisting(prefix + ".ready");
        using var stop = EventWaitHandle.OpenExisting(prefix + ".stop");
        using var sample = EventWaitHandle.OpenExisting(prefix + ".replay-sample");
        using var frames = EventWaitHandle.OpenExisting(prefix + ".replay-frame");
        var hwnd = CreateWindowEx(0, "STATIC", "Replay SDL", ChildStyle | VisibleStyle | ClipSiblings,
            0, 0, 640, 360, parent, 0, 0, 0);
        nint window = 0, renderer = 0;
        try
        {
            Require(hwnd != 0 && SDL_Init(0x20) == 0, "Cannot initialize replay SDL video");
            window = SDL_CreateWindowFrom(hwnd);
            Require(window != 0, "SDL_CreateWindowFrom failed");
            renderer = SDL_CreateRenderer(window, -1, 1);
            Require(renderer != 0, "SDL software renderer unavailable");
            bool started = false, sentPoor = false;
            int count = 0;
            while (!stop.WaitOne(25) && IsWindow(parent))
            {
                GetClientRect(parent, out var size);
                GetClientRect(hwnd, out var client);
                if (size.Right != client.Right || size.Bottom != client.Bottom)
                    SetWindowPos(hwnd, 0, 0, 0, size.Right, size.Bottom, 0x0004 | 0x0010);
                SDL_PumpEvents();
                SDL_SetRenderDrawColor(renderer, 16, (byte)(96 + count++ % 64), 155, 255);
                Require(SDL_RenderClear(renderer) == 0, "SDL_RenderClear failed");
                SDL_RenderPresent(renderer);
                frames.Set();
                if (!started)
                {
                    ready.Set();
                    Console.WriteLine("aurora-connection/1,1920,1080,1,6000,20000,0,1000,12,0");
                    started = true;
                }
                if (!sentPoor && sample.WaitOne(0))
                {
                    Console.WriteLine("aurora-connection/1,1920,1080,1,5900,18500,100,1200,48,1");
                    sentPoor = true;
                }
            }
            return 0;
        }
        finally
        {
            if (renderer != 0) SDL_DestroyRenderer(renderer);
            if (window != 0) SDL_DestroyWindow(window);
            SDL_Quit();
            if (hwnd != 0) DestroyWindow(hwnd);
        }
    }

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Rect { public int Left, Top, Right, Bottom; }
    private delegate bool EnumProc(nint hwnd, nint parameter);
    [DllImport("user32")] private static extern bool EnumWindows(EnumProc callback, nint parameter);
    [DllImport("user32")] private static extern nint GetWindow(nint hwnd, uint command);
    [DllImport("user32")] private static extern nint GetParent(nint hwnd);
    [DllImport("user32")] private static extern bool IsWindow(nint hwnd);
    [DllImport("user32")] private static extern bool IsWindowVisible(nint hwnd);
    [DllImport("user32")] private static extern uint GetDpiForWindow(nint hwnd);
    [DllImport("user32")] private static extern bool GetWindowRect(nint hwnd, out Rect bounds);
    [DllImport("user32")] private static extern bool GetClientRect(nint hwnd, out Rect bounds);
    [DllImport("user32")] private static extern uint GetWindowThreadProcessId(nint hwnd, out int processId);
    [DllImport("user32", CharSet = CharSet.Unicode)] private static extern nint FindWindowEx(nint parent, nint after,
        string className, string title);
    [DllImport("user32", CharSet = CharSet.Unicode)] private static extern bool PostMessage(nint hwnd, uint message,
        nint wParam, nint lParam);
    [DllImport("user32", CharSet = CharSet.Unicode)] private static extern nint CreateWindowEx(uint exStyle, string className,
        string title, uint style, int x, int y, int w, int h, nint parent, nint menu, nint instance, nint param);
    [DllImport("user32")] private static extern bool DestroyWindow(nint hwnd);
    [DllImport("user32")] private static extern bool SetWindowPos(nint hwnd, nint after, int x, int y, int w, int h, uint flags);
    [DllImport("SDL2", CallingConvention = CallingConvention.Cdecl)] private static extern int SDL_Init(uint flags);
    [DllImport("SDL2", CallingConvention = CallingConvention.Cdecl)] private static extern nint SDL_CreateWindowFrom(nint hwnd);
    [DllImport("SDL2", CallingConvention = CallingConvention.Cdecl)] private static extern void SDL_DestroyWindow(nint window);
    [DllImport("SDL2", CallingConvention = CallingConvention.Cdecl)] private static extern nint SDL_CreateRenderer(nint window, int index, uint flags);
    [DllImport("SDL2", CallingConvention = CallingConvention.Cdecl)] private static extern void SDL_DestroyRenderer(nint renderer);
    [DllImport("SDL2", CallingConvention = CallingConvention.Cdecl)] private static extern int SDL_SetRenderDrawColor(nint renderer, byte r, byte g, byte b, byte a);
    [DllImport("SDL2", CallingConvention = CallingConvention.Cdecl)] private static extern int SDL_RenderClear(nint renderer);
    [DllImport("SDL2", CallingConvention = CallingConvention.Cdecl)] private static extern void SDL_RenderPresent(nint renderer);
    [DllImport("SDL2", CallingConvention = CallingConvention.Cdecl)] private static extern void SDL_PumpEvents();
    [DllImport("SDL2", CallingConvention = CallingConvention.Cdecl)] private static extern void SDL_Quit();
}
