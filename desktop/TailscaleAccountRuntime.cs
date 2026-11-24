using System;
using System.Diagnostics;
using System.Threading.Tasks;

namespace Aurora;

internal interface ITailscaleAccountRuntime
{
    TailscaleMode Mode { get; }
    bool IsTransitioning { get; }
    int Generation { get; }
    bool SupportsEmbedded { get; }
    bool HasSystemRuntime { get; }
    event Action ContextChanged;
    Task<TailscaleStatus> ReadStatus();
    Task<string> ReadAddress();
    Task<TailscaleProfile[]> ReadProfiles();
    Task SwitchMode(TailscaleMode mode);
    Task SwitchProfile(string id);
    Task RequestConnection();
    Task Logout();
    void OpenLogin(Uri uri);
}

internal sealed class TailscaleAccountRuntime : ITailscaleAccountRuntime
{
    public TailscaleMode Mode => TailscaleRuntime.Mode;
    public bool IsTransitioning => TailscaleRuntime.IsTransitioning;
    public int Generation => TailscaleRuntime.Generation;
    public bool SupportsEmbedded => TailscaleRuntime.SupportsEmbedded;
    public bool HasSystemRuntime => TailscaleRuntime.HasSystemRuntime;
    public event Action ContextChanged
    {
        add
        {
            TailscaleRuntime.ContextChanging += value;
            TailscaleRuntime.ContextChanged += value;
            TailscaleRuntime.ModeChanged += value;
        }
        remove
        {
            TailscaleRuntime.ContextChanging -= value;
            TailscaleRuntime.ContextChanged -= value;
            TailscaleRuntime.ModeChanged -= value;
        }
    }

    public Task<TailscaleStatus> ReadStatus() => Task.Run(Upstream.ReadTailscale);
    public Task<string> ReadAddress() => Task.Run(() => Upstream.ReadNetwork().LocalAddress?.ToString());
    public Task<TailscaleProfile[]> ReadProfiles() => Task.Run(TailscaleRuntime.ReadSystemProfiles);
    public Task SwitchMode(TailscaleMode mode) => TailscaleRuntime.SwitchAsync(mode);
    public Task SwitchProfile(string id) => TailscaleRuntime.SwitchSystemProfileAsync(id);
    public Task RequestConnection() => Task.Run(Upstream.RequestTailscaleConnection);
    public Task Logout() => Task.Run(Upstream.LogoutTailscale);

    public void OpenLogin(Uri uri)
    {
        using var browser = Process.Start(new ProcessStartInfo(uri.AbsoluteUri) { UseShellExecute = true });
    }
}
