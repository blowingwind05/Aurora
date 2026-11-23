using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

namespace Aurora;

internal enum TailscaleMode { System, Embedded }

internal sealed record TailscaleProfile(string Id, string Name, string LoginName, string Tailnet, bool IsCurrent)
{
    public override string ToString()
    {
        var label = string.IsNullOrWhiteSpace(Name) || Name == LoginName ? LoginName : $"{Name} · {LoginName}";
        return string.IsNullOrWhiteSpace(Tailnet) || Tailnet == LoginName ? label : $"{label} · {Tailnet}";
    }
}

internal static class TailscaleRuntime
{
    private static readonly string SettingsPath = Path.Combine(Environment.GetFolderPath(OperatingSystem.IsMacOS()
        ? Environment.SpecialFolder.ApplicationData : Environment.SpecialFolder.LocalApplicationData), "Aurora", "tailnet-provider");
    private static readonly SemaphoreSlim Transition = new(1, 1);
    private static volatile TailscaleMode mode = ReadMode();
    private static volatile bool transitioning;
    private static int generation;

    internal static TailscaleMode Mode => mode;
    internal static bool IsTransitioning => transitioning;
    internal static int Generation => Volatile.Read(ref generation);
    internal static bool SupportsEmbedded => EmbeddedTailnet.IsAvailable;
    internal static bool HasSystemRuntime => Upstream.HasSystemTailscale;
    internal static event Action ModeChanged;
    internal static event Action ContextChanging;
    internal static event Action ContextChanged;

    internal static async Task SwitchAsync(TailscaleMode next)
    {
        if (next is not (TailscaleMode.System or TailscaleMode.Embedded)) throw new ArgumentOutOfRangeException(nameof(next));
        if (next == TailscaleMode.Embedded && !SupportsEmbedded)
            throw new InvalidOperationException("此版本无法切换 Tailscale 运行方式。");
        await Transition.WaitAsync();
        var modeChanged = false;
        try
        {
            if (next == mode) return;
            if (next == TailscaleMode.System && !HasSystemRuntime)
                throw new InvalidOperationException("系统 Tailscale 暂不可用。");
            BeginTransition();
            if (next == TailscaleMode.Embedded) await EmbeddedTailnet.Request("status");
            await Task.Run(() => SaveMode(next));
            mode = next;
            modeChanged = true;
        }
        finally
        {
            EndTransition();
            if (modeChanged) ModeChanged?.Invoke();
        }
    }

    internal static TailscaleProfile[] ReadSystemProfiles() => mode == TailscaleMode.System && !transitioning
        ? Upstream.ReadSystemTailscaleProfiles() : Array.Empty<TailscaleProfile>();

    internal static async Task SwitchSystemProfileAsync(string profileId)
    {
        await Transition.WaitAsync();
        try
        {
            if (mode != TailscaleMode.System) throw new InvalidOperationException("请先选用系统 Tailscale。");
            var selected = (await Task.Run(Upstream.ReadSystemTailscaleProfiles)).SingleOrDefault(item => item.Id == profileId);
            if (selected == null) throw new InvalidOperationException("系统 Tailscale 已找不到该账号，请刷新账号列表。");
            if (selected.IsCurrent) return;
            BeginTransition();
            await Task.Run(() => Upstream.SwitchSystemTailscaleProfile(selected.Id));
        }
        finally { EndTransition(); }
    }

    private static void BeginTransition()
    {
        transitioning = true;
        Interlocked.Increment(ref generation);
        ContextChanging?.Invoke();
    }

    private static void EndTransition()
    {
        var refresh = transitioning;
        transitioning = false;
        Transition.Release();
        if (refresh) ContextChanged?.Invoke();
    }

    private static TailscaleMode ReadMode()
    {
        try
        {
            if (File.Exists(SettingsPath))
            {
                var saved = File.ReadAllText(SettingsPath).Trim();
                if (saved == nameof(TailscaleMode.System) && HasSystemRuntime) return TailscaleMode.System;
                if (saved == nameof(TailscaleMode.Embedded) && SupportsEmbedded) return TailscaleMode.Embedded;
            }
        }
        catch (Exception error) when (error is UnauthorizedAccessException or IOException)
        {
            Trace.TraceWarning("Tailscale runtime preference could not be read: {0}", error.Message);
        }

        return !HasSystemRuntime && SupportsEmbedded ? TailscaleMode.Embedded : TailscaleMode.System;
    }

    private static void SaveMode(TailscaleMode next)
    {
        var temp = SettingsPath + "." + Guid.NewGuid().ToString("N");
        Directory.CreateDirectory(Path.GetDirectoryName(SettingsPath));
        try
        {
            File.WriteAllText(temp, next.ToString());
            File.Move(temp, SettingsPath, overwrite: true);
        }
        finally { if (File.Exists(temp)) File.Delete(temp); }
    }
}
