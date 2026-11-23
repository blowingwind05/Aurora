using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Shapes;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform;

namespace Aurora;

internal sealed class DeviceCard : Border
{
    private static readonly IBrush OfflineWallpaperOverlay = new SolidColorBrush(Color.Parse("#FBFBFB"), 0.30);
    private static readonly Lazy<Bitmap>[] DefaultCovers = Array.ConvertAll(new[]
    {
        "mountain-valley.webp",
        "forest-path.webp",
        "blossom-grove.webp",
        "ginkgo-lakeside.webp",
        "baikal-piano.webp",
        "baikal-accordion.webp",
        "sunflower-field.webp",
        "sunset-sky.webp",
    }, name => new Lazy<Bitmap>(() => LoadCover(name)));
    private readonly Button connect;
    private readonly TextBlock address;
    private readonly StackPanel details;
    private readonly List<StackPanel> toolContents = new();
    private readonly ColumnDefinition moreColumn;
    private readonly bool canConnect;
    private readonly Image coverArt;
    private readonly Bitmap defaultCover;
    private Bitmap preview;
    internal string PreviewPath { get; }
    internal Device Device { get; }

    public DeviceCard(Device device, int index, Func<Device, Task> launch, Func<IPAddress, string, Task> copy,
        Func<Device, DevicePowerAction, Task> requestPower, string previewPath = null)
    {
        canConnect = device.CanConnect;
        Device = device;
        PreviewPath = previewPath;
        Background = Brushes.White;
        BorderBrush = Brush.Parse("#DBE2ED");
        BorderThickness = new Thickness(1);
        CornerRadius = new CornerRadius(14);
        ClipToBounds = true;

        var cover = new Grid();
        defaultCover = DefaultCovers[index % DefaultCovers.Length].Value;
        coverArt = new Image { Source = defaultCover, Stretch = Stretch.UniformToFill, IsHitTestVisible = false };
        cover.Children.Add(coverArt);
        if (!device.Online)
            cover.Children.Add(new Border { Background = OfflineWallpaperOverlay, IsHitTestVisible = false });
        cover.Children.Add(new Border
        {
            IsHitTestVisible = false,
            Background = new LinearGradientBrush
            {
                StartPoint = new RelativePoint(0, 0, RelativeUnit.Relative), EndPoint = new RelativePoint(0, 1, RelativeUnit.Relative),
                GradientStops = new GradientStops
                {
                    new(Color.Parse("#000C1325"), 0.25), new(Color.Parse("#420C1325"), 0.55), new(Color.Parse("#EF0C1325"), 1)
                }
            }
        });

        var title = new TextBlock { Text = device.Name, FontSize = 17, FontWeight = FontWeight.SemiBold,
            Foreground = Brushes.White, TextTrimming = TextTrimming.CharacterEllipsis };
        var addresses = string.Join("  |  ", AvailableAddresses(device).ConvertAll(value => $"{value.Source}  {value.Address}"));
        address = new TextBlock { Text = addresses, FontSize = 12, Foreground = Brushes.White,
            TextTrimming = TextTrimming.CharacterEllipsis, IsVisible = addresses.Length != 0 };
        details = new StackPanel { Spacing = 4, Margin = new Thickness(14, 0, 14, 12), VerticalAlignment = VerticalAlignment.Bottom,
            Children = { title, address } };
        cover.Children.Add(details);
        cover.Children.Add(Status(device));
        ToolTip.SetTip(cover, string.Join("\n", new[] { device.Name, addresses, Description(device) }));
        ToolTip.SetShowOnDisabled(cover, true);

        connect = new Button { Content = cover, Height = 176, IsEnabled = canConnect,
            HorizontalAlignment = HorizontalAlignment.Stretch, Classes = { "deviceHero" } };
        AuroraVisual.Label(connect, $"连接桌面：{device.Name}", Description(device));
        ToolTip.SetShowOnDisabled(connect, true);
        connect.Click += async (_, _) => await launch(device);

        var footer = new Grid { ColumnDefinitions = new ColumnDefinitions("*,*,*,*,34"), Background = Brushes.White };
        moreColumn = footer.ColumnDefinitions[4];
        AddTool(footer, 0, "文件传输", AuroraVisual.FolderSync);
        AddTool(footer, 1, "观看模式", AuroraVisual.SquarePlay);
        AddTool(footer, 2, "终端", AuroraVisual.SquareTerminal);
        AddTool(footer, 3, "端口映射", AuroraVisual.PanelsTopLeft);

        var more = new Button { Classes = { "deviceTool" } };
        more.Content = AuroraVisual.Icon(AuroraVisual.Grid2X2, more, 17);
        AuroraVisual.Label(more, $"更多工具：{device.Name}", "更多工具");
        var menu = new MenuFlyout();
        foreach (var value in AvailableAddresses(device))
        {
            var item = new MenuItem { Header = $"复制{(value.Source == "Tailscale" ? " Tailscale " : value.Source)}地址" };
            item.Resources["MenuFlyoutItemBackgroundPointerOver"] = Brush.Parse("#E8EDF4");
            item.Resources["MenuFlyoutItemForegroundPointerOver"] = Brush.Parse("#252A31");
            item.Click += async (_, _) => { await copy(value.Address, value.Source); more.Focus(); };
            menu.Items.Add(item);
        }
        if (menu.Items.Count > 0) menu.Items.Add(new Separator { Height = 6, Margin = new Thickness(0), Opacity = 0 });
        foreach (var (label, icon, action) in new[]
        {
            ("重启", AuroraVisual.RotateCcw, DevicePowerAction.Restart),
            ("关机", AuroraVisual.Power, DevicePowerAction.Shutdown)
        })
        {
            var item = new MenuItem { Header = label, IsEnabled = device.SupportsPower };
            item.Icon = AuroraVisual.Icon(icon, size: 16);
            item.Resources["MenuFlyoutItemBackgroundPointerOver"] = Brush.Parse("#E8EDF4");
            item.Resources["MenuFlyoutItemForegroundPointerOver"] = Brush.Parse("#252A31");
            if (!device.SupportsPower)
            {
                ToolTip.SetTip(item, "远程电源操作需要被控端在线并支持此功能");
                ToolTip.SetShowOnDisabled(item, true);
            }
            item.Click += async (_, _) => { await requestPower(device, action); more.Focus(); };
            menu.Items.Add(item);
        }
        more.Flyout = menu;
        more.IsEnabled = menu.Items.Count > 0;
        Grid.SetColumn(more, 4);
        footer.Children.Add(more);

        Child = new StackPanel { Children = { connect, footer } };
    }

    internal void ReloadPreview()
    {
        if (PreviewPath == null || !File.Exists(PreviewPath)) return;
        try
        {
            using var file = File.Open(PreviewPath, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Delete);
            if (file.Length is <= 0 or > DesktopWallpaper.MaxPreviewBytes) return;
            var bitmap = new Bitmap(file);
            coverArt.Source = bitmap;
            preview?.Dispose();
            preview = bitmap;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or ArgumentException)
        {
            Debug.WriteLine(error);
        }
    }

    protected override void OnAttachedToVisualTree(VisualTreeAttachmentEventArgs args)
    {
        base.OnAttachedToVisualTree(args);
        ReloadPreview();
    }

    protected override void OnDetachedFromVisualTree(VisualTreeAttachmentEventArgs args)
    {
        coverArt.Source = defaultCover;
        preview?.Dispose();
        preview = null;
        base.OnDetachedFromVisualTree(args);
    }

    public void SetBusy(bool busy)
    {
        connect.IsEnabled = canConnect && !busy;
    }

    protected override Size MeasureOverride(Size availableSize)
    {
        if (double.IsFinite(availableSize.Width)) Resize(availableSize.Width - 2);
        return base.MeasureOverride(availableSize);
    }

    private void Resize(double width)
    {
        if (width <= 0) return;
        connect.Height = Math.Max(176, width / 1.88);
        address.FontSize = width <= 325 ? 11 : 12;
        details.Margin = width <= 250 ? new Thickness(8, 0, 8, 12) : new Thickness(14, 0, 14, 12);
        moreColumn.Width = new GridLength(width <= 250 ? 29 : 34);
        foreach (var content in toolContents)
            content.Orientation = width <= 325 ? Orientation.Vertical : Orientation.Horizontal;
    }

    private void AddTool(Grid footer, int column, string label, string icon)
    {
        var button = new Button { Classes = { "deviceTool" }, IsEnabled = false };
        var content = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4, HorizontalAlignment = HorizontalAlignment.Center,
            Children = { AuroraVisual.Icon(icon, button, 16), new TextBlock { Text = label, FontSize = 11 } } };
        toolContents.Add(content);
        button.Content = content;
        AuroraVisual.Label(button, label, $"{label}尚未开放");
        ToolTip.SetShowOnDisabled(button, true);
        Grid.SetColumn(button, column);
        footer.Children.Add(button);
        var line = new Border { Width = 1, Height = 15, Background = Brush.Parse("#E7EBF3"),
            HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Center, IsHitTestVisible = false };
        Grid.SetColumn(line, column);
        footer.Children.Add(line);
    }

    private static Control Status(Device device)
    {
        var color = Brush.Parse(device.CanConnect ? "#244FC5" : device.Online ? "#98681C" : "#68788D");
        var dot = new Ellipse { Width = 7, Height = 7, Stroke = color, StrokeThickness = 1.15,
            Fill = device.CanConnect ? color : Brushes.Transparent, VerticalAlignment = VerticalAlignment.Center };
        return new Border
        {
            Background = Brush.Parse("#F8FAFD"), CornerRadius = new CornerRadius(6), Padding = new Thickness(7, 4),
            Margin = new Thickness(11), VerticalAlignment = VerticalAlignment.Top, HorizontalAlignment = HorizontalAlignment.Left,
            IsHitTestVisible = false,
            Child = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 5,
                Children = { dot, new TextBlock { Text = device.Online || device.CanConnect ? "在线" : "离线", FontSize = 12,
                    FontWeight = FontWeight.SemiBold, Foreground = device.CanConnect ? color : Brush.Parse("#526176") } } }
        };
    }

    private static string Description(Device device) =>
        device.CanConnect ? "连接桌面" : device.Online ? "在线，未检测到串流" : "设备离线";

    private static List<(string Source, IPAddress Address)> AvailableAddresses(Device device)
    {
        var list = new List<(string, IPAddress)>(2);
        if ((device.LanAddress ?? device.Lan?.Endpoint.Address) is { } lan) list.Add(("局域网", lan));
        var tailnet = device.Tailnet?.Endpoint.Address ?? (device.TailnetVisible ? device.Address : null);
        if (tailnet != null) list.Add(("Tailscale", tailnet));
        return list;
    }

    private static Bitmap LoadCover(string name)
    {
        using var stream = AssetLoader.Open(new Uri($"avares://Aurora/Assets/Wallpapers/{name}"));
        return new Bitmap(stream);
    }
}
