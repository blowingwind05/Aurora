using System.Threading.Channels;
using Aurora;

if (args.Length != 2) throw new ArgumentException("Expected the Qt executable and HostReplay.qml paths.");
using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(30));
var received = Channel.CreateUnbounded<QtShellRequest>();
using var bridge = QtShellBridge.Start(args[0], args[1], request => received.Writer.TryWrite(request));
var shell = new QtShellState("signedOut", "", "未登录", [], 0, false, false, "", false, false, true);
var disabled = new QtShellHostState(true, "disabled", true, false, true, false, true, false, false,
    "", "", false, false, "", "", false);
var running = disabled with
{
    Status = "running", Allowed = true, LanAddress = "192.0.2.18:47989", SettingsAvailable = true
};
const string tailnet = "[fd7a:115c:a1e0:ab12:4843:cd96:627f:caf4]:47989";

Publish(disabled);
await Expect("hostEnable");
Publish(disabled with { Status = "starting", Busy = true, RefreshAvailable = false });
await Expect("test-starting");
Publish(running);
await Expect("hostCopyLan");
Publish(running with { TailscaleAddress = "100.100.0.18:47989" });
await Expect("test-narrow");
Publish(running with { TailscaleAddress = tailnet });
await Expect("hostCopyTailnet");
var permissions = running with { TailscaleAddress = tailnet, PermissionsAvailable = true };
Publish(permissions);
await Expect("hostOpenPermissions");
Publish(permissions with { Feedback = "已打开 macOS 隐私设置。" });
await Expect("hostContinuePermissions");
Publish(permissions with { Busy = true, Refreshing = true });
await Expect("test-permission-pending");
Publish(permissions with { Feedback = "检测完成，Sunshine 正在运行。" });
await Expect("hostOpenSettings");
Publish(running with { Feedback = "已打开 Sunshine 设置。" });
await Expect("hostRefresh");
Publish(running with { Busy = true, Refreshing = true });
await Expect("test-refreshing");
Publish(running with { Feedback = "检测完成，Sunshine 正在运行。" });
await Expect("test-cancelled");
await Expect("hostDisable");
Publish(running with { Status = "stopping", Busy = true });
await Expect("test-stopping");
Publish(disabled with { Feedback = "已关闭本机被控" });
await Expect("hostEnable");
Publish(disabled with
{
    Status = "notReady", Allowed = true, RetryAvailable = true,
    Hint = "启动本机被控失败。", FeedbackFailed = true
});
await Expect("hostRetry");
Publish(running with { Feedback = "检测完成，Sunshine 正在运行。" });
await Expect("test-finished");
await bridge.Completion.WaitAsync(timeout.Token);
Console.WriteLine("PASS: Qt local host navigation, snapshot-driven switch, both address actions, permissions, refresh, settings handoff, Escape/cancel/confirm, and retry.");

void Publish(QtShellHostState host) => bridge.Publish(shell with
{
    Host = host,
    DeviceAccess = host.TailscaleAddress.Length > 0 ? "ready" : "signedOut",
    AccountName = host.TailscaleAddress.Length > 0 ? "回放账户" : "",
    AccountSubtitle = host.TailscaleAddress.Length > 0 ? "Tailscale" : "未登录"
});

async Task Expect(string action)
{
    var next = received.Reader.ReadAsync(timeout.Token).AsTask();
    if (await Task.WhenAny(next, bridge.Completion) == bridge.Completion && !next.IsCompleted)
        throw new InvalidOperationException($"Qt closed before sending {action}.");
    var request = await next;
    if (request != new QtShellRequest(action, "", 0))
        throw new InvalidOperationException($"Expected {action}, got {request}.");
}
