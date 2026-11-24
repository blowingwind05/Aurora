using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Win32;

namespace Aurora;

internal enum SunshineAccess { Setup, Login, SignedIn }
internal sealed record SunshinePairing(string Id, string Name, string Address)
{
    internal int? SourcePort { get; init; }

    public override string ToString() => $"{Name} · {Address}";
}
internal sealed record SunshinePreferences(string Name, bool StreamAudio);
internal sealed record SunshinePrivacyDisplay(string Id, string Name)
{
    public override string ToString() => Name;
}
internal sealed record SunshineDisplaySelection(string Id, bool Exclusive, bool Screensaver);
internal sealed record SunshinePrivacySettings(SunshineDisplaySelection Saved, SunshineDisplaySelection Active,
    bool SupportsVirtualDisplay, bool SupportsExtended, bool SupportsScreensaver, SunshinePrivacyDisplay[] Displays);

internal sealed class SunshineLoginRejectedException()
    : InvalidOperationException("无法登录 Sunshine，请检查账号和密码。");
internal sealed class SunshineTlsException()
    : InvalidOperationException("无法验证本机 Sunshine 的证书。若证书已更换，请重新导入公开证书指纹。");

internal sealed class SunshineAdmin : IDisposable
{
    private readonly HttpClient http;
    private AuthenticationHeaderValue authorization;
    public string SettingsUrl => http.BaseAddress.AbsoluteUri;
    public string CredentialScope { get; private init; }
    internal bool IsManaged { get; }

    internal SunshineAdmin(HttpMessageHandler handler, Uri endpoint, bool managed = false)
    {
        if (!endpoint.IsLoopback || endpoint.Scheme != Uri.UriSchemeHttps)
            throw new ArgumentException("Sunshine 管理只允许本机 HTTPS。", nameof(endpoint));
        IsManaged = managed;
        http = new HttpClient(handler)
        {
            BaseAddress = endpoint,
            Timeout = TimeSpan.FromSeconds(5),
            MaxResponseContentBufferSize = 256 * 1024
        };
    }

    public static SunshineAdmin OpenLocal()
        => OpenLocal(null);

    public static SunshineAdmin OpenLocal(byte[] importedPin) => Open(bundled: false, importedPin);

    internal static SunshineAdmin OpenBundled()
    {
        var password = BundledSunshine.ReadAdminPassword();
        var admin = Open(bundled: true, importedPin: null);
        admin.authorization = BasicAuthorization(BundledSunshine.AdminUsername, password);
        return admin;
    }

    private static SunshineAdmin Open(bool bundled, byte[] importedPin)
    {
        var configPath = bundled ? BundledSunshine.ConfigPath : OperatingSystem.IsWindows()
            ? InstalledConfiguration() : throw new PlatformNotSupportedException();
        var directory = Path.GetDirectoryName(configPath);
        var options = !bundled && File.Exists(configPath) ? File.ReadLines(configPath)
            .Select(line => line.Split('=', 2))
            .Where(parts => parts.Length == 2 && parts[0].Trim() is "cert" or "port")
            .GroupBy(parts => parts[0].Trim())
            .ToDictionary(group => group.Key, group => group.First()[1].Split('#', 2)[0].Trim()) : new();
        var defaultCertificatePath = Path.Combine(directory, "credentials", "cacert.pem");
        var certificatePath = bundled ? BundledSunshine.CertificatePath
            : options.GetValueOrDefault("cert", Path.Combine("credentials", "cacert.pem"));
        if (string.IsNullOrWhiteSpace(certificatePath)) certificatePath = Path.Combine("credentials", "cacert.pem");
        certificatePath = Path.GetFullPath(certificatePath, directory);
        if (certificatePath.StartsWith(@"\\", StringComparison.Ordinal))
            throw new InvalidOperationException("Sunshine 管理只使用本机的公开证书。");
        var port = 47989;
        if (!bundled && options.TryGetValue("port", out var configured) &&
            (!int.TryParse(configured, out port) || port is < 1 or > 65534))
            throw new InvalidOperationException("Sunshine 的管理端口配置无法识别。");
        var endpoint = bundled ? BundledSunshine.AdminEndpoint : new Uri($"https://127.0.0.1:{port + 1}/");
        var scope = OperatingSystem.IsWindows() && !bundled ? SunshineTrust.Scope(certificatePath, endpoint.Port) : null;
        byte[] expected;
        var supportsImport = !bundled && string.Equals(certificatePath, defaultCertificatePath, StringComparison.OrdinalIgnoreCase);
        if (importedPin != null)
        {
            if (!supportsImport || importedPin.Length != 32) throw new SunshineCertificateAccessException();
            expected = importedPin;
        }
        else try
        {
            using var certificate = X509Certificate2.CreateFromPem(File.ReadAllText(certificatePath));
            expected = certificate.GetCertHash(HashAlgorithmName.SHA256);
        }
        catch (UnauthorizedAccessException)
        {
            if (bundled) throw new InvalidOperationException("Aurora 无法读取内置 Sunshine 的公开证书。");
            if (!OperatingSystem.IsWindows() || !supportsImport)
                throw new InvalidOperationException("当前账号无法读取自定义的 Sunshine 公开证书。请让管理员只开放该公开证书的读取权限。");
            expected = SunshineTrust.ReadPin(scope) ?? throw new SunshineCertificateAccessException();
        }
        catch (IOException)
        {
            throw new InvalidOperationException("未找到可读取的 Sunshine 本机证书。请先启动 Sunshine，或使用它的设置页。");
        }
        var handler = new HttpClientHandler
        {
            AllowAutoRedirect = false,
            UseProxy = false,
            UseCookies = false,
            ServerCertificateCustomValidationCallback = (request, presented, _, _) =>
                MatchesLocalCertificate(endpoint, request.RequestUri, expected, presented)
        };
        return new SunshineAdmin(handler, endpoint, bundled)
        {
            CredentialScope = $"{endpoint.Port}/{Convert.ToHexString(expected)}",
            TrustScope = scope,
            SupportsCertificateImport = supportsImport,
            VerifiedCertificateHash = expected
        };
    }

    [System.Runtime.Versioning.SupportedOSPlatform("windows")]
    private static string InstalledConfiguration()
    {
        using var service = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Services\SunshineService");
        var command = service?.GetValue("ImagePath") as string ?? "";
        var match = Regex.Match(command, "^(?:\"([^\"]+)\"|([^\"\\r\\n]+?\\.exe))(?:\\s|$)", RegexOptions.IgnoreCase);
        var executable = match.Success ? (match.Groups[1].Success ? match.Groups[1].Value : match.Groups[2].Value) : null;
        if (executable != null) executable = Environment.ExpandEnvironmentVariables(executable);
        if (executable == null || !Path.IsPathFullyQualified(executable) || executable.StartsWith(@"\\", StringComparison.Ordinal) ||
            !string.Equals(Path.GetFileName(executable), "sunshinesvc.exe", StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("没有找到可验证的 Windows Sunshine 服务安装。");
        var directory = Path.Combine(Directory.GetParent(Path.GetDirectoryName(executable)).FullName, "config");
        return Path.Combine(directory, "sunshine.conf");
    }

    internal static bool MatchesLocalCertificate(Uri endpoint, Uri request, byte[] expected, X509Certificate2 presented) =>
        request?.IsLoopback == true && request.Scheme == Uri.UriSchemeHttps &&
        request.Host == endpoint.Host && request.Port == endpoint.Port && presented != null &&
        CryptographicOperations.FixedTimeEquals(expected, presented.GetCertHash(HashAlgorithmName.SHA256));

    private string TrustScope { get; init; }
    private byte[] VerifiedCertificateHash { get; init; }
    public bool SupportsCertificateImport { get; private init; }
    public void RememberVerifiedCertificate()
    {
        if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException();
        SunshineTrust.SavePin(TrustScope, VerifiedCertificateHash);
    }

    public async Task<SunshineAccess> CheckAccess()
    {
        using var response = await Send(HttpMethod.Get, "api/config");
        if (IsWelcome(response)) return SunshineAccess.Setup;
        if (response.StatusCode == HttpStatusCode.Unauthorized) return SunshineAccess.Login;
        response.EnsureSuccessStatusCode();
        await ReadSuccess(response);
        return SunshineAccess.SignedIn;
    }

    private static bool IsWelcome(HttpResponseMessage response) =>
        response.StatusCode == HttpStatusCode.TemporaryRedirect && response.Headers.Location?.OriginalString == "/welcome";

    public async Task Login(string username, string password)
    {
        ValidateAccount(username, password);
        authorization = BasicAuthorization(username, password);
        try
        {
            if (await CheckAccess() != SunshineAccess.SignedIn)
                throw new SunshineLoginRejectedException();
        }
        catch
        {
            authorization = null;
            throw;
        }
    }

    private static AuthenticationHeaderValue BasicAuthorization(string username, string password) =>
        new("Basic", Convert.ToBase64String(Encoding.UTF8.GetBytes(username + ":" + password)));

    public async Task CreateAccount(string username, string password, string confirmation)
    {
        ValidateAccount(username, password);
        if (password != confirmation) throw new InvalidOperationException("两次输入的密码不一致。");
        if (await CheckAccess() != SunshineAccess.Setup)
            throw new InvalidOperationException("Sunshine 已有管理账号，请重新打开窗口并登录。");
        await Post("api/password", new { newUsername = username, newPassword = password, confirmNewPassword = confirmation });
        await Login(username, password);
    }

    internal static void ValidateAccount(string username, string password)
    {
        if (string.IsNullOrWhiteSpace(username) || username.Contains(':') || username.Any(char.IsControl))
            throw new InvalidOperationException("请输入有效的用户名；用户名不能含冒号或控制字符。");
        if (string.IsNullOrEmpty(password) || password.Any(char.IsControl))
            throw new InvalidOperationException("请输入密码，密码不能含控制字符。");
    }

    public async Task<SunshinePreferences> ReadPreferences() => ParsePreferences(await ReadOptions());

    internal static SunshinePreferences ParsePreferences(JsonElement root)
    {
        var enabled = !root.TryGetProperty("stream_audio", out var audio) ||
            audio.ToString().ToLowerInvariant() is "on" or "yes" or "true" or "enable" or "enabled" ||
            audio.ToString().Contains('1');
        return new SunshinePreferences(ReadText(root, "sunshine_name"), enabled);
    }

    public async Task<JsonElement> ReadOptions()
    {
        using var response = await Send(HttpMethod.Get, "api/config");
        return await ReadSuccess(response);
    }

    public async Task SaveOptions(IReadOnlyDictionary<string, string> changes)
    {
        if (changes.Count == 0) return;
        var options = SunshineOptionsPage.ReadCatalog().ToDictionary(option => option.Key);
        foreach (var change in changes)
        {
            if (!options.TryGetValue(change.Key, out var option))
                throw new InvalidOperationException("不支持的设置：" + change.Key);
            SunshineOptionsPage.Validate(option, change.Value);
        }
        var payload = MergeOptions(await ReadOptions(), changes);
        await Post("api/config", payload);
        var actual = await ReadOptions();
        if (changes.Any(change => change.Value.Length == 0
                ? actual.TryGetProperty(change.Key, out _)
                : !actual.TryGetProperty(change.Key, out var value) || value.ToString().Trim() != change.Value.Trim()))
            throw new InvalidOperationException("回读结果与提交值不同，请检查当前 Sunshine 设置。");
    }

    internal async Task<SunshinePrivacySettings> ReadPrivacySettings()
    {
        if (!IsManaged) return null;
        using var response = await Send(HttpMethod.Get, "api/aurora/privacy");
        if (response.StatusCode == HttpStatusCode.NotFound) return null;
        return ParsePrivacySettings(await ReadSuccess(response));
    }

    internal static SunshinePrivacySettings ParsePrivacySettings(JsonElement root)
    {
        var hasDisplays = root.TryGetProperty("displays", out var list) && list.ValueKind == JsonValueKind.Array;
        var supportsVirtualDisplay = root.TryGetProperty("supports_virtual_display", out var virtualDisplay)
            ? virtualDisplay.ValueKind == JsonValueKind.True : hasDisplays;
        var displays = supportsVirtualDisplay && hasDisplays
            ? list.EnumerateArray().Select(item => new SunshinePrivacyDisplay(ReadText(item, "id"), ReadText(item, "name")))
                .Where(display => Guid.TryParseExact(display.Id, "B", out _)).ToArray()
            : Array.Empty<SunshinePrivacyDisplay>();
        var supportsExtended = supportsVirtualDisplay && root.TryGetProperty("saved_exclusive", out _);
        var savedId = ReadText(root, "saved");
        var activeId = ReadText(root, "active");
        var supportsScreensaver = ReadBoolean(root, "supports_screensaver");
        return new(
            new(savedId, savedId.Length != 0 && (!supportsExtended || ReadBoolean(root, "saved_exclusive")),
                ReadBoolean(root, "saved_screensaver")),
            new(activeId, activeId.Length != 0 && (!supportsExtended ||
                ReadBoolean(root, "active_exclusive")), ReadBoolean(root, "active_screensaver")),
            supportsVirtualDisplay, supportsExtended, supportsScreensaver, displays);
    }

    public async Task SavePreferences(string name, bool streamAudio, SunshineDisplaySelection display = null)
    {
        ValidateName(name, allowEmpty: true);
        using var response = await Send(HttpMethod.Get, "api/config");
        var current = await ReadSuccess(response);
        var payload = MergePreferences(current, name, streamAudio);
        if (display != null)
        {
            var privacy = await ReadPrivacySettings() ??
                throw new InvalidOperationException("请先更新内置被控组件。");
            if (display.Id != privacy.Saved.Id && display.Id.Length != 0 &&
                !privacy.Displays.Any(candidate => candidate.Id == display.Id))
                throw new InvalidOperationException("所选虚拟显示器已不可用，请重新打开设置。");
            if (display.Id.Length != 0 && !display.Exclusive && !privacy.SupportsExtended)
                throw new InvalidOperationException("请更新被控组件以保留本地显示器。");
            if (display.Screensaver && (display.Exclusive || !privacy.SupportsScreensaver && !privacy.Saved.Screensaver))
                throw new InvalidOperationException("当前被控组件或显示设置无法启用隐私屏保。");
            if (privacy.SupportsVirtualDisplay)
            {
                payload.Remove("aurora_privacy_display_id");
                payload.Remove("aurora_virtual_display_id");
                if (display.Id.Length != 0)
                    payload[display.Exclusive ? "aurora_privacy_display_id" : "aurora_virtual_display_id"] = display.Id;
            }
            if (privacy.SupportsScreensaver || privacy.Saved.Screensaver)
            {
                payload.Remove("aurora_privacy_screensaver");
                if (display.Screensaver) payload["aurora_privacy_screensaver"] = "enabled";
            }
        }
        await Post("api/config", payload);
        var saved = await ReadPreferences();
        if (saved.Name != name || saved.StreamAudio != streamAudio)
            throw new InvalidOperationException("Sunshine 没有返回刚保存的值，请重新读取设置。");
        if (display != null && (await ReadPrivacySettings())?.Saved != display)
            throw new InvalidOperationException("无法确认已保存的显示设置，请重新读取设置。");
    }

    internal static Dictionary<string, object> MergePreferences(JsonElement current, string name, bool streamAudio)
        => MergeOptions(current, new Dictionary<string, string>
        {
            ["sunshine_name"] = name, ["stream_audio"] = streamAudio ? "enabled" : "disabled"
        });

    internal static Dictionary<string, object> MergeOptions(JsonElement current, IReadOnlyDictionary<string, string> changes)
    {
        var payload = current.EnumerateObject()
            .Where(property => property.Name is not ("status" or "version" or "platform"))
            .ToDictionary(property => property.Name, property => (object)property.Value.Clone());
        foreach (var change in changes)
        {
            if (change.Value.Length == 0) payload.Remove(change.Key);
            else payload[change.Key] = change.Value;
        }
        return payload;
    }

    public Task<SunshinePairing[]> Pending() => Pending(CancellationToken.None);

    internal async Task<SunshinePairing[]> Pending(CancellationToken token)
    {
        using var response = await Send(HttpMethod.Get, "api/pin", token: token);
        response.EnsureSuccessStatusCode();
        var root = await ReadJson(response);
        if (!root.TryGetProperty("pairings", out var items) || items.ValueKind != JsonValueKind.Array)
            throw new InvalidOperationException("当前 Sunshine 不支持在 Aurora 内选择待确认的配对。");
        return items.EnumerateArray().Select(ParsePendingPairing).Where(item => item != null).ToArray();
    }

    internal static SunshinePairing ParsePendingPairing(JsonElement item)
    {
        if (item.ValueKind != JsonValueKind.Object) return null;
        var id = ReadText(item, "id");
        if (!Regex.IsMatch(id, @"\A[0-9a-fA-F]{32}\z")) return null;
        int? sourcePort = item.TryGetProperty("sourcePort", out var value) && value.ValueKind == JsonValueKind.Number &&
            value.TryGetInt32(out var port) && port is > 0 and <= IPEndPoint.MaxPort ? port : null;
        return new(id, ReadText(item, "name"), ReadText(item, "address")) { SourcePort = sourcePort };
    }

    public Task Pair(SunshinePairing pairing, string pin, string name) => Pair(pairing, pin, name, CancellationToken.None);

    internal Task Pair(SunshinePairing pairing, string pin, string name, CancellationToken token)
    {
        if (pairing == null || !Regex.IsMatch(pairing.Id, @"\A[0-9a-fA-F]{32}\z"))
            throw new InvalidOperationException("请选择一条待确认的配对请求。");
        if (!Regex.IsMatch(pin ?? "", @"\A[0-9]{4}\z"))
            throw new InvalidOperationException("请输入主控端显示的 4 位数字 PIN。");
        ValidateName(name, allowEmpty: false);
        return Post("api/pin", new { pairing_id = pairing.Id, pin, name }, token);
    }

    public async Task Restart()
    {
        if (await CheckAccess() != SunshineAccess.SignedIn)
            throw new InvalidOperationException("请先登录 Sunshine。");
        using var request = new HttpRequestMessage(HttpMethod.Post, "api/restart");
        request.Headers.Authorization = authorization;
        try
        {
            using var response = await http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead);
            response.EnsureSuccessStatusCode();
        }
        catch (HttpRequestException error) when (error.HttpRequestError == HttpRequestError.ResponseEnded) { }
    }

    internal static void ValidateName(string name, bool allowEmpty)
    {
        if ((!allowEmpty && string.IsNullOrWhiteSpace(name)) || name.Any(char.IsControl) ||
            name.Contains('#') || Encoding.UTF8.GetByteCount(name) > 128)
            throw new InvalidOperationException("名称最多 128 字节，不能含 # 或控制字符。");
    }

    private async Task Post(string path, object body, CancellationToken token = default)
    {
        using var response = await Send(HttpMethod.Post, path, body, token);
        await ReadSuccess(response);
    }

    private async Task<HttpResponseMessage> Send(HttpMethod method, string path, object body = null, CancellationToken token = default)
    {
        using var request = new HttpRequestMessage(method, path);
        request.Headers.Authorization = authorization;
        if (body != null) request.Content = JsonContent.Create(body);
        try { return await http.SendAsync(request, token); }
        catch (HttpRequestException error) when (error.HttpRequestError == HttpRequestError.SecureConnectionError)
        {
            throw new SunshineTlsException();
        }
        catch (HttpRequestException)
        {
            throw new InvalidOperationException("无法验证或连接本机 Sunshine。请确认服务已启动且使用安装目录里的证书。");
        }
        catch (TaskCanceledException) when (!token.IsCancellationRequested)
        {
            throw new InvalidOperationException("Sunshine 没有及时响应，请检查本机服务后重试。");
        }
    }

    private static async Task<JsonElement> ReadSuccess(HttpResponseMessage response)
    {
        if (response.StatusCode == HttpStatusCode.Unauthorized)
            throw new InvalidOperationException("Sunshine 账号或密码不正确，请重新打开管理窗口登录。");
        if (!response.IsSuccessStatusCode)
            throw new InvalidOperationException($"Sunshine 拒绝请求（HTTP {(int)response.StatusCode}）。");
        var root = await ReadJson(response);
        if (!root.TryGetProperty("status", out var status) || status.ValueKind != JsonValueKind.True)
            throw new InvalidOperationException("Sunshine 未接受这项操作。若是配对，请重新发起并核对 PIN。");
        return root;
    }

    private static async Task<JsonElement> ReadJson(HttpResponseMessage response)
    {
        using var json = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
        return json.RootElement.Clone();
    }

    private static string ReadText(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "";

    private static bool ReadBoolean(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.True;

    public void Dispose()
    {
        authorization = null;
        http.Dispose();
    }
}
