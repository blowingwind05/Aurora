using System.Threading.Channels;
using Aurora;

try
{
    await RunReplay(args);
    return 0;
}
catch (Exception error)
{
    var message = $"[{DateTime.UtcNow:O}] Tailscale Qt replay failed.{Environment.NewLine}{error}";
    var log = Path.Combine(AppContext.BaseDirectory, "tailscale-replay-error.log");
    try { File.AppendAllText(log, message + Environment.NewLine); }
    catch (Exception logError) when (logError is IOException or UnauthorizedAccessException)
    {
        Console.Error.WriteLine($"Unable to write the replay log: {logError.Message}");
    }
    Console.Error.WriteLine(message);
    return 1;
}

static async Task RunReplay(string[] args)
{
    if (args.Length != 2) throw new ArgumentException("Expected the Qt executable and Replay.qml paths.");
    using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(25));
    var requests = Channel.CreateUnbounded<QtShellRequest>();
    var devices = new QtShellState("signedOut", "", "未登录", [], 7, false, false, "", false, false, true);
    using var bridge = QtShellBridge.Start(args[0], args[1], request => requests.Writer.TryWrite(request));
    try
    {
        var checking = new QtShellTailscaleAccount("embedded", "内置 Tailscale", true, false, null, "Checking",
            "正在检查", "正在读取当前运行方式的连接状态。", "Tailscale", null, null, null, null, "", false, false, false)
            { Revision = 17 };
        var login = checking with { State = "NeedsLogin", Status = "未登录", PrimaryAction = "登录 Tailscale",
            Detail = "登录后可发现同一 Tailscale 网络中的设备。", Revision = 18, CanOpenPrimaryAction = true,
            RuntimeNotice = "尚未安装系统 Tailscale。Aurora 使用内置 Tailscale。", RefreshAvailable = true };
        var stopped = login with { Mode = "system", ModeLabel = "系统 Tailscale", HasSystemRuntime = true,
            RuntimeNotice = null, State = "Stopped", Status = "已暂停", Detail = "恢复异地连接并保留现有设置。",
            PrimaryAction = "恢复连接", Revision = 19 };
        var embedded = stopped with { Mode = "embedded", ModeLabel = "内置 Tailscale", State = "Connected",
            Status = "已连接", Detail = "", DisplayName = "联调账号", LoginName = "fixture@example.com", Initial = "联",
            TailnetName = "aurora-fixture", LocalAddress = "100.100.0.18", PrimaryAction = "", CanOpenPrimaryAction = false, Revision = 20 };
        var profiles = new[]
        {
            new QtShellTailscaleProfile("0", "联调账号", "fixture@example.com", "aurora-fixture", true),
            new QtShellTailscaleProfile("1", "另一个账号", "fixture.alternative@example.com", "aurora-alt", false)
        };
        var system = embedded with { Mode = "system", ModeLabel = "系统 Tailscale", Profiles = profiles, Revision = 21 };
        var promptMode = new QtShellTailscaleConfirmation(41, "切换到系统 Tailscale？",
            "Aurora 将通过系统 Tailscale 当前账号所在的网络发现和连接设备。", "切换", false);
        var promptProfile = new QtShellTailscaleConfirmation(43, "切换系统 Tailscale 账号？", "切换可能会断开正在进行的远控会话。", "切换", false);
        var promptLogout = new QtShellTailscaleConfirmation(45, "要退出登录吗？", "退出后本机会断开 Tailscale 网络。", "退出登录", true);

        Publish(checking);
        await Expect("tailscale.enter");
        Publish(login);
        await Expect("tailscale.refresh", revision: 18);
        Publish(login with { Busy = true, Refreshing = true, RefreshAvailable = false, CanOpenPrimaryAction = false });
        await Expect("test.refreshing");
        Publish(login);
        await Expect("tailscale.primary", revision: 18);
        Publish(stopped);
        await Expect("tailscale.primary", revision: 19);
        Publish(embedded);
        await Expect("tailscale.mode", "system", 20);
        Publish(embedded with { Busy = true, Confirmation = promptMode });
        await Expect("tailscale.cancel", "41");
        Publish(embedded);
        await Expect("tailscale.mode", "system", 20);
        Publish(embedded with { Busy = true, Confirmation = promptMode with { Id = 42 } });
        await Expect("tailscale.confirm", "42");
        Publish(embedded with { Busy = true, RuntimeProgress = true, Progress = "正在切换到系统 Tailscale…" });
        await Expect("test.switching");
        Publish(system with { Notice = "已切换至系统 Tailscale", NoticeVersion = 1 });
        await Expect("tailscale.profile", "1", 21);
        Publish(system with { Busy = true, Confirmation = promptProfile });
        await Expect("tailscale.confirm", "43");
        Publish(system with { Feedback = "无法切换账号。", RetryAvailable = true });
        await Expect("tailscale.retry", revision: 21);
        Publish(system with { Busy = true, Confirmation = promptProfile with { Id = 44 } });
        await Expect("tailscale.confirm", "44");
        var alternative = system with { Revision = 22, DisplayName = "另一个账号", LoginName = "fixture.alternative@example.com",
            Profiles = [profiles[0] with { Current = false }, profiles[1] with { Current = true }] };
        Publish(alternative);
        await Expect("tailscale.logout", revision: 22);
        Publish(alternative with { Busy = true, Confirmation = promptLogout });
        await Expect("tailscale.cancel", "45");
        Publish(alternative);
        await Expect("tailscale.logout", revision: 22);
        Publish(alternative with { Busy = true, Confirmation = promptLogout with { Id = 46 } });
        await Expect("tailscale.confirm", "46");
        Publish(login with { Mode = "system", ModeLabel = "系统 Tailscale", HasSystemRuntime = true,
            RuntimeNotice = null, Revision = 23, FocusVersion = 1 });
        await Expect("test.finished");
        await bridge.Completion.WaitAsync(timeout.Token);
        Console.WriteLine("PASS: Tailscale Qt login, resume, refresh, mode confirmation and cancellation, saved-account confirmation and retry, logout confirmation, and return to devices.");
    }
    finally
    {
        bridge.Dispose();
        await bridge.Completion.WaitAsync(TimeSpan.FromSeconds(3));
    }

    void Publish(QtShellTailscaleAccount account)
    {
        var signedIn = account.State == "Connected";
        bridge.Publish(devices with
        {
            TailscaleAccount = account,
            DeviceAccess = signedIn ? "ready" : "signedOut",
            AccountName = signedIn ? account.DisplayName : "",
            AccountSubtitle = signedIn ? "Tailscale" : account.Status
        });
    }

    async Task Expect(string action, string target = "", int revision = 0)
    {
        var next = requests.Reader.ReadAsync(timeout.Token).AsTask();
        if (await Task.WhenAny(next, bridge.Completion) == bridge.Completion && !next.IsCompleted)
            throw new InvalidOperationException($"Qt closed before sending {action}.");
        var actual = await next;
        if (actual != (new QtShellRequest(action, null, 0) { AccountTarget = target, AccountRevision = revision }))
            throw new InvalidOperationException($"Expected {action} {target} {revision}; received {actual}.");
    }
}
