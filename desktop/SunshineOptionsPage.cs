using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;

namespace Aurora;

internal sealed record SunshineOption(string Key, string Label, string Default, string Kind, string Group, string[] Choices);

internal sealed class SunshineOptionsPage : UserControl
{
    private const double ScrollGutter = 8;
    private const double FieldMaxWidth = 235;
    private readonly SunshineAdmin admin;
    private readonly SunshineOption[] catalog;
    private readonly TextBox search = new()
    {
        Classes = { "settingField" }, Watermark = "搜索设置名称或配置键",
        Width = FieldMaxWidth, VerticalAlignment = VerticalAlignment.Center
    };
    private readonly WrapPanel tabs = new() { ItemSpacing = 6, LineSpacing = 6 };
    private readonly StackPanel fields = new() { Spacing = 12, Margin = new Thickness(0, 0, ScrollGutter, 0) };
    private readonly ScrollViewer scroll = new()
    {
        HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
        VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
        AllowAutoHide = true
    };
    private readonly TextBlock feedback = new() { TextWrapping = TextWrapping.Wrap };
    private readonly Dictionary<string, string> original = new(StringComparer.Ordinal);
    private readonly Dictionary<string, string> changed = new(StringComparer.Ordinal);
    private bool busy;
    private string selectedCategory = "显示与编码";

    internal event Action<bool> Edited;
    internal bool HasChanges => changed.Count != 0;
    internal void Report(string message) => feedback.Text = message;
    internal void DiscardChanges() => changed.Clear();

    internal static SunshineOption[] ReadCatalog()
    {
        using var resource = typeof(SunshineOptionsPage).Assembly.GetManifestResourceStream("Aurora.SunshineOptions.json");
        return JsonSerializer.Deserialize<SunshineOption[]>(resource,
            new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
    }

    internal SunshineOptionsPage(SunshineAdmin admin)
    {
        this.admin = admin;
        catalog = ReadCatalog();
        var layout = new Grid { RowDefinitions = new RowDefinitions("Auto,*,Auto"), RowSpacing = 11 };
        Content = layout;

        var filterBar = new Grid
        {
            RowDefinitions = new RowDefinitions("Auto,Auto"),
            ColumnDefinitions = new ColumnDefinitions("*,Auto"),
            ColumnSpacing = 10,
            Margin = new Thickness(0, 0, ScrollGutter, 0)
        };
        foreach (var category in new[] { "显示与编码", "输入", "网络" })
        {
            var tab = new RadioButton
            {
                Classes = { "configTab" },
                Content = category,
                GroupName = "SunshineConfig",
                IsChecked = category == selectedCategory,
                IsTabStop = category == selectedCategory
            };
            AutomationProperties.SetName(tab, category);
            tab.IsCheckedChanged += (_, _) =>
            {
                tab.IsTabStop = tab.IsChecked == true;
                if (tab.IsTabStop) SelectCategory(category);
            };
            tab.Click += (_, _) =>
            {
                if (search.Text?.Length > 0) SelectCategory(category);
            };
            tab.KeyDown += (_, args) => MoveTab(tab, args);
            tabs.Children.Add(tab);
        }
        filterBar.Children.Add(tabs);
        filterBar.Children.Add(search);
        AutomationProperties.SetName(search, "搜索配置项");
        Grid.SetColumn(search, 1);
        filterBar.SizeChanged += (_, e) =>
        {
            var width = e.NewSize.Width;
            var compact = TopLevel.GetTopLevel(this)?.ClientSize.Width <= 470;
            var stacked = compact || width < 475;
            tabs.ItemSpacing = Math.Clamp(width * 0.025, 6, 24);
            Grid.SetRow(search, stacked ? 1 : 0);
            Grid.SetColumn(search, stacked ? 0 : 1);
            Grid.SetColumnSpan(search, stacked ? 2 : 1);
            search.Width = stacked ? double.NaN : Math.Min(FieldMaxWidth, width * 0.48);
            search.Margin = stacked ? new Thickness(0, 6, 0, 0) : default;
        };
        layout.Children.Add(filterBar);

        // The Fluent scrollbar overlays its viewport. Keep it inside the reserved gutter.
        scroll.TemplateApplied += (_, e) =>
        {
            if (e.NameScope.Find<ScrollBar>("PART_VerticalScrollBar") is { } bar)
                bar.Resources["ScrollBarSize"] = ScrollGutter;
        };
        scroll.Content = fields;
        Grid.SetRow(scroll, 1);
        layout.Children.Add(scroll);

        feedback.Classes.Add("muted");
        feedback.Margin = new Thickness(0, 0, ScrollGutter, 0);
        AutomationProperties.SetLiveSetting(feedback, AutomationLiveSetting.Polite);
        Grid.SetRow(feedback, 2);
        layout.Children.Add(feedback);

        search.TextChanged += (_, _) => ApplyFilter();
        search.IsEnabled = tabs.IsEnabled = false;
        feedback.IsVisible = false;
        feedback.PropertyChanged += (_, e) =>
        {
            if (e.Property == TextBlock.TextProperty)
                feedback.IsVisible = !string.IsNullOrWhiteSpace(feedback.Text);
        };
        UpdateButtons();
    }

    internal async Task<bool> SaveOptions()
    {
        var valid = new Dictionary<string, string>(StringComparer.Ordinal);
        string validationError = null;
        foreach (var option in catalog)
        {
            if (!changed.TryGetValue(option.Key, out var value)) continue;
            try
            {
                Validate(option, value);
                valid[option.Key] = value;
            }
            catch (InvalidOperationException error) { validationError ??= error.Message; }
        }
        await admin.SaveOptions(valid);
        foreach (var (key, value) in valid)
        {
            original[key] = value;
            if (changed.TryGetValue(key, out var latest) && latest == value) changed.Remove(key);
        }
        Report(validationError ?? "已保存。重启 Sunshine 后生效。");
        return validationError == null;
    }

    public Task LoadOptions() => Run(async () =>
    {
        await Read();
        search.IsEnabled = tabs.IsEnabled = true;
        Render();
    });

    private async Task Read()
    {
        var snapshot = await admin.ReadOptions();
        original.Clear();
        foreach (var option in catalog)
            original[option.Key] = admin.IsManaged && BundledSunshine.ManagedOptions.TryGetValue(option.Key, out var managed)
                ? managed : snapshot.TryGetProperty(option.Key, out var value) ? value.ToString() : option.Default;
    }

    private void MoveTab(RadioButton current, KeyEventArgs args)
    {
        var index = tabs.Children.IndexOf(current);
        var count = tabs.Children.Count;
        var target = args.Key switch
        {
            Key.Left => (index + count - 1) % count,
            Key.Right => (index + 1) % count,
            Key.Home => 0,
            Key.End => count - 1,
            _ => -1
        };
        if (target < 0) return;
        args.Handled = true;
        var tab = (RadioButton)tabs.Children[target];
        tab.IsChecked = true;
        tab.Focus(NavigationMethod.Directional);
    }

    private void SelectCategory(string category)
    {
        selectedCategory = category;
        if (string.IsNullOrEmpty(search.Text))
            ApplyFilter();
        else
            search.Text = "";
    }

    private void ApplyFilter()
    {
        Render();
        scroll.Offset = default;
    }

    internal static string CategoryFor(SunshineOption option) => option.Group switch
    {
        "输入" => "输入",
        "网络" => "网络",
        _ => "显示与编码"
    };

    private void Render()
    {
        fields.Children.Clear();
        if (original.Count == 0) return;
        var query = search.Text?.Trim() ?? "";
        var matches = catalog.Where(option => query.Length == 0
            ? CategoryFor(option) == selectedCategory
            : option.Label.Contains(query, StringComparison.OrdinalIgnoreCase) || option.Key.Contains(query, StringComparison.OrdinalIgnoreCase) ||
              option.Group.Contains(query, StringComparison.OrdinalIgnoreCase));
        foreach (var group in matches.GroupBy(option => option.Group))
        {
            var rows = new StackPanel();
            var heading = new TextBlock
            {
                Classes = { "sectionTitle" }, Text = group.Key, TextWrapping = TextWrapping.Wrap
            };
            AutomationProperties.SetHeadingLevel(heading, 3);
            rows.Children.Add(new Border { MinHeight = 45, Child = heading });
            foreach (var option in group)
                rows.Children.Add(CreateField(option));
            fields.Children.Add(new Border { Classes = { "panel", "settingsGroup" }, Child = rows });
        }
        if (fields.Children.Count == 0)
        {
            var empty = new TextBlock { Text = "没有匹配的设置。", Margin = new Thickness(0, 14) };
            empty.Classes.Add("muted");
            fields.Children.Add(empty);
        }
    }

    private Control CreateField(SunshineOption option)
    {
        var value = changed.GetValueOrDefault(option.Key, original[option.Key]);
        var title = new TextBlock { Classes = { "settingLabel" }, Text = option.Label };
        var key = new TextBlock
        {
            Classes = { "muted" }, Text = option.Key, FontSize = 11, TextWrapping = TextWrapping.Wrap
        };
        var label = new StackPanel
        {
            VerticalAlignment = VerticalAlignment.Center, Spacing = 3, Margin = new Thickness(0, 5),
            Children = { title, key }
        };
        var columns = option.Kind == "toggle" ? new ColumnDefinitions("*,Auto") : new ColumnDefinitions
        {
            new ColumnDefinition(52, GridUnitType.Star),
            new ColumnDefinition(48, GridUnitType.Star) { MaxWidth = FieldMaxWidth }
        };
        var row = new Grid { ColumnDefinitions = columns, ColumnSpacing = 10, MinHeight = 45 };
        row.Children.Add(label);
        Control editor;
        if (option.Kind == "choice")
        {
            var choices = option.Choices.Append(value).Distinct().ToArray();
            var combo = new ComboBox
            {
                Classes = { "settingField" }, MinWidth = 0, HorizontalAlignment = HorizontalAlignment.Stretch,
                ItemsSource = choices.Select(choice => choice.Length == 0 ? "自动 / 默认" : choice),
                SelectedIndex = Array.IndexOf(choices, value)
            };
            combo.SelectionChanged += (_, _) =>
            {
                if (combo.SelectedIndex is >= 0 && combo.SelectedIndex < choices.Length)
                    Change(option, choices[combo.SelectedIndex], text: false);
            };
            editor = combo;
        }
        else if (option.Kind == "toggle")
        {
            var toggle = new ToggleSwitch
            {
                Classes = { "settingSwitch" }, IsChecked = IsOptionEnabled(value),
                OnContent = "", OffContent = "", HorizontalAlignment = HorizontalAlignment.Right
            };
            toggle.IsCheckedChanged += (_, _) => Change(option, toggle.IsChecked == true ? "enabled" : "disabled", text: false);
            editor = toggle;
        }
        else
        {
            var input = new TextBox
            {
                Classes = { "settingField" }, MinWidth = 0, Text = value, Watermark = option.Default,
                TextWrapping = option.Kind == "json" ? TextWrapping.Wrap : TextWrapping.NoWrap
            };
            input.PropertyChanged += (_, args) =>
            {
                if (args.Property == TextBox.TextProperty) Change(option, input.Text ?? "", text: true);
            };
            editor = input;
        }
        AutomationProperties.SetName(editor, option.Label);
        AutomationProperties.SetLabeledBy(editor, title);
        var managed = admin.IsManaged && BundledSunshine.ManagedOptions.ContainsKey(option.Key);
        editor.IsEnabled = !managed;
        var help = managed ? "由 Aurora 统一管理" : option.Key;
        AutomationProperties.SetHelpText(editor, help);
        if (managed)
        {
            ToolTip.SetTip(row, help);
            ToolTip.SetShowOnDisabled(row, true);
        }
        editor.Margin = new Thickness(0, 5);
        editor.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(editor, 1);
        row.Children.Add(editor);
        return row;
    }

    internal static bool IsOptionEnabled(string value) =>
        value.Trim().ToLowerInvariant() is "on" or "yes" or "true" or "enable" or "enabled" || value.Contains('1');

    private void Change(SunshineOption option, string value, bool text)
    {
        if (value == changed.GetValueOrDefault(option.Key, original[option.Key])) return;
        changed[option.Key] = value;
        Edited?.Invoke(text);
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
