using System;
using System.Collections.Generic;
using System.Globalization;

namespace Aurora;

internal static class StreamPreferences
{
    private const string Enabled = "on";
    private const string Disabled = "off";
    private const string ViewerDisplayMode = "windowed";
    // Moonlight Qt 6.0.0 StreamingPreferences::reload() fallbacks.
    private static readonly Dictionary<string, string> Defaults = new(StringComparer.Ordinal)
    {
        ["resolution"] = "1280x720",
        ["fps"] = "60",
        ["display-mode"] = ViewerDisplayMode,
        ["vsync"] = Enabled,
        ["frame-pacing"] = Disabled,
        ["audio-config"] = "stereo",
        ["audio-on-host"] = Disabled,
        ["mute-on-focus-loss"] = Disabled,
        ["keep-awake"] = Enabled,
        ["absolute-mouse"] = Disabled,
        ["capture-system-keys"] = "never",
        ["touchscreen-trackpad"] = Disabled,
        ["mouse-buttons-swap"] = Disabled,
        ["reverse-scroll-direction"] = Disabled,
        ["swap-gamepad-buttons"] = Disabled,
        ["background-gamepad"] = Disabled,
        ["video-decoder"] = "auto",
        ["video-codec"] = "auto",
        ["hdr"] = Disabled,
        ["performance-overlay"] = Disabled,
        ["game-optimization"] = Enabled,
        ["quit-after"] = Disabled,
    };
    private static readonly Dictionary<string, string> Overrides = new(StringComparer.Ordinal);

    internal static string Get(string option)
    {
        lock (Overrides) return Overrides.GetValueOrDefault(option) ?? DefaultValue(option);
    }

    internal static void Set(string option, string value)
    {
        lock (Overrides) Overrides[option] = value;
    }

    internal static bool GetToggle(string option) => Get(option) == Enabled;

    internal static void SetToggle(string option, bool value) => Set(option, value ? Enabled : Disabled);

    internal static int GetBitrate() => int.Parse(Get("bitrate"), CultureInfo.InvariantCulture);

    internal static void SetBitrate(int kbps) => Set("bitrate", kbps.ToString(CultureInfo.InvariantCulture));

    internal static void Reset()
    {
        lock (Overrides) Overrides.Clear();
    }

    // getDefaultBitrate() at the resolutions and frame rates offered by Aurora.
    // Called with Overrides locked so the resolution and FPS form a single snapshot.
    private static string DefaultValue(string option)
    {
        if (option != "bitrate") return Defaults[option];
        var resolution = Overrides.GetValueOrDefault("resolution", Defaults["resolution"]);
        var fps = Overrides.GetValueOrDefault("fps", Defaults["fps"]);
        var resolutionFactor = resolution switch
        {
            "3840x2160" => 40,
            "2560x1440" => 20,
            "1920x1080" => 10,
            _ => 5,
        };
        var frameRateFactor = fps == "60" ? 2 : 1;
        return (resolutionFactor * frameRateFactor * 1000).ToString(CultureInfo.InvariantCulture);
    }

    internal static string[] Arguments(bool useViewerDefault = false)
    {
        Dictionary<string, string> effective;
        lock (Overrides) effective = new Dictionary<string, string>(Overrides, StringComparer.Ordinal);

        if (useViewerDefault) effective.TryAdd("display-mode", ViewerDisplayMode);
        if (effective.GetValueOrDefault("vsync") == Disabled)
            effective["frame-pacing"] = Disabled;
        if (effective.GetValueOrDefault("hdr") == Enabled)
            effective["video-decoder"] = "auto";

        var arguments = new List<string>();
        foreach (var (option, value) in effective)
        {
            arguments.Add(value == Disabled ? "--no-" + option : "--" + option);
            if (value is not (Enabled or Disabled)) arguments.Add(value);
        }
        return arguments.ToArray();
    }
}
