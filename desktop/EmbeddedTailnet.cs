using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace Aurora;

internal static class EmbeddedTailnet
{
    private static readonly object Gate = new();
    private static readonly SemaphoreSlim HostConfiguration = new(1);
    private static Helper helper;
    private static bool shuttingDown;

    private static string Executable => OperatingSystem.IsMacOS()
        ? Path.Combine(AppContext.BaseDirectory, "AuroraTailnet")
        : Path.Combine(AppContext.BaseDirectory, "Components", "Tailscale", "AuroraTailnet.exe");

    private static string StateDirectory => Path.Combine(Environment.GetFolderPath(OperatingSystem.IsMacOS()
        ? Environment.SpecialFolder.ApplicationData : Environment.SpecialFolder.LocalApplicationData), "Aurora", "Tailnet");

    internal static bool IsAvailable => (OperatingSystem.IsWindows() || OperatingSystem.IsMacOS()) && File.Exists(Executable);

    internal static Task<JsonElement> Request(string method, object args = null, CancellationToken token = default)
    {
        token.ThrowIfCancellationRequested();
        lock (Gate)
        {
            if (shuttingDown) throw new ObjectDisposedException(nameof(EmbeddedTailnet));
            if (!IsAvailable) throw new InvalidOperationException("这个版本没有内置 Tailscale 节点。");
            if (helper?.IsAlive != true)
            {
                helper?.Dispose();
                helper = new Helper();
            }
            return helper.Request(method, args, token);
        }
    }

    internal static string ReadStatus() => Request("status").GetAwaiter().GetResult().GetRawText();

    internal static async Task ConfigureHost(NetworkSnapshot network, AuroraPresence presence = null, CancellationToken token = default)
    {
        await HostConfiguration.WaitAsync(token).ConfigureAwait(false);
        try
        {
            var active = network?.IsAuthenticated == true && network.Mode == TailscaleMode.Embedded &&
                TailscaleRuntime.Mode == network.Mode && network.Generation == TailscaleRuntime.Generation &&
                !TailscaleRuntime.IsTransitioning;
            Helper node;
            lock (Gate) node = helper?.IsAlive == true ? helper : null;
            if (node == null) return;
            if (!active)
            {
                try { await node.Request("publish", new { enabled = false }, token).ConfigureAwait(false); }
                finally { await node.Request("host", new { enabled = false }, token).ConfigureAwait(false); }
                return;
            }
            await node.Request("host", new { enabled = BundledSunshine.IsRunning }, token).ConfigureAwait(false);
            await node.Request("publish", new { enabled = true, owner = network.Owner, nodeId = network.LocalId,
                packet = presence.EmbeddedAnnouncement() }, token).ConfigureAwait(false);
        }
        finally { HostConfiguration.Release(); }
    }

    internal static async Task ResumeIfConfigured()
    {
        if (!IsAvailable || !Directory.Exists(StateDirectory)) return;
        try { await Request("status").ConfigureAwait(false); }
        catch (Exception error) when (error is IOException or InvalidOperationException or TimeoutException)
        { Debug.WriteLine(error.Message); }
    }

    internal static void Stop()
    {
        lock (Gate)
        {
            shuttingDown = true;
            helper?.Dispose();
            helper = null;
        }
    }

    internal sealed class StreamLease : IDisposable
    {
        private readonly Helper owner;
        private readonly string lease;
        public StreamHost Route { get; }

        internal StreamLease(StreamHost route, Helper owner = null, string lease = null)
        {
            Route = route;
            this.lease = lease;
            this.owner = owner;
        }

        public void Dispose()
        {
            if (owner != null && lease != null) _ = Close();
        }

        private async Task Close()
        {
            try { await owner.Request("close", new { lease }, CancellationToken.None).ConfigureAwait(false); }
            catch (Exception error) when (error is IOException or InvalidOperationException or OperationCanceledException or TimeoutException)
            { Debug.WriteLine(error.Message); }
        }
    }

    internal static async Task<StreamLease> OpenStream(StreamHost route, CancellationToken token)
    {
        if (route?.ViaEmbeddedTailnet != true) return new(route);
        if (TailscaleRuntime.Mode != TailscaleMode.Embedded || TailscaleRuntime.IsTransitioning)
            throw new InvalidOperationException("Aurora 的 Tailscale 连接方式已切换，请重新连接。");
        var generation = TailscaleRuntime.Generation;
        Helper owner;
        Task<JsonElement> opening;
        lock (Gate)
        {
            token.ThrowIfCancellationRequested();
            // Consume the allocation reply after cancellation so the lease still gets released.
            opening = Request("open", new { address = route.Endpoint.Address.ToString(), port = route.Endpoint.Port });
            owner = helper;
        }
        var result = await opening.ConfigureAwait(false);
        var local = new IPEndPoint(IPAddress.Loopback, result.GetProperty("port").GetInt32());
        var lease = new StreamLease(route with { Endpoint = local }, owner,
            result.GetProperty("lease").GetString());
        if (!token.IsCancellationRequested && generation == TailscaleRuntime.Generation && !TailscaleRuntime.IsTransitioning) return lease;
        lease.Dispose();
        token.ThrowIfCancellationRequested();
        throw new InvalidOperationException("Aurora 的 Tailscale 连接方式已切换，请重新连接。");
    }

    internal sealed class Helper : IDisposable
    {
        private readonly Process process;
        private readonly SemaphoreSlim writer = new(1);
        private readonly ConcurrentDictionary<long, TaskCompletionSource<JsonElement>> pending = new();
        private long sequence;
        private volatile bool closed;

        internal bool IsAlive => !closed && !process.HasExited;

        internal Helper()
        {
            var options = new ProcessStartInfo(Executable)
            {
                UseShellExecute = false, CreateNoWindow = true, RedirectStandardInput = true,
                RedirectStandardOutput = true, RedirectStandardError = true,
                StandardInputEncoding = new UTF8Encoding(false), StandardOutputEncoding = Encoding.UTF8
            };
            options.ArgumentList.Add("--state-dir");
            options.ArgumentList.Add(StateDirectory);
            process = Process.Start(options) ?? throw new InvalidOperationException("无法启动内置 Tailscale 节点。");
            process.ErrorDataReceived += (_, _) => { };
            process.BeginErrorReadLine();
            _ = ReadResponses();
        }

        internal async Task<JsonElement> Request(string method, object args, CancellationToken token)
        {
            using var deadline = CancellationTokenSource.CreateLinkedTokenSource(token);
            deadline.CancelAfter(TimeSpan.FromSeconds(12));
            var id = Interlocked.Increment(ref sequence);
            var reply = new TaskCompletionSource<JsonElement>(TaskCreationOptions.RunContinuationsAsynchronously);
            pending.TryAdd(id, reply);
            try
            {
                await writer.WaitAsync(deadline.Token).ConfigureAwait(false);
                try
                {
                    if (!IsAlive) throw new IOException("内置 Tailscale 节点已关闭。");
                    var json = JsonSerializer.Serialize(new { id, method, @params = args });
                    await process.StandardInput.WriteLineAsync(json).ConfigureAwait(false);
                    await process.StandardInput.FlushAsync().ConfigureAwait(false);
                }
                finally { writer.Release(); }
                return await reply.Task.WaitAsync(deadline.Token).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (!token.IsCancellationRequested)
            { throw new TimeoutException("内置 Tailscale 节点未及时响应。"); }
            finally { pending.TryRemove(id, out _); }
        }

        private async Task ReadResponses()
        {
            try
            {
                while (await process.StandardOutput.ReadLineAsync().ConfigureAwait(false) is { } line)
                {
                    using var document = JsonDocument.Parse(line);
                    var response = document.RootElement;
                    if (!pending.TryGetValue(response.GetProperty("id").GetInt64(), out var waiter)) continue;
                    if (response.GetProperty("ok").GetBoolean())
                        waiter.TrySetResult(response.TryGetProperty("result", out var result) ? result.Clone() : default);
                    else
                        waiter.TrySetException(new InvalidOperationException(response.TryGetProperty("error", out var reason)
                            ? reason.GetString() : "内置 Tailscale 请求失败。"));
                }
            }
            catch (Exception error) when (error is IOException or InvalidOperationException or JsonException or System.Collections.Generic.KeyNotFoundException)
            { Debug.WriteLine(error.Message); }
            finally
            {
                closed = true;
                foreach (var waiter in pending.Values)
                    waiter.TrySetException(new IOException("与内置 Tailscale 节点的连接已关闭。"));
            }
        }

        public void Dispose()
        {
            closed = true;
            try { process.StandardInput.Close(); }
            catch (Exception error) when (error is IOException or InvalidOperationException) { Debug.WriteLine(error.Message); }
            process.Dispose();
        }
    }
}
