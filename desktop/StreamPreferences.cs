using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text.Json;

namespace Aurora;

internal static class StreamPreferences
{
    internal const string FollowHostResolution = "host";

    private static readonly StreamPreferenceStore Store = new(GetSettingsPath());

    internal static string Get(string option) => Store.Get(option);
    internal static bool Set(string option, string value) => Store.Set(option, value);
    internal static bool GetToggle(string option) => Store.GetToggle(option);
    internal static bool SetToggle(string option, bool value) => Store.SetToggle(option, value);
    internal static int GetBitrate() => Store.GetBitrate();
    internal static bool SetBitrate(int kbps) => Store.SetBitrate(kbps);
    internal static bool Reset() => Store.Reset();
    internal static string[] Arguments(bool useViewerDefault = false, bool useBundledDefaults = false) =>
        Store.Arguments(useViewerDefault, useBundledDefaults);

    private static string GetSettingsPath()
    {
        var directory = Environment.GetFolderPath(OperatingSystem.IsMacOS()
            ? Environment.SpecialFolder.ApplicationData : Environment.SpecialFolder.LocalApplicationData,
            Environment.SpecialFolderOption.DoNotVerify);
        return string.IsNullOrEmpty(directory) ? null : Path.Combine(directory, "Aurora", "stream-preferences.json");
    }
}

internal sealed class StreamPreferenceStore
{
    private const int MaxSettingsBytes = 16 * 1024;
    private const string Enabled = "on";
    private const string Disabled = "off";
    private const string ViewerDisplayMode = "windowed";
    // Moonlight Qt 6.0.0 fallbacks with Aurora's desktop-oriented viewer defaults.
    private static readonly Dictionary<string, string> Defaults = new(StringComparer.Ordinal)
    {
        ["resolution"] = StreamPreferences.FollowHostResolution,
        ["fps"] = "60",
        ["display-mode"] = ViewerDisplayMode,
        ["vsync"] = Enabled,
        ["frame-pacing"] = Disabled,
        ["audio-config"] = "stereo",
        ["audio-on-host"] = Disabled,
        ["mute-on-focus-loss"] = Disabled,
        ["keep-awake"] = Enabled,
        ["absolute-mouse"] = Enabled,
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

    private readonly string settingsPath;
    private readonly Dictionary<string, string> overrides;

    internal StreamPreferenceStore(string settingsPath)
    {
        this.settingsPath = settingsPath;
        overrides = ReadOverrides(settingsPath);
    }

    internal string Get(string option)
    {
        lock (overrides) return overrides.GetValueOrDefault(option) ?? DefaultValue(option);
    }

    // The bool reports persistence. The selection still applies for this run if the disk write fails.
    internal bool Set(string option, string value)
    {
        if (!IsValid(option, value))
            throw new ArgumentException("Invalid stream preference option or value.", nameof(value));

        lock (overrides)
        {
            overrides[option] = value;
            return SaveOverrides();
        }
    }

    internal bool GetToggle(string option) => Get(option) == Enabled;
    internal bool SetToggle(string option, bool value) => Set(option, value ? Enabled : Disabled);

    internal int GetBitrate() => int.Parse(Get("bitrate"), CultureInfo.InvariantCulture);
    internal bool SetBitrate(int kbps) => Set("bitrate", kbps.ToString(CultureInfo.InvariantCulture));

    internal bool Reset()
    {
        lock (overrides)
        {
            overrides.Clear();
            return SaveOverrides();
        }
    }

    // Estimate for the settings slider before remote capture geometry is available.
    // Called with overrides locked so the resolution and FPS form a single snapshot.
    private string DefaultValue(string option)
    {
        if (option != "bitrate") return Defaults[option];
        var resolution = overrides.GetValueOrDefault("resolution", Defaults["resolution"]);
        var fps = overrides.GetValueOrDefault("fps", Defaults["fps"]);
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

    internal string[] Arguments(bool useViewerDefault = false, bool useBundledDefaults = false)
    {
        Dictionary<string, string> effective;
        lock (overrides) effective = new Dictionary<string, string>(overrides, StringComparer.Ordinal);

        if (effective.GetValueOrDefault("resolution") == StreamPreferences.FollowHostResolution)
            effective.Remove("resolution");
        if (useViewerDefault) effective.TryAdd("display-mode", ViewerDisplayMode);
        effective.TryAdd("absolute-mouse", Defaults["absolute-mouse"]);
        if (useBundledDefaults)
        {
            effective.TryAdd("vsync", Defaults["vsync"]);
            effective.TryAdd("frame-pacing", Defaults["frame-pacing"]);
            effective.TryAdd("keep-awake", Defaults["keep-awake"]);
        }
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

    private static bool IsValid(string option, string value)
    {
        if (option == "bitrate")
            return int.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out int kbps)
                && kbps is >= 500 and <= 150000 && value == kbps.ToString(CultureInfo.InvariantCulture);
        if (option == null || !Defaults.TryGetValue(option, out var defaultValue)) return false;
        if (defaultValue is Enabled or Disabled) return value is Enabled or Disabled;

        return option switch
        {
            "resolution" => value is StreamPreferences.FollowHostResolution or "1280x720" or "1920x1080" or "2560x1440" or "3840x2160",
            "fps" => value is "30" or "60",
            "display-mode" => value is "fullscreen" or "borderless" or ViewerDisplayMode,
            "audio-config" => value is "stereo" or "5.1-surround" or "7.1-surround",
            "capture-system-keys" => value is "never" or "fullscreen" or "always",
            "video-decoder" => value is "auto" or "hardware" or "software",
            "video-codec" => value is "auto" or "H.264" or "HEVC" or "AV1",
            _ => false,
        };
    }

    private static Dictionary<string, string> ReadOverrides(string path)
    {
        var result = new Dictionary<string, string>(StringComparer.Ordinal);
        if (path == null) return result;

        try
        {
            using var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Delete);
            if (file.Length > MaxSettingsBytes) return result;
            using var document = JsonDocument.Parse(file);
            if (document.RootElement.ValueKind != JsonValueKind.Object) return result;
            foreach (var property in document.RootElement.EnumerateObject())
            {
                if (property.Value.ValueKind != JsonValueKind.String) continue;
                var value = property.Value.GetString();
                if (IsValid(property.Name, value)) result[property.Name] = value;
            }
        }
        catch (Exception error) when (error is FileNotFoundException or DirectoryNotFoundException)
        {
            // First launch has no overrides.
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException)
        {
            result.Clear();
            Trace.TraceWarning("Aurora could not read the stream preferences: {0}", error.Message);
        }

        return result;
    }

    private bool SaveOverrides()
    {
        if (settingsPath == null)
        {
            Trace.TraceWarning("Aurora could not save the stream preferences because the user data directory is unavailable.");
            return false;
        }

        var temporary = settingsPath + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(settingsPath));
            using (var file = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
            {
                JsonSerializer.Serialize(file, overrides);
                file.Flush(flushToDisk: true);
            }
            File.Move(temporary, settingsPath, overwrite: true);
            return true;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            Trace.TraceWarning("Aurora could not save the stream preferences: {0}", error.Message);
            return false;
        }
        finally
        {
            try { File.Delete(temporary); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException)
            {
                Trace.TraceWarning("Aurora could not clean up a stream preferences temporary file: {0}", error.Message);
            }
        }
    }
}
