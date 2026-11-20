using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;

namespace Aurora;

internal sealed record SunshineOption(string Key, string Label, string Default, string Kind, string Group, string[] Choices);

internal sealed class SunshineOptionsPage : UserControl
{
    private readonly SunshineAdmin admin;
    private readonly SunshineOption[] catalog;
    private readonly TextBox search = new() { Watermark = "搜索设置名称或配置键…" };
    private readonly ListBox groups = new();
    private readonly StackPanel fields = new() { Spacing = 8 };
    private readonly TextBlock feedback = new() { TextWrapping = TextWrapping.Wrap };
    private readonly Button save = new() { Content = "保存改动" };
    private readonly Button restart = new() { Content = "重启 Sunshine" };
    private readonly Dictionary<string, string> original = new(StringComparer.Ordinal);
    private readonly Dictionary<string, string> changed = new(StringComparer.Ordinal);
    private bool busy;
    private bool confirmRestart;
    private bool confirmClose;

    internal static SunshineOption[] ReadCatalog()
    {
        using var resource = typeof(SunshineOptionsPage).Assembly.GetManifestResourceStream("Aurora.SunshineOptions.json");
        return JsonSerializer.Deserialize<SunshineOption[]>(resource,
            new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
    }

    internal SunshineOptionsPage(SunshineAdmin admin, Func<Task> back)
    {
        this.admin = admin;
        catalog = ReadCatalog();
        var header = new StackPanel { Spacing = 7 };
        var heading = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto"), ColumnSpacing = 8 };
        heading.Children.Add(new TextBlock { Text = "Sunshine 详细设置", FontSize = 21, FontWeight = FontWeight.SemiBold, VerticalAlignment = VerticalAlignment.Center });
        var backButton = new Button { Content = "返回常用设置" };
        backButton.Classes.Add("action");
        Grid.SetColumn(backButton, 1);
        heading.Children.Add(backButton);
        header.Children.Add(heading);
        backButton.Click += async (_, _) => { if (CanLeave()) await back(); };
        header.Children.Add(new TextBlock
        {
            Text = "修改后先保存；重启被控服务才会应用，并会断开现有串流。",
            TextWrapping = TextWrapping.Wrap
        });
        header.Children.Add(search);
        var layout = new Grid
        {
            RowDefinitions = new RowDefinitions("Auto,*,Auto"), RowSpacing = 10
        };
        Content = layout;
        layout.Children.Add(header);

        var body = new Grid { ColumnDefinitions = new ColumnDefinitions("126,*"), ColumnSpacing = 12 };
        Grid.SetRow(body, 1);
        groups.ItemsSource = catalog.Select(option => option.Group).Distinct().ToArray();
        body.Children.Add(groups);
        var scroll = new ScrollViewer { Content = fields };
        Grid.SetColumn(scroll, 1);
        body.Children.Add(scroll);
        layout.Children.Add(body);
        var footer = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto,Auto"), ColumnSpacing = 8 };
        Grid.SetRow(footer, 2);
        feedback.Classes.Add("muted");
        feedback.VerticalAlignment = VerticalAlignment.Center;
        footer.Children.Add(feedback);
        save.Classes.Add("action");
        save.Classes.Add("accent");
        restart.Classes.Add("action");
        Grid.SetColumn(save, 1);
        Grid.SetColumn(restart, 2);
        footer.Children.Add(save);
        footer.Children.Add(restart);
        layout.Children.Add(footer);

        groups.SelectionChanged += (_, _) => Render();
        search.TextChanged += (_, _) => Render();
        groups.SelectedIndex = 0;
        search.IsEnabled = groups.IsEnabled = false;
        save.Click += async (_, _) => await Run(async () =>
        {
            await admin.SaveOptions(changed);
            foreach (var change in changed)
                original[change.Key] = change.Value.Length == 0
                    ? catalog.First(option => option.Key == change.Key).Default : change.Value;
            changed.Clear();
            Render();
            feedback.Text = "已保存。重启 Sunshine 后生效。";
        });
        restart.Click += async (_, _) => await Run(async () =>
        {
            if (!confirmRestart)
            {
                confirmRestart = true;
                restart.Content = "再次点击：断开并重启";
                feedback.Text = "重启会断开正在进行的串流。";
                return;
            }
            confirmRestart = false;
            restart.Content = "重启 Sunshine";
            await admin.Restart();
            feedback.Text = "已发送重启请求。请等待 Sunshine 恢复。";
        });
        UpdateButtons();
    }

    public Task LoadOptions() => Run(async () =>
    {
        await Read();
        search.IsEnabled = groups.IsEnabled = true;
        Render();
        feedback.Text = $"已加载 {catalog.Length} 项 Sunshine 设置。";
    });

    public bool CanLeave()
    {
        if (busy) return false;
        if (changed.Count == 0 || confirmClose) return true;
        confirmClose = true;
        feedback.Text = "尚有未保存的改动。再次返回或关闭会放弃这些改动。";
        return false;
    }

    private async Task Read()
    {
        var snapshot = await admin.ReadOptions();
        original.Clear();
        foreach (var option in catalog)
            original[option.Key] = snapshot.TryGetProperty(option.Key, out var value) ? value.ToString() : option.Default;
    }

    private void Render()
    {
        fields.Children.Clear();
        if (original.Count == 0) return;
        var query = search.Text?.Trim() ?? "";
        var selected = groups.SelectedItem as string;
        foreach (var option in catalog.Where(option => query.Length == 0
            ? option.Group == selected
            : option.Label.Contains(query, StringComparison.OrdinalIgnoreCase) || option.Key.Contains(query, StringComparison.OrdinalIgnoreCase) ||
              option.Group.Contains(query, StringComparison.OrdinalIgnoreCase)))
            fields.Children.Add(CreateField(option));
        if (fields.Children.Count == 0) fields.Children.Add(new TextBlock { Text = "没有匹配的设置。" });
    }

    private Control CreateField(SunshineOption option)
    {
        var value = changed.GetValueOrDefault(option.Key, original[option.Key]);
        var label = new StackPanel { VerticalAlignment = VerticalAlignment.Center, Spacing = 3 };
        label.Children.Add(new TextBlock { Text = option.Label, FontWeight = FontWeight.SemiBold, TextWrapping = TextWrapping.Wrap });
        var key = new TextBlock { Text = option.Key, FontSize = 11, TextWrapping = TextWrapping.Wrap };
        key.Classes.Add("muted");
        label.Children.Add(key);
        var row = new Grid { ColumnDefinitions = new ColumnDefinitions("*,1.2*"), ColumnSpacing = 12 };
        row.Children.Add(label);
        Control editor;
        if (option.Kind == "choice")
        {
            var choices = option.Choices.Append(value).Distinct().ToArray();
            var combo = new ComboBox { ItemsSource = choices.Select(choice => choice.Length == 0 ? "自动 / 默认" : choice),
                SelectedIndex = Array.IndexOf(choices, value), HorizontalAlignment = HorizontalAlignment.Stretch };
            combo.SelectionChanged += (_, _) =>
            {
                if (combo.SelectedIndex is >= 0 && combo.SelectedIndex < choices.Length)
                    Change(option, choices[combo.SelectedIndex]);
            };
            editor = combo;
        }
        else if (option.Kind == "toggle")
        {
            var toggle = new ToggleSwitch
            {
                IsChecked = IsOptionEnabled(value), OnContent = "开启", OffContent = "关闭", HorizontalAlignment = HorizontalAlignment.Left
            };
            toggle.IsCheckedChanged += (_, _) => Change(option, toggle.IsChecked == true ? "enabled" : "disabled");
            editor = toggle;
        }
        else
        {
            var input = new TextBox { Text = value, HorizontalAlignment = HorizontalAlignment.Stretch,
                Watermark = option.Default, TextWrapping = option.Kind == "json" ? TextWrapping.Wrap : TextWrapping.NoWrap };
            input.TextChanged += (_, _) => Change(option, input.Text ?? "");
            editor = input;
        }
        Avalonia.Automation.AutomationProperties.SetName(editor, option.Label);
        Grid.SetColumn(editor, 1);
        row.Children.Add(editor);
        return new Border
        {
            Background = Brushes.White, CornerRadius = new CornerRadius(9), Padding = new Thickness(12, 8), Child = row
        };
    }

    internal static bool IsOptionEnabled(string value) =>
        value.Trim().ToLowerInvariant() is "on" or "yes" or "true" or "enable" or "enabled" || value.Contains('1');

    private void Change(SunshineOption option, string value)
    {
        if (value == original[option.Key]) changed.Remove(option.Key);
        else changed[option.Key] = value;
        confirmClose = confirmRestart = false;
        restart.Content = "重启 Sunshine";
        feedback.Text = changed.Count == 0 ? "没有待保存的改动。" : $"有 {changed.Count} 项待保存。";
        UpdateButtons();
    }

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

    private void UpdateButtons()
    {
        fields.IsEnabled = !busy;
        save.IsEnabled = !busy && changed.Count > 0;
        restart.IsEnabled = !busy && original.Count > 0 && changed.Count == 0;
    }

    private async Task Run(Func<Task> action)
    {
        if (busy) return;
        busy = true;
        UpdateButtons();
        try { await action(); }
        catch (Exception error) { feedback.Text = error.Message; }
        finally { busy = false; UpdateButtons(); }
    }
}
