using System.Threading.Channels;
using Aurora;
using Aurora.Replay;

try
{
    ReplayFailureBoundary.DisableCrashDialogs();
    if (args.Length != 2) throw new ArgumentException("Expected the Qt executable and Replay.qml paths.");
    using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20));
    var received = Channel.CreateUnbounded<QtShellRequest>();
    using var bridge = QtShellBridge.Start(args[0], args[1], request => received.Writer.TryWrite(request));
    var loading = new QtShellState("loading", "", "待确认", [], 0, false, false, "", false, false, true);
    var display = new QtShellDevice("node:书房", "书房主机", true, true, "192.0.2.36", "100.100.0.36", "");
    var offline = new QtShellDevice("node:offline", "离线电脑", false, false, "", "100.100.0.24", "");
    var ready = loading with
    {
        DeviceAccess = "ready",
        AccountName = "联调账户",
        AccountSubtitle = "GitHub",
        AccountAvatar = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNQDdjyHwAD8AIpSdRvbwAAAABJRU5ErkJggg==",
        DeviceEpoch = 42,
        RefreshAvailable = true,
        Devices = [display, offline]
    };

    bridge.Publish(loading);
    await Expect("test-loading");
    bridge.Publish(ready);
    await Expect("refresh");
    bridge.Publish(ready with { RefreshAvailable = false, Refreshing = true });
    await Expect("test-refreshing");
    bridge.Publish(ready with { RefreshFeedback = "已刷新，发现 2 台设备。" });
    await Expect("test-refreshed");
    bridge.Publish(ready with { RefreshFeedback = "局域网发现失败。共发现 2 台设备。", RefreshFailed = true });
    await Expect("connect", display.DeviceId, 42);
    bridge.Publish(ready with { ConnectionPending = true, Devices = [offline, display with { DisplayName = "书房主机（更新）" }] });
    await Expect("test-connecting");
    bridge.Publish(loading with { DeviceAccess = "signedOut", AccountSubtitle = "未登录", DeviceEpoch = 43 });
    await Expect("account");
    await Expect("test-finished");
    await bridge.Completion.WaitAsync(timeout.Token);

    using var stopped = QtShellBridge.Start(args[0], args[1], _ => { });
    stopped.Dispose();
    await stopped.Completion.WaitAsync(timeout.Token);
    Console.WriteLine("PASS: Qt loading/account/devices, refresh result, connect device ID, account reset, and child shutdown.");

    async Task Expect(string action, string deviceId = "", int epoch = 0)
    {
        var next = received.Reader.ReadAsync(timeout.Token).AsTask();
        if (await Task.WhenAny(next, bridge.Completion) == bridge.Completion && !next.IsCompleted)
            throw new InvalidOperationException($"Qt closed before sending {action}.");
        var request = await next;
        if (request != new QtShellRequest(action, deviceId, epoch))
            throw new InvalidOperationException($"Expected {action} / {deviceId} / {epoch}, got {request}.");
    }
    return 0;
}
catch (Exception error)
{
    return ReplayFailureBoundary.Report(error);
}
