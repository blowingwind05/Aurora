using System;
using System.Text.Json;

namespace Aurora;

internal enum TailscaleState { NotInstalled, Starting, Stopped, NeedsLogin, NeedsApproval, Connected, Unknown }

internal sealed record TailscaleStatus(TailscaleState State, Uri LoginUri = null, string TailnetName = null)
{
    public static TailscaleStatus Parse(string json)
    {
        using var document = JsonDocument.Parse(json);
        var root = document.RootElement;
        if (root.ValueKind != JsonValueKind.Object) return new(TailscaleState.Unknown);
        var state = ReadString(root, "BackendState") switch
        {
            "Running" => TailscaleState.Connected,
            "NeedsLogin" => TailscaleState.NeedsLogin,
            "NeedsMachineAuth" => TailscaleState.NeedsApproval,
            "Stopped" => TailscaleState.Stopped,
            "Starting" or "NoState" => TailscaleState.Starting,
            _ => TailscaleState.Unknown
        };
        var url = state == TailscaleState.NeedsLogin ? ReadString(root, "AuthURL") : null;
        Uri login = null;
        if (Uri.TryCreate(url, UriKind.Absolute, out var candidate) && candidate.Scheme == Uri.UriSchemeHttps &&
            candidate.Host == "login.tailscale.com" && candidate.IsDefaultPort && candidate.UserInfo.Length == 0 &&
            candidate.AbsolutePath.StartsWith("/a/", StringComparison.Ordinal))
            login = candidate;
        var name = root.TryGetProperty("CurrentTailnet", out var tailnet) ? ReadString(tailnet, "Name") : null;
        return new(state, login, name);
    }

    private static string ReadString(JsonElement root, string key) =>
        root.ValueKind == JsonValueKind.Object && root.TryGetProperty(key, out var value) && value.ValueKind == JsonValueKind.String
            ? value.GetString() : null;
}
