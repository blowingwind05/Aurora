using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Channels;
using System.Threading.Tasks;

namespace Aurora;

internal sealed record QtShellDevice(string DeviceId, string DisplayName, bool Online, bool CanConnect,
    string LanAddress, string TailscaleAddress, string WallpaperUrl);

internal sealed record QtShellHostState(bool Available, string Status, bool Managed, bool Allowed,
    bool ToggleAvailable, bool Busy, bool RefreshAvailable, bool Refreshing, bool RetryAvailable,
    string LanAddress, string TailscaleAddress, bool PermissionsAvailable, bool SettingsAvailable,
    string Hint, string Feedback, bool FeedbackFailed);

internal sealed record QtShellGeneralPreferences(int AcknowledgedRequestId, bool CloseToTray, bool TrayAvailable,
    bool StartupEnabled, bool StartupAvailable, string SleepMode, string CloseFeedback, string StartupFeedback,
    string SleepFeedback);

internal sealed record QtShellState(string DeviceAccess, string AccountName, string AccountSubtitle,
    QtShellDevice[] Devices, int DeviceEpoch, bool RefreshAvailable, bool Refreshing, string RefreshFeedback,
    bool RefreshFailed, bool ConnectionPending, bool ReduceMotion)
{
    public QtShellHostState Host { get; init; }
    public QtShellGeneralPreferences GeneralSettings { get; init; }
    public QtShellTailscaleAccount TailscaleAccount { get; init; }
    public bool ManagedWindow { get; init; }
    public bool WindowVisible { get; init; } = true;
    public int WindowActivation { get; init; }
    public string WindowPage { get; init; }
}

internal sealed record QtShellTailscaleAccount(string Mode, string ModeLabel, bool SupportsEmbedded,
    bool HasSystemRuntime, string RuntimeNotice, string State, string Status, string Detail, string DisplayName,
    string LoginName, string Initial, string TailnetName, string LocalAddress, string PrimaryAction,
    bool CanOpenPrimaryAction, bool RefreshAvailable, bool Refreshing)
{
    public int Revision { get; init; }
    public bool Busy { get; init; }
    public QtShellTailscaleProfile[] Profiles { get; init; } = [];
    public QtShellTailscaleConfirmation Confirmation { get; init; }
    public string Progress { get; init; }
    public bool RuntimeProgress { get; init; }
    public string Feedback { get; init; }
    public bool RuntimeFeedback { get; init; }
    public bool RetryAvailable { get; init; }
    public string Notice { get; init; }
    public int NoticeVersion { get; init; }
    public int FocusVersion { get; init; }
}

internal sealed record QtShellTailscaleProfile(string Key, string Name, string LoginName, string Tailnet, bool Current);
internal sealed record QtShellTailscaleConfirmation(int Id, string Title, string Body, string Label, bool Destructive);

internal sealed record QtShellRequest(string Action, string DeviceId, int DeviceEpoch,
    int PreferenceRequestId = 0, JsonElement? PreferenceValue = null)
{
    public string AccountTarget { get; init; }
    public int AccountRevision { get; init; }
    public bool? FullScreen { get; init; }
}

internal sealed record QtShellLaunch(string Executable, string EntryPoint);
internal sealed record WindowsQtConnectionMetrics(string Latency);
internal sealed record WindowsQtSessionState(string PanelKind, int PixelX, int PixelY, int HeightLimit,
    bool FullScreen, bool Poor, WindowsQtConnectionMetrics ConnectionMetrics);

internal sealed class QtShellBridge : IDisposable
{
    private static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web);
    private readonly Channel<object> snapshots = Channel.CreateBounded<object>(
        new BoundedChannelOptions(1) { SingleWriter = true, SingleReader = true, FullMode = BoundedChannelFullMode.DropOldest });
    private readonly TaskCompletionSource stop = new(TaskCreationOptions.RunContinuationsAsynchronously);

    private QtShellBridge(Process child, QtShellLaunch launch, Action<QtShellRequest> receive)
    {
        Launch = launch;
        Completion = Task.Run(() => Run(child, receive));
    }

    internal Task Completion { get; }
    internal QtShellLaunch Launch { get; }
    internal void Publish(QtShellState state) => snapshots.Writer.TryWrite(state);
    internal void Publish(WindowsQtSessionState state) => snapshots.Writer.TryWrite(state);

    internal static QtShellBridge Start(string executable, string qml, Action<QtShellRequest> receive)
    {
        return Start(new QtShellLaunch(Path.GetFullPath(executable), Path.GetFullPath(qml)), 0, receive);
    }

    internal static QtShellBridge StartWindowsSession(QtShellLaunch launch, nint owner, Action<QtShellRequest> receive)
    {
        return Start(launch, owner, receive);
    }

    private static QtShellBridge Start(QtShellLaunch launch, nint owner, Action<QtShellRequest> receive)
    {
        if (!File.Exists(launch.EntryPoint)) throw new FileNotFoundException("Qt Shell QML entry was not found.");
        var options = new ProcessStartInfo
        {
            FileName = launch.Executable,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = owner != 0,
            StandardInputEncoding = new UTF8Encoding(false),
            StandardOutputEncoding = Encoding.UTF8
        };
        if (owner != 0) options.ArgumentList.Add("--windows-session");
        options.ArgumentList.Add(launch.EntryPoint);
        if (owner != 0)
        {
            options.ArgumentList.Add(FormattableString.Invariant($"{owner.ToInt64()}"));
            options.ArgumentList.Add(FormattableString.Invariant($"{Environment.ProcessId}"));
        }
        var child = Process.Start(options);
        if (owner != 0)
        {
            AllowSetForegroundWindow((uint)child.Id);
            child.ErrorDataReceived += (_, line) =>
            {
                if (line.Data != null) Console.Error.WriteLine(line.Data);
            };
            child.BeginErrorReadLine();
        }
        return new QtShellBridge(child, launch, receive);
    }

    [DllImport("user32.dll")]
    private static extern bool AllowSetForegroundWindow(uint processId);

    private async Task Run(Process child, Action<QtShellRequest> receive)
    {
        using (child)
        {
            using var shutdown = new CancellationTokenSource();
            var read = ReadRequests(child.StandardOutput, receive, shutdown.Token);
            var write = WriteSnapshots(child.StandardInput, shutdown.Token);
            try
            {
                var finished = await Task.WhenAny(read, write, stop.Task);
                await finished;
            }
            catch (Exception error) when (error is IOException or OperationCanceledException)
            {
                Debug.WriteLine(error.Message);
            }
            finally
            {
                snapshots.Writer.TryComplete();
                shutdown.Cancel();
                // Closing either side ends the session. Killing the child also unblocks anonymous pipe I/O.
                try { if (!child.HasExited) child.Kill(); }
                catch (Exception error) when (error is InvalidOperationException or Win32Exception)
                { Debug.WriteLine(error.Message); }
                try { await Task.WhenAll(read, write, child.WaitForExitAsync()); }
                catch (Exception error) when (error is IOException or OperationCanceledException)
                { Debug.WriteLine(error.Message); }
            }
        }
    }

    private static async Task ReadRequests(StreamReader output, Action<QtShellRequest> receive, CancellationToken token)
    {
        while (await output.ReadLineAsync(token) is { } line)
        {
            QtShellRequest request;
            try { request = JsonSerializer.Deserialize<QtShellRequest>(line, Json); }
            catch (JsonException) { continue; }
            if (request != null) receive(request);
        }
    }

    private async Task WriteSnapshots(StreamWriter input, CancellationToken token)
    {
        // Write to the redirected stream so no synchronous StreamWriter auto-flush can block shutdown.
        await foreach (var state in snapshots.Reader.ReadAllAsync(token))
        {
            var line = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(state, Json) + "\n");
            await input.BaseStream.WriteAsync(line, token);
        }
    }

    public void Dispose() => stop.TrySetResult();
}