// SPDX-License-Identifier: GPL-3.0-only
using System.Text.Json;

namespace Aurora;


internal sealed class QtShellGeneralSettings
{
    internal QtShellGeneralPreferences Snapshot { get; private set; }

    internal bool TryHandle(QtShellRequest request, out bool changed)
    {
        changed = false;
        if (request.Action is not ("read-general-settings" or "set-close-to-tray" or
            "set-startup-enabled" or "set-sleep-mode"))
            return false;

        if (request.PreferenceRequestId <= (Snapshot?.AcknowledgedRequestId ?? 0))
            return true;

        var before = Read(request.PreferenceRequestId);
        bool closeFailed = false;
        bool startupFailed = false;
        bool sleepFailed = false;
        switch (request.Action)
        {
            case "set-close-to-tray":
                closeFailed = !before.TrayAvailable || !TryReadBool(request.PreferenceValue, out var closeToTray) ||
                    (closeToTray != before.CloseToTray && !TrayResidency.TrySetCloseToTray(closeToTray));
                break;
            case "set-startup-enabled":
                startupFailed = !before.StartupAvailable || !TryReadBool(request.PreferenceValue, out var startupEnabled) ||
                    (startupEnabled != before.StartupEnabled && !GeneralSettingsPreferences.TrySetStartup(startupEnabled));
                break;
            case "set-sleep-mode":
                sleepFailed = !TryReadSleepMode(request.PreferenceValue, out var mode) ||
                    (mode != HostSleepPrevention.Mode && !GeneralSettingsPreferences.TrySetSleepMode(mode));
                break;
        }

        var after = request.Action == "read-general-settings" ? before : Read(request.PreferenceRequestId);
        changed = (before.CloseToTray, before.StartupEnabled, before.SleepMode) !=
            (after.CloseToTray, after.StartupEnabled, after.SleepMode);
        Snapshot = after with
        {
            CloseFeedback = closeFailed ? GeneralSettingsPreferences.CloseChangeFailed : "",
            StartupFeedback = startupFailed && after.StartupAvailable
                ? GeneralSettingsPreferences.StartupChangeFailed : after.StartupFeedback,
            SleepFeedback = sleepFailed ? GeneralSettingsPreferences.SleepChangeFailed : "",
        };
        return true;
    }

    private static QtShellGeneralPreferences Read(int requestId)
    {
        var startup = GeneralSettingsPreferences.ReadStartup();
        return new QtShellGeneralPreferences(requestId, TrayResidency.CloseToTray, TrayResidency.IsAvailable,
            startup.Enabled, startup.Available, HostSleepPrevention.Mode.ToString(), "", startup.Feedback, "");
    }

    private static bool TryReadBool(JsonElement? payload, out bool value)
    {
        value = payload?.ValueKind == JsonValueKind.True;
        return payload?.ValueKind is JsonValueKind.True or JsonValueKind.False;
    }

    private static bool TryReadSleepMode(JsonElement? payload, out HostSleepMode mode)
    {
        HostSleepMode? parsed = payload is { ValueKind: JsonValueKind.String } text ? text.GetString() switch
        {
            nameof(HostSleepMode.System) => HostSleepMode.System,
            nameof(HostSleepMode.OnAc) => HostSleepMode.OnAc,
            nameof(HostSleepMode.Always) => HostSleepMode.Always,
            _ => null,
        } : null;
        mode = parsed.GetValueOrDefault();
        return parsed.HasValue;
    }
}
