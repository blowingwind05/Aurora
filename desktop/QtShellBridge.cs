using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
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

internal sealed record QtShellState(string DeviceAccess, string AccountName, string AccountSubtitle,
    QtShellDevice[] Devices, int DeviceEpoch, bool RefreshAvailable, bool Refreshing, string RefreshFeedback,
    bool RefreshFailed, bool ConnectionPending, bool ReduceMotion)
{
    public QtShellHostState Host { get; init; }
}

internal sealed record QtShellRequest(string Action, string DeviceId, int DeviceEpoch);

internal sealed class QtShellBridge : IDisposable
{
    private static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web);
    private readonly Channel<QtShellState> snapshots = Channel.CreateBounded<QtShellState>(
        new BoundedChannelOptions(1) { SingleWriter = true, SingleReader = true, FullMode = BoundedChannelFullMode.DropOldest });
    private readonly TaskCompletionSource stop = new(TaskCreationOptions.RunContinuationsAsynchronously);

    private QtShellBridge(Process child, Action<QtShellRequest> receive)
    {
        Completion = Task.Run(() => Run(child, receive));
    }

    internal Task Completion { get; }
    internal void Publish(QtShellState state) => snapshots.Writer.TryWrite(state);

    internal static QtShellBridge Start(string executable, string qml, Action<QtShellRequest> receive)
    {
        var options = new ProcessStartInfo
        {
            FileName = Path.GetFullPath(executable),
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            StandardInputEncoding = new UTF8Encoding(false),
            StandardOutputEncoding = Encoding.UTF8
        };
        options.ArgumentList.Add(Path.GetFullPath(qml));
        if (!File.Exists(options.ArgumentList[0])) throw new FileNotFoundException("Qt Shell QML entry was not found.");
        return new QtShellBridge(Process.Start(options), receive);
    }

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