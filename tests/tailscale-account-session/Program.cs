using Aurora;

try
{
    await VerifyModeProfilesAndRetry();
    await VerifyLateReadAndStaleConfirmation();
    await VerifyLogoutOwnerAndLogin();
    return 0;
}
catch (Exception error)
{
    Console.Error.WriteLine(error);
    return 1;
}

static TailscaleStatus Connected(long owner, string name) =>
    new(TailscaleState.Connected, TailnetName: name + "-network", DisplayName: name, OwnerId: owner);

static void Check(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}

static async Task VerifyModeProfilesAndRetry()
{
    var runtime = new OfflineRuntime(TailscaleMode.Embedded, Connected(1, "Embedded"));
    var prompt = new Approval();
    var notices = new List<string>();
    using var session = Session(runtime, prompt, notices, hasActiveStream: true);
    await session.SetActive(true);
    var embedded = session.ReadQtPresentation();
    Check(embedded is { Mode: "embedded", State: "Connected", Profiles.Length: 0 } &&
        embedded.DisplayName == "Embedded" && runtime.ProfileReads == 0, "embedded account must not inspect system profiles");
    await session.HandleQtAction("tailscale.profile", "0", embedded.Revision);
    Check(runtime.ProfileSwitches.Count == 0, "embedded profile request reached system runtime");

    var switchMode = session.HandleQtAction("tailscale.mode", "system", embedded.Revision);
    Check(prompt.Kind == "切换到系统 Tailscale？" && session.ReadQtPresentation().Busy, "missing mode confirmation or busy state");
    prompt.Resolve(false);
    await switchMode;
    Check(runtime.ModeSwitches == 0, "declined mode change reached runtime");

    switchMode = session.HandleQtAction("tailscale.mode", "system", session.ReadQtPresentation().Revision);
    prompt.Resolve(true);
    await switchMode;
    var system = session.ReadQtPresentation();
    Check(runtime.ModeSwitches == 1 && runtime.ProfileReads == 1 && system is { Mode: "system", Profiles.Length: 2 } &&
        system.Revision != embedded.Revision && notices.Single() == "已切换至系统 Tailscale", "system readback or profile deduplication failed");
    await session.HandleQtAction("tailscale.profile", "1", embedded.Revision);
    Check(prompt.Kind == null && runtime.ProfileSwitches.Count == 0, "stale Qt index reached confirmation");

    runtime.FailNextProfile = true;
    var change = session.HandleQtAction("tailscale.profile", "1", system.Revision);
    Check(prompt.Kind == "切换系统 Tailscale 账号？" && prompt.Detail.Contains("远控会话"), "missing active-stream profile confirmation");
    prompt.Resolve(true);
    await change;
    Check(session.ReadQtPresentation() is { RetryAvailable: true, RuntimeFeedback: false, Feedback: "fixture refusal" },
        "profile failure did not produce an account retry");
    var retry = session.HandleQtAction("tailscale.retry", "", session.ReadQtPresentation().Revision);
    Check(prompt.Kind != null, "retry did not reconfirm");
    prompt.Resolve(true);
    await retry;
    Check(runtime.ProfileSwitches.SequenceEqual(["work", "work"]) &&
        session.CurrentProfile?.Id == "work" &&
        session.ReadQtPresentation() is { RetryAvailable: false, DisplayName: "Work" } &&
        session.ReadView().DisplayName == "Work", "profile retry/readback did not update both presentations");

    switchMode = session.HandleQtAction("tailscale.mode", "embedded", session.ReadQtPresentation().Revision);
    prompt.Resolve(true);
    await switchMode;
    var reads = runtime.ProfileReads;
    await session.RefreshStatus();
    Check(session.ReadQtPresentation() is { Mode: "embedded", Profiles.Length: 0 } &&
        runtime.ProfileReads == reads && session.ReadView().DisplayName == "Embedded", "system profiles crossed into embedded mode");
    Console.WriteLine("PASS shared presentation, mode confirmation, profile revisions and retry, embedded/system isolation.");
}

static async Task VerifyLateReadAndStaleConfirmation()
{
    var runtime = new OfflineRuntime(TailscaleMode.System, Connected(2, "Personal"));
    var prompt = new Approval();
    using var session = Session(runtime, prompt, [], hasActiveStream: true);
    await session.SetActive(true);
    var published = new List<TailscaleStatus>();
    session.StatusChanged += published.Add;
    var stale = new TaskCompletionSource<TailscaleStatus>();
    runtime.NextRead = stale.Task;
    var poll = session.Poll();
    var old = session.ReadQtPresentation();
    runtime.BeginTransition();
    Check(session.ReadQtPresentation() is { State: "Starting", Profiles.Length: 0, RefreshAvailable: false } &&
        session.ReadView().LocalAddress == null, "transition showed old account data or accepted actions");
    runtime.FinishTransition(TailscaleMode.System, Connected(3, "Fresh"));
    stale.SetResult(Connected(99, "Stale"));
    await poll;
    await WaitForAccount(session, "Fresh");
    Check(published.All(status => status.OwnerId != 99), "late status from an old generation was published");
    var fresh = session.ReadQtPresentation();

    await session.HandleQtAction("tailscale.profile", "1", old.Revision);
    Check(prompt.Kind == null && fresh.Revision != old.Revision, "old revision remained actionable");

    var change = session.HandleQtAction("tailscale.profile", "1", fresh.Revision);
    runtime.BeginTransition();
    runtime.FinishTransition(TailscaleMode.System, Connected(4, "Next"));
    prompt.Resolve(true);
    await change;
    await WaitForAccount(session, "Next");
    Check(runtime.ProfileSwitches.Count == 0, "an approved request crossed a new generation");

    change = session.HandleQtAction("tailscale.profile", "1", session.ReadQtPresentation().Revision);
    session.RevokePendingActions();
    prompt.Resolve(true);
    await change;
    Check(runtime.ProfileSwitches.Count == 0, "handoff allowed an action from the detached view");
    await session.SetActive(false);
    await session.RequestLogout();
    Check(prompt.Kind == null, "inactive session still accepted an account action");
    Console.WriteLine("PASS transition redaction, late read rejection, stale confirmations, and frontend handoff.");
}

static async Task VerifyLogoutOwnerAndLogin()
{
    var runtime = new OfflineRuntime(TailscaleMode.System, Connected(2, "Personal"));
    var prompt = new Approval();
    using var session = Session(runtime, prompt, [], hasActiveStream: false);
    await session.SetActive(true);
    await session.RequestProfileSwitch(session.Profiles[1]);
    Check(prompt.Kind == null && runtime.ProfileSwitches.SequenceEqual(["work"]),
        "switching a saved account without an active stream should not prompt");
    var canceled = session.RequestLogout();
    Check(prompt.Kind == "logout", "logout did not ask for confirmation");
    prompt.Resolve(false);
    await canceled;

    var late = new TaskCompletionSource<TailscaleStatus>();
    runtime.NextRead = late.Task;
    var poll = session.Poll();
    var logout = session.RequestLogout();
    runtime.Status = Connected(7, "Different owner");
    late.SetResult(runtime.Status);
    await poll;
    prompt.Resolve(true);
    await logout;
    Check(runtime.Logouts == 0, "approval applied to a different owner");

    logout = session.HandleQtAction("tailscale.logout", "", session.ReadQtPresentation().Revision);
    prompt.Resolve(true);
    await logout;
    Check(runtime.Logouts == 1 && session.ReadQtPresentation() is { State: "NeedsLogin", FocusVersion: > 0 } &&
        runtime.Opened.Count == 0, "logout readback or Qt login focus failed");

    runtime.Status = new(TailscaleState.NeedsLogin, new Uri("https://login.tailscale.com.example.invalid/a/test"));
    await session.Poll();
    await session.RequestPrimaryAction();
    Check(session.Feedback?.Message?.StartsWith("无法验证") == true && runtime.Opened.Count == 0,
        "an unofficial login URL reached the opener");

    runtime.Status = new(TailscaleState.Stopped);
    await session.Poll();
    await session.RequestPrimaryAction();
    Check(runtime.ConnectionRequests == 1 && runtime.Opened.Count == 0, "resume launched the login page without another action");
    await session.HandleQtAction("tailscale.primary", "", session.ReadQtPresentation().Revision);
    Check(runtime.Opened.Single().AbsoluteUri == "https://login.tailscale.com/a/offline-fixture", "official login target was not passed to the fake opener");
    Console.WriteLine("PASS logout cancellation, owner gate, login focus, resume and official URL allowlist.");
}

static async Task WaitForAccount(TailscaleAccountSession session, string name)
{
    var completed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
    void OnChanged()
    {
        if (session.CanChoose && session.ReadView().DisplayName == name) completed.TrySetResult();
    }
    session.Changed += OnChanged;
    try
    {
        OnChanged();
        await completed.Task.WaitAsync(TimeSpan.FromSeconds(5));
    }
    finally { session.Changed -= OnChanged; }
}

static TailscaleAccountSession Session(OfflineRuntime runtime, Approval prompt, List<string> notices, bool hasActiveStream) =>
    new(runtime, callback => callback(), () => prompt.Ask("logout", ""), prompt.Ask, () => hasActiveStream, notices.Add);

sealed class Approval
{
    private TaskCompletionSource<bool> pending;
    public string Kind { get; private set; }
    public string Detail { get; private set; }
    public Task<bool> Ask(string kind, string detail)
    {
        if (pending != null) throw new InvalidOperationException("A confirmation is already open.");
        Kind = kind;
        Detail = detail;
        pending = new();
        return pending.Task;
    }
    public void Resolve(bool approved)
    {
        var decision = pending ?? throw new InvalidOperationException("No confirmation was presented.");
        pending = null;
        Kind = Detail = null;
        decision.SetResult(approved);
    }
}

sealed class OfflineRuntime(TailscaleMode mode, TailscaleStatus status) : ITailscaleAccountRuntime
{
    private string selected = "personal";
    public TailscaleMode Mode { get; private set; } = mode;
    public TailscaleStatus Status { get; set; } = status;
    public bool IsTransitioning { get; private set; }
    public int Generation { get; private set; }
    public bool SupportsEmbedded => true;
    public bool HasSystemRuntime => true;
    public event Action ContextChanged;
    public Task<TailscaleStatus> NextRead { get; set; }
    public bool FailNextProfile { get; set; }
    public int ProfileReads { get; private set; }
    public int ModeSwitches { get; private set; }
    public int Logouts { get; private set; }
    public int ConnectionRequests { get; private set; }
    public List<string> ProfileSwitches { get; } = [];
    public List<Uri> Opened { get; } = [];

    public Task<TailscaleStatus> ReadStatus()
    {
        var result = NextRead;
        NextRead = null;
        return result ?? Task.FromResult(Status);
    }

    public Task<string> ReadAddress() => Task.FromResult(Mode == TailscaleMode.System ? "100.100.0.2" : "100.100.0.1");

    public Task<TailscaleProfile[]> ReadProfiles()
    {
        if (Mode != TailscaleMode.System || IsTransitioning)
            throw new InvalidOperationException("A non-system context requested system profiles.");
        ProfileReads++;
        return Task.FromResult<TailscaleProfile[]>([
            new("personal", "Personal", "personal@example.invalid", "personal-network", selected == "personal"),
            new("work", "Work", "work@example.invalid", "work-network", selected == "work"),
            new("work", "Duplicate", "duplicate@example.invalid", "", false),
            new("", "Malformed", "", "", false)
        ]);
    }

    public void BeginTransition()
    {
        IsTransitioning = true;
        Generation++;
        ContextChanged?.Invoke();
    }

    public void FinishTransition(TailscaleMode next, TailscaleStatus nextStatus)
    {
        Mode = next;
        Status = nextStatus;
        IsTransitioning = false;
        ContextChanged?.Invoke();
    }

    public Task SwitchMode(TailscaleMode next)
    {
        ModeSwitches++;
        BeginTransition();
        var name = next == TailscaleMode.Embedded ? "Embedded" : "Personal";
        FinishTransition(next, new(TailscaleState.Connected, TailnetName: name + "-network", DisplayName: name, OwnerId: 3));
        return Task.CompletedTask;
    }

    public Task SwitchProfile(string id)
    {
        if (Mode != TailscaleMode.System) throw new InvalidOperationException("Profile changes require system mode.");
        ProfileSwitches.Add(id);
        if (FailNextProfile)
        {
            FailNextProfile = false;
            throw new InvalidOperationException("fixture refusal");
        }
        selected = id;
        BeginTransition();
        FinishTransition(Mode, new(TailscaleState.Connected, DisplayName: "Work", OwnerId: 4));
        return Task.CompletedTask;
    }

    public Task RequestConnection()
    {
        ConnectionRequests++;
        Status = new(TailscaleState.NeedsLogin, new Uri("https://login.tailscale.com/a/offline-fixture"));
        return Task.CompletedTask;
    }

    public Task Logout()
    {
        Logouts++;
        Status = new(TailscaleState.NeedsLogin);
        return Task.CompletedTask;
    }

    public void OpenLogin(Uri uri) => Opened.Add(uri);
}
