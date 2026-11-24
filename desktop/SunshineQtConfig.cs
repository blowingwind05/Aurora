using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;

namespace Aurora;

internal static class SunshineQtConfig
{
    private const string Name = "sunshine_name";
    private const string Audio = "stream_audio";
    private const string Source = "displaySource";
    private const string Exclusive = "exclusiveDisplay";
    private const string Legacy = "legacyScreensaver";
    private static readonly Dictionary<string, SunshineOption> Catalog =
        SunshineOptionCatalog.ReadCatalog().ToDictionary(option => option.Key, StringComparer.Ordinal);

    internal static async Task<(QtSunshineConfig Config, SunshinePreferences Preferences, SunshinePrivacySettings Displays)>
        ReadAsync(SunshineAdmin admin)
    {
        var snapshot = await admin.ReadOptions();
        var preferences = SunshineAdmin.ParsePreferences(snapshot);
        var options = Catalog.Values.Select(option => new QtSunshineOption(option.Key, option.Label, option.Default,
            option.Kind, option.Group, SunshineOptionCatalog.CategoryFor(option), option.Choices ?? [],
            admin.IsManaged && BundledSunshine.ManagedOptions.ContainsKey(option.Key))).ToArray();
        var values = options.ToDictionary(option => option.Key, option => option.Managed
            ? BundledSunshine.ManagedOptions[option.Key]
            : snapshot.TryGetProperty(option.Key, out var value) ? value.ToString() : option.Default);
        values[Name] = preferences.Name;
        values[Audio] = preferences.StreamAudio ? "enabled" : "disabled";

        var displays = await admin.ReadPrivacySettings();
        QtSunshinePrivacy privacy = null;
        if (displays != null)
        {
            var saved = displays.Saved;
            values[Source] = saved.Id;
            values[Exclusive] = saved.Exclusive ? "enabled" : "disabled";
            values[Legacy] = saved.Screensaver ? "enabled" : "disabled";
            var legacy = saved.Screensaver || displays.Active.Screensaver;
            var showSource = displays.SupportsVirtualDisplay &&
                (saved.Id.Length != 0 || displays.Active.Id.Length != 0 || !displays.SupportsScreensaver);
            var choices = displays.Displays.Select(display => new QtSunshineDisplay(display.Id, display.Name)).ToList();
            if (saved.Id.Length != 0 && !choices.Any(display => display.Id == saved.Id))
                choices.Add(new(saved.Id, "已选虚拟显示器不可用"));
            choices.Insert(0, new("", "按原有显示设置"));
            privacy = new(showSource, showSource && displays.SupportsExtended, legacy, choices.ToArray(), saved != displays.Active);
        }
        return (new(options, values, privacy), preferences, displays);
    }

    internal static async Task<QtSunshineSaveResult> SaveAsync(SunshineAdmin admin, int id, Dictionary<string, string> changes)
    {
        var saved = new List<string>();
        var errors = new Dictionary<string, string>(StringComparer.Ordinal);
        var preferences = new Dictionary<string, string>(StringComparer.Ordinal);
        var options = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var (key, raw) in changes)
        {
            try
            {
                if (raw == null || raw.Length > 65536) throw new InvalidOperationException("设置值缺失或过长。");
                var value = raw.Trim();
                if (key == Name)
                {
                    SunshineAdmin.ValidateName(raw, allowEmpty: true);
                    preferences[key] = value;
                }
                else if (key is Audio or Exclusive or Legacy)
                {
                    if (value is not ("enabled" or "disabled"))
                        throw new InvalidOperationException("无效的开关值。");
                    preferences[key] = value;
                }
                else if (key == Source)
                {
                    if (value.Length != 0 && !Guid.TryParseExact(value, "B", out _))
                        throw new InvalidOperationException("无效的虚拟显示器。");
                    preferences[key] = value;
                }
                else
                {
                    if (!Catalog.TryGetValue(key, out var option))
                        throw new InvalidOperationException("不支持的 Sunshine 设置。");
                    if (admin.IsManaged && BundledSunshine.ManagedOptions.ContainsKey(key))
                        throw new InvalidOperationException("此配置项由 Aurora 统一管理。");
                    SunshineOptionCatalog.Validate(option, raw);
                    options[key] = value;
                }
            }
            catch (InvalidOperationException error) { errors[key] = error.Message; }
        }

        if (preferences.Count != 0)
        {
            try
            {
                var current = await admin.ReadPreferences();
                var display = await ReadSubmittedDisplay(admin, preferences);
                await admin.SavePreferences(preferences.GetValueOrDefault(Name, current.Name),
                    preferences.GetValueOrDefault(Audio, current.StreamAudio ? "enabled" : "disabled") == "enabled", display);
                saved.AddRange(preferences.Keys);
            }
            catch (Exception error)
            {
                foreach (var key in preferences.Keys) errors[key] = error.Message;
            }
        }

        if (options.Count != 0)
        {
            try
            {
                await admin.SaveOptions(options);
                saved.AddRange(options.Keys);
            }
            catch (Exception error)
            {
                foreach (var key in options.Keys) errors[key] = error.Message;
            }
        }
        return new(id, saved.ToArray(), errors);
    }

    private static async Task<SunshineDisplaySelection> ReadSubmittedDisplay(SunshineAdmin admin,
        Dictionary<string, string> changes)
    {
        if (!changes.ContainsKey(Source) && !changes.ContainsKey(Exclusive) && !changes.ContainsKey(Legacy)) return null;
        var current = await admin.ReadPrivacySettings() ??
            throw new InvalidOperationException("当前被控组件不支持远控显示设置。");
        if ((changes.ContainsKey(Source) && !current.SupportsVirtualDisplay) ||
            (changes.ContainsKey(Exclusive) && !current.SupportsExtended))
            throw new InvalidOperationException("当前被控组件不支持所选显示设置。");
        var id = changes.GetValueOrDefault(Source, current.Saved.Id);
        var legacy = changes.GetValueOrDefault(Legacy, current.Saved.Screensaver ? "enabled" : "disabled") == "enabled";
        if (legacy && !current.Saved.Screensaver)
            throw new InvalidOperationException("旧版自动隐私屏保关闭后不能再次启用。");
        var exclusive = id.Length != 0 && !legacy && (!current.SupportsExtended ||
            changes.GetValueOrDefault(Exclusive, current.Saved.Exclusive ? "enabled" : "disabled") == "enabled");
        return new(id, exclusive, legacy);
    }
}
