using System;
using System.Globalization;
using System.Linq;
using System.Text.Json;

namespace Aurora;

internal sealed record SunshineOption(string Key, string Label, string Default, string Kind, string Group, string[] Choices);

internal static class SunshineOptionCatalog
{
    internal static SunshineOption[] ReadCatalog()
    {
        using var resource = typeof(SunshineOptionCatalog).Assembly.GetManifestResourceStream("Aurora.SunshineOptions.json");
        return JsonSerializer.Deserialize<SunshineOption[]>(resource,
            new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
    }

    internal static string CategoryFor(SunshineOption option) => option.Group switch
    {
        "输入" => "输入",
        "网络" => "网络",
        _ => "显示与编码"
    };

    internal static void Validate(SunshineOption option, string value)
    {
        if (value.Any(char.IsControl) || value.Contains('#'))
            throw new InvalidOperationException($"{option.Label}不能包含换行、控制字符或 #。");
        if (value.Length == 0) return;
        if (option.Kind == "number" && (!double.TryParse(value, NumberStyles.Float,
                CultureInfo.InvariantCulture, out var number) || !double.IsFinite(number)))
            throw new InvalidOperationException($"{option.Label}请输入数字，小数用点。");
        if (option.Key == "port" && (!int.TryParse(value, out var port) || port is < 1029 or > 65514))
            throw new InvalidOperationException("端口须为 1029–65514。");
        if (option.Kind == "json" && option.Key != "keybindings")
        {
            try
            {
                using var json = JsonDocument.Parse(value);
                var expected = option.Key == "global_prep_cmd" ? JsonValueKind.Array : JsonValueKind.Object;
                if (json.RootElement.ValueKind != expected) throw new JsonException();
            }
            catch (JsonException) { throw new InvalidOperationException($"{option.Label}不是有效的 JSON。"); }
        }
    }
}
