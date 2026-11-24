using System;
using System.Diagnostics;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Threading;
using System.Threading.Tasks;

[assembly: InternalsVisibleTo("Aurora.TailscaleAccount.Replay")]

namespace Aurora;

internal enum TailscaleAccountFocus { Primary, Mode, Profile }

internal sealed record TailscaleAccountView(TailscaleState? State, string Status, string Detail, string PrimaryAction,
    string DisplayName, string LoginName, string Initial, string TailnetName, string LocalAddress);
internal sealed record TailscaleAccountFeedback(string Message, bool Runtime, bool Timed);

// Desktop callers serialize entry points and continuations on their dispatcher.
internal sealed partial class TailscaleAccountSession : IDisposable
{
    private readonly ITailscaleAccountRuntime runtime;
    private readonly Action<Action> post;
    private readonly Func<Task<bool>> confirmLogout;
    private readonly Func<string, string, Task<bool>> confirmSwitch;
    private readonly Func<bool> hasActiveSession;
    private readonly Action<string> showSwitchSuccess;
    private readonly SemaphoreSlim operation = new(1, 1);
    private TailscaleStatus current = new(TailscaleState.Unknown);
    private TailscaleMode mode;
    private TailscaleProfile[] profiles = [];
    private TailscaleProfile currentProfile;
    private int readVersion;
    private int foregroundOperations;
    private int interactionEpoch;
    private bool loginAfterSwitch;
    private bool switchingProfile;
    private Func<Task> retry;
    private string switchMessage;
    private string address;
    private bool hasStatus;
    private bool active;
    private bool closed;
    private bool confirmingLogout;
    private bool refreshing;
    private bool focusPrimary;
    private bool focusLoginAfterLogout;
    private int primaryFocusVersion;

    internal TailscaleAccountSession(ITailscaleAccountRuntime runtime, Action<Action> post,
        Func<Task<bool>> confirmLogout, Func<string, string, Task<bool>> confirmSwitch,
        Func<bool> hasActiveSession, Action<string> showSwitchSuccess)
    {
        this.runtime = runtime;
        this.post = post;
        this.confirmLogout = confirmLogout;
        this.confirmSwitch = confirmSwitch;
        this.hasActiveSession = hasActiveSession;
        this.showSwitchSuccess = showSwitchSuccess;
        mode = runtime.Mode;
        readVersion = runtime.Generation;
        runtime.ContextChanged += OnRuntimeChanged;
    }

    internal event Action Changed;
    internal event Action<TailscaleStatus> StatusChanged;
    internal event Action<TailscaleAccountFocus> FocusRequested;
    internal bool Active => active && !closed;
    internal bool Busy => foregroundOperations > 0 || confirmingLogout || runtime.IsTransitioning;
    private bool HasCurrentContext => IsCurrentContext(mode, readVersion);
    internal bool CanChoose => Active && !Busy && HasCurrentContext;
    internal bool CanLogout => hasStatus && HasCurrentContext && (CanChoose || confirmingLogout && foregroundOperations == 0);
    internal bool SupportsEmbedded => runtime.SupportsEmbedded;
    internal bool HasSystemRuntime => runtime.HasSystemRuntime;
    internal TailscaleMode Mode => mode;
    internal TailscaleStatus Current => current;
    internal TailscaleProfile[] Profiles => HasCurrentContext && mode == TailscaleMode.System ? profiles : [];
    internal TailscaleProfile CurrentProfile => HasCurrentContext ? currentProfile : null;
    internal bool IsConnected => HasCurrentContext && hasStatus && current.State == TailscaleState.Connected;
    internal bool WaitingForLoginFocus => focusLoginAfterLogout;
    internal string Progress => switchMessage;
    internal bool RuntimeProgress => switchMessage != null && !switchingProfile;
    internal TailscaleAccountFeedback Feedback { get; private set; }
    internal bool RetryAvailable => retry != null;

    internal Task SetActive(bool value)
    {
        if (closed || active == value) return Task.CompletedTask;
        active = value;
        if (active)
            return Run(() => Read(refreshAddress: true, refreshProfiles: true), keepFeedback: true);

        RevokePendingActions();
        focusPrimary = focusLoginAfterLogout = false;
        Feedback = null;
        Notify();
        return Task.CompletedTask;
    }

    internal void RevokePendingActions()
    {
        interactionEpoch++;
        SetRetry(null);
    }

    internal Task Poll() => Active ? Run(() => Read(), background: true) : Task.CompletedTask;

    internal async Task RefreshStatus()
    {
        if (!CanChoose) return;
        refreshing = true;
        try
        {
            await Run(async () =>
            {
                if (runtime.IsTransitioning) return;
                await Read(refreshAddress: true, refreshProfiles: true);
                if (!Active || !HasCurrentContext) return;
                var view = ReadView();
                if (view.State == TailscaleState.Connected)
                    SetFeedback("检测完成，Tailscale 已连接。", timed: true);
                else
                    SetFeedback($"检测完成，Tailscale {view.Status}。");
            }, keepFeedback: true);
        }
        finally
        {
            refreshing = false;
            Notify();
        }
    }

    internal Task RequestRetry() => CanChoose && retry != null ? retry() : Task.CompletedTask;

    public void Dispose()
    {
        closed = true;
        runtime.ContextChanged -= OnRuntimeChanged;
    }

    private void Notify()
    {
        if (!closed) Changed?.Invoke();
    }

    private void SetProfiles(TailscaleProfile[] next)
    {
        var available = next?.Where(profile => !string.IsNullOrWhiteSpace(profile.Id) &&
            (!string.IsNullOrWhiteSpace(profile.Name) || !string.IsNullOrWhiteSpace(profile.LoginName)))
            .DistinctBy(profile => profile.Id, StringComparer.Ordinal).ToArray() ?? [];
        if (profiles.SequenceEqual(available)) return;
        profiles = available;
        currentProfile = profiles.FirstOrDefault(profile => profile.IsCurrent);
        Notify();
    }

    private void InvalidateAccount()
    {
        hasStatus = false;
        address = null;
        focusLoginAfterLogout = false;
        loginAfterSwitch = false;
    }

    private void SynchronizeMode()
    {
        var next = runtime.Mode;
        if (next == mode) return;
        mode = next;
        InvalidateAccount();
        SetProfiles(null);
        Notify();
    }

    private void OnRuntimeChanged() => post(() =>
    {
        if (closed) return;
        if (switchMessage != null || runtime.IsTransitioning)
        {
            Notify();
            return;
        }
        var version = runtime.Generation;
        var changed = mode != runtime.Mode || readVersion != version;
        if (!changed) return;
        readVersion = version;
        SynchronizeMode();
        InvalidateAccount();
        SetProfiles(null);
        SetRetry(null);
        if (active)
            _ = Run(() => Read(refreshAddress: true, forceStatus: true, refreshProfiles: true), keepFeedback: true);
    });

    internal Task RequestModeSwitch(TailscaleMode choice)
    {
        if (!CanChoose || choice == mode ||
            (choice == TailscaleMode.Embedded ? !runtime.SupportsEmbedded : !runtime.HasSystemRuntime))
            return Task.CompletedTask;

        var label = DescribeMode(choice, hasSystem: true);
        return SwitchSelection(TailscaleAccountFocus.Mode, () => choice != mode, () => mode == choice,
            () => ConfirmChange($"切换到{label}？",
                choice == TailscaleMode.Embedded
                    ? "Aurora 将通过内置 Tailscale 登录的网络发现和连接设备。"
                    : "Aurora 将通过系统 Tailscale 当前账号所在的网络发现和连接设备。"),
            () => runtime.SwitchMode(choice), () => RequestModeSwitch(choice),
            $"正在切换到{label}…", $"已切换至{label}");
    }

    internal Task RequestProfileSwitch(TailscaleProfile choice)
    {
        bool IsAvailable() => mode == TailscaleMode.System && choice.Id != currentProfile?.Id &&
            profiles.Any(profile => profile.Id == choice.Id);
        if (!CanChoose || !IsAvailable()) return Task.CompletedTask;
        var account = string.IsNullOrWhiteSpace(choice.LoginName) ? choice.Name : choice.LoginName;
        return SwitchSelection(TailscaleAccountFocus.Profile, IsAvailable, () => currentProfile?.Id == choice.Id,
            () => hasActiveSession?.Invoke() == true
                ? ConfirmChange("切换系统 Tailscale 账号？",
                    $"将切换到 {account}，并更改系统 Tailscale 的当前账号。切换可能会断开正在进行的远控会话。")
                : Task.FromResult(true),
            () => runtime.SwitchProfile(choice.Id), () => RequestProfileSwitch(choice),
            "正在切换系统 Tailscale 账号…", $"已切换至 {account}");
    }

    private Task<bool> ConfirmChange(string title, string description)
    {
        if (confirmSwitch != null) return confirmSwitch(title, description);
        SetFeedback("无法打开切换确认。");
        return Task.FromResult(false);
    }

    private async Task SwitchSelection(TailscaleAccountFocus selector, Func<bool> isApplicable, Func<bool> isConfirmed, Func<Task<bool>> approve,
        Func<Task> change, Func<Task> retryChange, string progress, string success)
    {
        var selectedMode = mode;
        var version = runtime.Generation;
        var declined = false;
        var succeeded = false;
        var forAccount = selector == TailscaleAccountFocus.Profile;
        var interaction = interactionEpoch;
        await Run(async () =>
        {
            if (!active || interaction != interactionEpoch || !IsCurrentContext(selectedMode, version) || !isApplicable()) return;
            if (!await approve())
            {
                declined = true;
                return;
            }
            if (!active || interaction != interactionEpoch || !IsCurrentContext(selectedMode, version) || !isApplicable()) return;
            switchingProfile = forAccount;
            switchMessage = progress;
            Notify();
            string failure = null;
            var retryable = false;
            try
            {
                await change();
                if (!closed)
                {
                    InvalidateAccount();
                    SetProfiles(null);
                    Notify();
                }
            }
            catch (OperationCanceledException) { failure = "已取消切换。"; }
            catch (Exception error)
            {
                failure = error.Message;
                retryable = true;
            }

            try
            {
                if (closed) return;
                switchMessage = "正在检查当前 Tailscale 状态…";
                Notify();
                await Read(refreshAddress: true, forceStatus: true, refreshProfiles: true);
                if (failure == null && (!hasStatus || current.State == TailscaleState.Unknown || !isConfirmed()))
                    failure = "暂时无法确认切换结果，请刷新状态。";
                if (failure == null && current.State == TailscaleState.NeedsLogin)
                {
                    loginAfterSwitch = forAccount || current.OwnerId.HasValue;
                    focusPrimary = true;
                    Notify();
                }
            }
            catch (Exception error) { failure ??= error.Message; }
            finally
            {
                switchMessage = null;
                switchingProfile = false;
                if (!closed)
                {
                    Notify();
                    if (failure != null)
                    {
                        SetFeedback(failure, runtime: !forAccount);
                        if (retryable && isApplicable()) SetRetry(retryChange);
                    }
                    else succeeded = true;
                }
            }
        });
        if (CanChoose && interaction == interactionEpoch && (declined || forAccount && !loginAfterSwitch))
            FocusRequested?.Invoke(selector);
        if (succeeded && Active && interaction == interactionEpoch) showSwitchSuccess?.Invoke(success);
    }

    internal Task RequestPrimaryAction()
    {
        if (!CanChoose || !hasStatus || ReadView().PrimaryAction.Length == 0) return Task.CompletedTask;
        return mode == TailscaleMode.System && current.State == TailscaleState.NotInstalled
            ? RequestModeSwitch(TailscaleMode.Embedded) : Run(Activate);
    }

    private async Task Activate()
    {
        var state = current.State;
        var interaction = interactionEpoch;
        var selectedMode = mode;
        var version = runtime.Generation;
        if (!IsCurrentContext(selectedMode, version)) return;

        if (state == TailscaleState.NotInstalled)
        {
            if (selectedMode != TailscaleMode.Embedded || !runtime.SupportsEmbedded) return;
        }
        else if (state is not (TailscaleState.Stopped or TailscaleState.NeedsLogin)) return;

        if (state != TailscaleState.NeedsLogin || current.LoginUri == null)
        {
            await runtime.RequestConnection();
            if (!IsCurrentContext(selectedMode, version)) return;
            await Read();
        }
        if (!active || interaction != interactionEpoch || !IsCurrentContext(selectedMode, version) ||
            state != TailscaleState.NeedsLogin || current.State != TailscaleState.NeedsLogin) return;

        if (current.LoginUri == null)
            SetFeedback("正在等待 Tailscale 生成官方登录链接。链接就绪后，请再次点击「登录 Tailscale」。");
        else if (!IsOfficialLoginUri(current.LoginUri))
            SetFeedback("无法验证 Tailscale 登录链接，请刷新状态后重试。");
        else if (Open(current.LoginUri))
        {
            SetFeedback("请在浏览器完成 Tailscale 登录，Aurora 会自动刷新。");
        }
    }

    private static bool IsOfficialLoginUri(Uri uri) =>
        uri is { IsAbsoluteUri: true, Scheme: "https", Host: "login.tailscale.com", IsDefaultPort: true, UserInfo.Length: 0 }
        && uri.AbsolutePath.StartsWith("/a/", StringComparison.Ordinal);

    internal async Task RequestLogout()
    {
        if (!CanChoose || !hasStatus || current.State != TailscaleState.Connected) return;
        var selectedMode = mode;
        var version = runtime.Generation;
        var owner = current.OwnerId;
        var interaction = interactionEpoch;
        confirmingLogout = true;
        Notify();
        try
        {
            if (!await confirmLogout() || closed) return;
            await Run(async () =>
            {
                if (!active || interaction != interactionEpoch || !IsCurrentContext(selectedMode, version) || !hasStatus ||
                    current.State != TailscaleState.Connected || current.OwnerId != owner) return;
                await runtime.Logout();
                if (closed) return;
                focusLoginAfterLogout = active;
                await Read(forceStatus: true, refreshProfiles: true);
            });
        }
        catch (Exception error)
        {
            SetFeedback(error.Message);
        }
        finally
        {
            confirmingLogout = false;
            TryFocusPrimaryAction();
            Notify();
        }
    }

    private bool IsCurrentContext(TailscaleMode selectedMode, int version) =>
        !closed && !runtime.IsTransitioning && selectedMode == runtime.Mode &&
        version == runtime.Generation;

    private async Task Read(bool refreshAddress = false, bool forceStatus = false, bool refreshProfiles = false)
    {
        if (closed || runtime.IsTransitioning) return;
        SynchronizeMode();
        var selectedMode = mode;
        var version = readVersion = runtime.Generation;
        TailscaleStatus next;
        try { next = await runtime.ReadStatus(); }
        catch
        {
            if (!IsCurrentContext(selectedMode, version)) return;
            address = null;
            Publish(new(TailscaleState.Unknown), forceStatus);
            if (refreshProfiles) await RefreshSystemProfiles(selectedMode, version);
            throw;
        }
        if (!IsCurrentContext(selectedMode, version)) return;

        var accountChanged = hasStatus && (current.OwnerId != next.OwnerId || current.TailnetName != next.TailnetName);
        var networkChanged = !hasStatus || current.State != next.State || accountChanged;
        if (networkChanged) address = null;
        if (accountChanged && selectedMode == TailscaleMode.System) SetProfiles(null);
        Publish(next, forceStatus);
        if (IsCurrentContext(selectedMode, version) && next.State == TailscaleState.Connected && (networkChanged || refreshAddress))
        {
            string nextAddress = null;
            try { nextAddress = await runtime.ReadAddress(); }
            catch (Exception error)
            {
                Debug.WriteLine(error);
            }
            if (!IsCurrentContext(selectedMode, version)) return;
            address = nextAddress;
            Notify();
        }
        if (refreshProfiles || accountChanged) await RefreshSystemProfiles(selectedMode, version);
    }

    private async Task RefreshSystemProfiles(TailscaleMode selectedMode, int version)
    {
        if (!active || selectedMode != TailscaleMode.System || !IsCurrentContext(selectedMode, version)) return;
        TailscaleProfile[] next = null;
        try { next = await runtime.ReadProfiles(); }
        catch (NotSupportedException) { }
        catch (Exception error) { Debug.WriteLine(error); }
        if (active && IsCurrentContext(selectedMode, version)) SetProfiles(next);
    }

    private void Publish(TailscaleStatus next, bool force)
    {
        var notify = !hasStatus || current != next || force;
        if (next.State != TailscaleState.NeedsLogin) loginAfterSwitch = false;
        hasStatus = true;
        current = next;
        Notify();
        if (notify) StatusChanged?.Invoke(current);
    }

    internal static string DescribeMode(TailscaleMode mode, bool hasSystem) =>
        mode == TailscaleMode.Embedded ? "内置 Tailscale" : hasSystem ? "系统 Tailscale" : "暂不可用";

    internal static (string Status, string Detail, string Action) DescribeStatus(
        TailscaleState? state, TailscaleMode mode, bool supportsEmbedded, bool loginAfterSwitch = false) =>
        state switch
        {
            null => ("正在检查", "正在读取当前运行方式的连接状态。", ""),
            TailscaleState.NotInstalled when mode == TailscaleMode.Embedded && supportsEmbedded =>
                ("未启动", "启动内置 Tailscale 后即可登录。", "启动内置 Tailscale"),
            TailscaleState.NotInstalled when supportsEmbedded =>
                ("暂不可用", "可以使用内置 Tailscale 连接设备。", "使用内置 Tailscale"),
            TailscaleState.NotInstalled => ("暂不可用", "当前无法建立连接，可刷新状态后重试。", ""),
            TailscaleState.NeedsLogin when loginAfterSwitch =>
                ("未登录", "此账号需要重新登录。", "登录 Tailscale"),
            TailscaleState.NeedsLogin => ("未登录", "登录后可发现同一 Tailscale 网络中的设备。", "登录 Tailscale"),
            TailscaleState.NeedsApproval => ("等待批准", "你的 Tailscale 网络需要管理员批准这台设备。批准后会自动更新。", ""),
            TailscaleState.Stopped when mode == TailscaleMode.Embedded =>
                ("已停止", "启动内置 Tailscale 后即可连接设备。", "启动内置 Tailscale"),
            TailscaleState.Stopped => ("已暂停", "恢复异地连接并保留现有设置。", "恢复连接"),
            TailscaleState.Starting => ("正在启动", "稍等片刻，Aurora 会自动刷新。", ""),
            TailscaleState.Connected => ("已连接", "", ""),
            _ when mode == TailscaleMode.Embedded => ("状态未知", "暂时无法读取内置 Tailscale，请刷新状态后重试。", ""),
            _ => ("状态未知", "暂时无法读取 Tailscale。请确认系统客户端和后台服务已启动，再刷新状态。", "")
        };


    internal TailscaleAccountView ReadView()
    {
        var state = !HasCurrentContext ? TailscaleState.Starting : hasStatus ? current.State : (TailscaleState?)null;
        var (status, detail, primary) = DescribeStatus(state, mode, runtime.SupportsEmbedded, loginAfterSwitch);
        var connected = state == TailscaleState.Connected;
        var profile = HasCurrentContext && hasStatus && mode == TailscaleMode.System ? currentProfile : null;
        var login = connected ? current.LoginName ?? profile?.LoginName : profile?.LoginName;
        var name = (connected ? current.DisplayName : null) ?? profile?.Name ?? login ?? "Tailscale";
        return new(state, status, detail, primary, name, login != name ? login : null, connected ? current.AccountInitial : null,
            connected ? current.TailnetName ?? "暂不可用" : null, connected ? address ?? "暂不可用" : null);
    }

    private void TryFocusPrimaryAction()
    {
        if (!CanChoose || !hasStatus || (!focusPrimary && !focusLoginAfterLogout)) return;
        if (focusLoginAfterLogout && current.State is not (TailscaleState.NeedsLogin or TailscaleState.Stopped or TailscaleState.NotInstalled)) return;
        focusPrimary = focusLoginAfterLogout = false;
        if (ReadView().PrimaryAction.Length != 0) primaryFocusVersion++;
        FocusRequested?.Invoke(TailscaleAccountFocus.Primary);
    }

    private async Task Run(Func<Task> work, bool background = false, bool keepFeedback = false)
    {
        if (closed) return;
        if (background)
        {
            if (confirmingLogout || !operation.Wait(0)) return;
        }
        else
        {
            foregroundOperations++;
            Notify();
            await operation.WaitAsync();
        }

        try
        {
            if (closed) return;
            if (!background && !keepFeedback) SetFeedback(null);
            await work();
        }
        catch (Exception error)
        {
            if (!background) SetFeedback(error.Message);
        }
        finally
        {
            if (!background) foregroundOperations--;
            operation.Release();
            if (!closed)
            {
                TryFocusPrimaryAction();
                Notify();
            }
        }
    }

    private void SetFeedback(string message, bool runtime = false, bool timed = false)
    {
        if (!Active) return;
        retry = null;
        Feedback = message == null ? null : new(message, runtime, timed);
        Notify();
        if (timed && Feedback is { } feedback) _ = ExpireFeedback(feedback);
    }

    private async Task ExpireFeedback(TailscaleAccountFeedback feedback)
    {
        await Task.Delay(TimeSpan.FromSeconds(3));
        if (Active && ReferenceEquals(Feedback, feedback))
        {
            Feedback = null;
            Notify();
        }
    }

    private void SetRetry(Func<Task> change)
    {
        retry = Active ? change : null;
        Notify();
    }

    private bool Open(Uri url)
    {
        try
        {
            runtime.OpenLogin(url);
            return true;
        }
        catch (Exception error)
        {
            SetFeedback("无法打开：" + error.Message);
            return false;
        }
    }
}
