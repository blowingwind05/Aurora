using System;
using System.Globalization;
using System.Text.Json;

namespace Aurora;

internal enum TailscaleState { NotInstalled, Starting, Stopped, NeedsLogin, NeedsApproval, Connected, Unknown }

internal sealed record TailscaleStatus(
    TailscaleState State, Uri LoginUri = null, string TailnetName = null,
    string DisplayName = null, string LoginName = null, long? OwnerId = null, Uri AvatarUri = null)
{
    public string AccountInitial
    {
        get
        {
            var name = DisplayName ?? LoginName;
            return string.IsNullOrEmpty(name) ? null : StringInfo.GetNextTextElement(name).ToUpperInvariant();
        }
    }

    public static TailscaleStatus Parse(string json)
    {
        using var document = JsonDocument.Parse(json);
        var root = document.RootElement;
        if (root.ValueKind != JsonValueKind.Object) return new(TailscaleState.Unknown);
        root.TryGetProperty("Self", out var self);
        long? ownerId = self.ValueKind == JsonValueKind.Object &&
            self.TryGetProperty("UserID", out var userId) && userId.ValueKind == JsonValueKind.Number &&
            userId.TryGetInt64(out var id) && id > 0 ? id : null;
        var state = ReadString(root, "BackendState") switch
        {
            "Running" => TailscaleState.Connected,
            "NeedsLogin" => TailscaleState.NeedsLogin,
            "NeedsMachineAuth" => TailscaleState.NeedsApproval,
            "Stopped" => TailscaleState.Stopped,
            "Starting" or "NoState" => TailscaleState.Starting,
            _ => TailscaleState.Unknown
        };

        if (state == TailscaleState.NeedsLogin)
        {
            var url = ReadString(root, "AuthURL");
            var login = Uri.TryCreate(url, UriKind.Absolute, out var candidate) && candidate.Scheme == Uri.UriSchemeHttps &&
                candidate.Host == "login.tailscale.com" && candidate.IsDefaultPort && candidate.UserInfo.Length == 0 &&
                candidate.AbsolutePath.StartsWith("/a/", StringComparison.Ordinal) ? candidate : null;
            return new(state, LoginUri: login, OwnerId: ownerId);
        }
        if (state != TailscaleState.Connected) return new(state, OwnerId: ownerId);

        var tailnet = root.TryGetProperty("CurrentTailnet", out var network) ? ReadString(network, "Name") : null;
        if (ownerId is not long owner ||
            (self.TryGetProperty("Tags", out var tags) && tags.ValueKind == JsonValueKind.Array && tags.GetArrayLength() > 0) ||
            !root.TryGetProperty("User", out var users) || users.ValueKind != JsonValueKind.Object ||
            !users.TryGetProperty(owner.ToString(CultureInfo.InvariantCulture), out var profile))
            return new(state, TailnetName: tailnet, OwnerId: ownerId);

        return new(state, TailnetName: tailnet, DisplayName: ReadString(profile, "DisplayName"),
            LoginName: ReadString(profile, "LoginName"), OwnerId: ownerId,
            AvatarUri: TailscaleAvatar.VettedUri(ReadString(profile, "ProfilePicURL")));
    }

    private static string ReadString(JsonElement root, string key)
    {
        if (root.ValueKind != JsonValueKind.Object || !root.TryGetProperty(key, out var value) ||
            value.ValueKind != JsonValueKind.String) return null;
        var text = value.GetString();
        return string.IsNullOrWhiteSpace(text) ? null : text.Trim();
    }
}
