using System.Diagnostics;
using System.Net;
using System.Net.Http.Json;
using System.Text.Json;
using System.Threading.Channels;
using Aurora;

try
{
    await RunAsync(args);
    return 0;
}
catch (Exception error)
{
    Console.Error.WriteLine(error);
    return 1;
}

static async Task RunAsync(string[] args)
{
    if (args is ["--verify-failure"])
        throw new InvalidOperationException("Expected isolated replay failure for exit-code and logging verification.");
    if (args.Length is not (0 or 2))
        throw new ArgumentException("Expected either no arguments or paths to AuroraQtShell and SunshineReplay.qml.");

    using var handler = new OfflineSunshine();
    using var admin = new SunshineAdmin(handler, new Uri("https://127.0.0.1:47990/"), managed: true);
    var (before, _, _) = await SunshineQtConfig.ReadAsync(admin);
    Require(before.Values["sunshine_name"] == "书房电脑", "read the current host name");
    Require(before.Options.Length == 96 && before.Options.Single(option => option.Key == "port").Managed,
        "read the existing catalog and fixed managed options");
    var save = await SunshineQtConfig.SaveAsync(admin, 1, new() { ["sunshine_name"] = "验收电脑" });
    var (after, _, _) = await SunshineQtConfig.ReadAsync(admin);
    Require(save.Saved.SequenceEqual(new[] { "sunshine_name" }) && save.Errors.Count == 0 &&
        after.Values["sunshine_name"] == "验收电脑" && handler.Config["unknown_option"].GetString() == "keep",
        "commit, read back, and retain unrelated keys");
    var refused = await SunshineQtConfig.SaveAsync(admin, 2, new() { ["port"] = "48001", ["min_threads"] = "bad", ["min_log_level"] = "3" });
    Require(refused.Errors.ContainsKey("port") && refused.Errors.ContainsKey("min_threads") &&
        refused.Saved.SequenceEqual(new[] { "min_log_level" }), "reject invalid and managed fields, save unrelated valid fields");
    handler.IgnoreNextWrite = true;
    var mismatch = await SunshineQtConfig.SaveAsync(admin, 3, new() { ["sunshine_name"] = "未提交" });
    Require(mismatch.Saved.Length == 0 && mismatch.Errors.ContainsKey("sunshine_name"),
        "reject a success response whose readback differs");
    var display = await SunshineQtConfig.SaveAsync(admin, 4, new() { ["displaySource"] = OfflineSunshine.VirtualId, ["exclusiveDisplay"] = "enabled" });
    var (displayState, _, _) = await SunshineQtConfig.ReadAsync(admin);
    Require(display.Errors.Count == 0 && displayState.Values["exclusiveDisplay"] == "enabled" &&
        displayState.Privacy.RestartNeeded, "verify saved versus active display state");
    var reset = await SunshineQtConfig.SaveAsync(admin, 5, new() { ["displaySource"] = "" });
    Require(reset.Errors.Count == 0 && !handler.Config.ContainsKey("aurora_privacy_display_id"),
        "restore the original display layout choice");
    Console.WriteLine("PASS: SunshineAdmin offline read, save/readback, invalid input, fixed options, mismatch and display state.");

    if (args.Length == 2)
        await RunQtAsync(args, admin, handler);
}

static async Task RunQtAsync(string[] args, SunshineAdmin admin, OfflineSunshine handler)
{
    if (Environment.GetEnvironmentVariable("QT_QPA_PLATFORM") != "offscreen")
        throw new InvalidOperationException("Run this replay with the isolated PowerShell runner.");
    using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(55));
    var requests = Channel.CreateUnbounded<QtShellRequest>();
    using var bridge = QtShellBridge.Start(args[0], args[1], request => requests.Writer.TryWrite(request));
    var shell = new QtShellState("signedOut", "", "", [], 0, false, false, "", false, false, true)
        { ManagedWindow = true };
    var host = new QtShellHostState(true, "running", true, true, true, false, true, false, false,
        "192.0.2.18:48989", "", false, true, "", "", false);
    var state = new QtSunshineState("idle", "", false, false, false, false, false, "", "", "", [], null, null);
    Publish();
    await Expect("sunshineLoad");
    var (config, _, _) = await SunshineQtConfig.ReadAsync(admin);
    state = state with { Status = "ready", Session = "offline-session", Config = config };
    Publish();

    await Expect("test-name-edited");
    var timer = Stopwatch.StartNew();
    var name = await Expect("sunshineSave");
    Require(timer.ElapsedMilliseconds >= 530, "text debounce before autosave");
    CheckChange(name, "sunshine_name", "Studio");
    state = state with { Busy = true };
    Publish();
    await Expect("test-name-edited-again");
    await Save(name);
    var again = await Expect("sunshineSave");
    CheckChange(again, "sunshine_name", "Studio2");
    await Save(again);

    var audio = await Expect("sunshineSave");
    CheckChange(audio, "stream_audio", "disabled");
    await Save(audio);

    var invalid = await Expect("sunshineSave");
    CheckChange(invalid, "min_threads", "bad");
    await Save(invalid);
    await Expect("test-kept-draft");
    var corrected = await Expect("sunshineSave");
    CheckChange(corrected, "min_threads", "4");
    await Save(corrected);

    await Expect("test-restart-cancelled");
    var restart = await Expect("sunshineRestart");
    Require(restart.Sunshine.Session == state.Session, "restart carries the active Sunshine session");
    await admin.Restart();
    Require(handler.RestartCount == 1, "one restart request after confirmation");
    await Expect("test-finished");
    await Expect("window-close");
    bridge.Dispose();
    await bridge.Completion.WaitAsync(timeout.Token);
    Console.WriteLine("PASS: Qt single-window Sunshine read/edit/autosave/readback, in-flight edit, audio, 320px advanced search, validation, discard cancel and restart confirmation.");

    void Publish() => bridge.Publish(shell with { Host = host with { Sunshine = state } });

    async Task<QtShellRequest> Expect(string action)
    {
        while (true)
        {
            var next = requests.Reader.ReadAsync(timeout.Token).AsTask();
            if (await Task.WhenAny(next, bridge.Completion) == bridge.Completion && !next.IsCompleted)
                throw new InvalidOperationException($"Qt closed before sending {action}.");
            var request = await next;
            if (request.Action is "devices-foreground" or "devices-background" or "avatar-visible" or "avatar-hidden") continue;
            if (request.Action != action)
                throw new InvalidOperationException($"Expected {action}, received {request.Action}. {request.DeviceId}");
            return request;
        }
    }

    void CheckChange(QtShellRequest request, string key, string value) =>
        Require(request.Sunshine?.Session == state.Session && request.Sunshine.Changes.Count == 1 &&
            request.Sunshine.Changes.GetValueOrDefault(key) == value, $"submit only {key} = {value}");

    async Task Save(QtShellRequest request)
    {
        var input = request.Sunshine;
        var result = await SunshineQtConfig.SaveAsync(admin, input.Id, input.Changes);
        var (config, _, _) = await SunshineQtConfig.ReadAsync(admin);
        state = state with
        {
            Busy = false, Config = config, Result = result,
            Feedback = result.Errors.Count == 0 ? "已保存。重启 Sunshine 后生效。" : ""
        };
        Publish();
    }
}

static void Require(bool passed, string description)
{
    if (!passed) throw new InvalidOperationException(description);
}

sealed class OfflineSunshine : HttpMessageHandler
{
    internal const string VirtualId = "{bdd30211-073b-47ff-90e9-9817b5a59ac0}";
    internal Dictionary<string, JsonElement> Config { get; private set; } =
        JsonSerializer.Deserialize<Dictionary<string, JsonElement>>(
            """{"sunshine_name":"书房电脑","stream_audio":"enabled","unknown_option":"keep"}""");
    internal bool IgnoreNextWrite { get; set; }
    internal int RestartCount { get; private set; }

    protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
    {
        switch (request.RequestUri.AbsolutePath)
        {
            case "/api/config":
                if (request.Method == HttpMethod.Post)
                {
                    var posted = await request.Content.ReadFromJsonAsync<Dictionary<string, JsonElement>>(cancellationToken);
                    if (!IgnoreNextWrite) Config = posted;
                    IgnoreNextWrite = false;
                }
                return Reply(new Dictionary<string, JsonElement>(Config) { ["status"] = JsonSerializer.SerializeToElement(true) });
            case "/api/aurora/privacy":
                var saved = Config.GetValueOrDefault("aurora_privacy_display_id").ValueKind != JsonValueKind.Undefined;
                return Reply(new
                {
                    status = true, supports_virtual_display = true, supports_screensaver = false,
                    saved = saved || Config.ContainsKey("aurora_virtual_display_id") ? VirtualId : "",
                    active = "", saved_exclusive = saved, active_exclusive = false,
                    displays = new[] { new { id = VirtualId, name = "离线虚拟显示器" } }
                });
            case "/api/restart" when request.Method == HttpMethod.Post:
                RestartCount++;
                return Reply(new { status = true });
            default:
                throw new InvalidOperationException($"Unexpected offline path: {request.RequestUri.AbsolutePath}");
        }
    }

    private static HttpResponseMessage Reply<T>(T payload) => new(HttpStatusCode.OK) { Content = JsonContent.Create(payload) };
}
