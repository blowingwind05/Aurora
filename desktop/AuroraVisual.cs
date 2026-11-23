using System;
using System.Globalization;
using System.Linq;
using System.Xml.Linq;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Controls.Shapes;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Platform;

namespace Aurora;

internal static class AuroraVisual
{
    // Lucide 1.77.0 SVG geometry from assets/icons/lucide in Aurora Visual Design.
    // SVG rectangles, lines and circles are expressed as equivalent path segments.
    internal const string Monitor =
        "M4 3H20a2 2 0 0 1 2 2V15a2 2 0 0 1 -2 2H4a2 2 0 0 1 -2 -2V5a2 2 0 0 1 2 -2Z " +
        "M8 21L16 21 " +
        "M12 17L12 21";
    internal const string Panel = "M5 3H19a2 2 0 0 1 2 2V19a2 2 0 0 1 -2 2H5a2 2 0 0 1 -2 -2V5a2 2 0 0 1 2 -2Z M3 9h18";
    internal const string Gear =
        "M9.671 4.136a2.34 2.34 0 0 1 4.659 0 2.34 2.34 0 0 0 3.319 1.915 2.34 2.34 0 0 1 2.33 4.033 2.34 2.34 0 0 0 0 3.831 2.34 2.34 0 0 1-2.33 4.033 2.34 2.34 0 0 0-3.319 1.915 2.34 2.34 0 0 1-4.659 0 2.34 2.34 0 0 0-3.32-1.915 2.34 2.34 0 0 1-2.33-4.033 2.34 2.34 0 0 0 0-3.831A2.34 2.34 0 0 1 6.35 6.051a2.34 2.34 0 0 0 3.319-1.915 " +
        "M15 12a3 3 0 1 1 -6 0a3 3 0 1 1 6 0Z";
    internal const string Refresh =
        "M3 12a9 9 0 0 1 9-9 9.75 9.75 0 0 1 6.74 2.74L21 8 " +
        "M21 3v5h-5 " +
        "M21 12a9 9 0 0 1-9 9 9.75 9.75 0 0 1-6.74-2.74L3 16 " +
        "M8 16H3v5";
    internal const string RotateCcw = "M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8 M3 3v5h5";
    internal const string Power = "M12 2v10 M18.4 6.6a9 9 0 1 1-12.77.04";
    internal const string Copy =
        "M10 8H20a2 2 0 0 1 2 2V20a2 2 0 0 1 -2 2H10a2 2 0 0 1 -2 -2V10a2 2 0 0 1 2 -2Z " +
        "M4 16c-1.1 0-2-.9-2-2V4c0-1.1.9-2 2-2h10c1.1 0 2 .9 2 2";
    internal const string FolderSync =
        "M9 20H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h3.9a2 2 0 0 1 1.69.9l.81 1.2a2 2 0 0 0 1.67.9H20a2 2 0 0 1 2 2v.5 " +
        "M12 10v4h4 " +
        "M12 14l1.535-1.605a5 5 0 0 1 8 1.5 " +
        "M22 22v-4h-4 " +
        "M22 18l-1.535 1.605a5 5 0 0 1-8-1.5";
    internal const string SquarePlay =
        "M5 3H19a2 2 0 0 1 2 2V19a2 2 0 0 1 -2 2H5a2 2 0 0 1 -2 -2V5a2 2 0 0 1 2 -2Z " +
        "M9 9.003a1 1 0 0 1 1.517-.859l4.997 2.997a1 1 0 0 1 0 1.718l-4.997 2.997A1 1 0 0 1 9 14.996z";
    internal const string SquareTerminal =
        "m7 11 2-2-2-2 " +
        "M11 13h4 " +
        "M5 3H19a2 2 0 0 1 2 2V19a2 2 0 0 1 -2 2H5a2 2 0 0 1 -2 -2V5a2 2 0 0 1 2 -2Z";
    internal const string PanelsTopLeft =
        "M5 3H19a2 2 0 0 1 2 2V19a2 2 0 0 1 -2 2H5a2 2 0 0 1 -2 -2V5a2 2 0 0 1 2 -2Z " +
        "M3 9h18 " +
        "M9 21V9";
    internal const string Grid2X2 =
        "M12 3v18 " +
        "M3 12h18 " +
        "M5 3H19a2 2 0 0 1 2 2V19a2 2 0 0 1 -2 2H5a2 2 0 0 1 -2 -2V5a2 2 0 0 1 2 -2Z";
    internal const string ArrowLeft = "m12 19-7-7 7-7 M19 12H5";
    internal const string Menu = "M4 6h16 M4 12h16 M4 18h16";
    internal const string LoaderCircle = "M21 12a9 9 0 1 1-9-9";
    internal const string ArrowRight = "M5 12h14 M12 5l7 7-7 7";
    internal const string ChevronRight = "m9 18 6-6-6-6";
    internal const string ChevronDown = "m6 9 6 6 6-6";
    internal const string Minus = "M5 12h14";
    internal const string Square = "M5 3H19a2 2 0 0 1 2 2V19a2 2 0 0 1 -2 2H5a2 2 0 0 1 -2 -2V5a2 2 0 0 1 2 -2Z";
    internal const string RestoreWindow = "M8 8V4H20V16H16 M4 8H16V20H4Z";
    internal const string Close = "M18 6 6 18 M6 6l12 12";

    private static readonly Lazy<(Rect ViewBox, Geometry Outline, double StrokeWidth)> HostSvg = new(() =>
    {
        var svg = ReadSvg("panel-top-tight.svg");
        var rect = svg.Element(svg.Name.Namespace + "rect");
        var radius = SvgNumber(rect.Attribute("rx"));
        var outline = new GeometryGroup
        {
            Children =
            {
                new RectangleGeometry(new Rect(SvgNumber(rect.Attribute("x")), SvgNumber(rect.Attribute("y")),
                    SvgNumber(rect.Attribute("width")), SvgNumber(rect.Attribute("height"))), radius, radius),
                Geometry.Parse((string)svg.Element(svg.Name.Namespace + "path").Attribute("d"))
            }
        };
        return (SvgViewBox(svg), outline, SvgNumber(svg.Attribute("stroke-width")));
    });

    private static readonly Lazy<(Rect ViewBox, IBrush Fill, (Geometry Dot, double Opacity)[] Paths)> TailnetSvg = new(() =>
    {
        var svg = ReadSvg("tailscale-dot.svg");
        var paths = svg.Elements(svg.Name.Namespace + "path")
            .Select(path => (Geometry.Parse((string)path.Attribute("d")),
                path.Attribute("opacity") is { } opacity ? SvgNumber(opacity) : 1)).ToArray();
        return (SvgViewBox(svg), Brush.Parse((string)svg.Attribute("fill")), paths);
    });

    internal static Viewbox Icon(string geometry, Button owner = null, double size = 18,
        double strokeThickness = 2, double scale = 1)
    {
        var path = new Path
        {
            Data = Geometry.Parse(geometry),
            Stroke = Brush.Parse("#19283F"),
            StrokeThickness = strokeThickness,
            StrokeLineCap = PenLineCap.Round,
            StrokeJoin = PenLineJoin.Round,
            Stretch = Stretch.None,
            IsHitTestVisible = false
        };
        if (owner != null)
            path[!Shape.StrokeProperty] = owner[!Button.ForegroundProperty];

        var canvas = new Canvas { Width = 24, Height = 24, IsHitTestVisible = false };
        canvas.Children.Add(path);
        var icon = new Viewbox { Width = size, Height = size, Child = canvas, IsHitTestVisible = false };
        // Scaling the visual leaves the navigation slot and text spacing unchanged.
        if (scale != 1)
        {
            icon.RenderTransformOrigin = RelativePoint.Center;
            icon.RenderTransform = new ScaleTransform(scale, scale);
        }
        return icon;
    }

    internal static Viewbox SetTextAction(Button button, string label, string geometry, bool leading = false)
    {
        var icon = Icon(geometry, button, size: 16, strokeThickness: 2);
        var text = new TextBlock { Text = label, VerticalAlignment = VerticalAlignment.Center };
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 5, VerticalAlignment = VerticalAlignment.Center };
        row.Children.Add(leading ? (Control)icon : text);
        row.Children.Add(leading ? (Control)text : icon);
        button.Content = row;
        AutomationProperties.SetName(button, label);
        return icon;
    }

    internal static void Label(Button button, string name, string tooltip = null)
    {
        AutomationProperties.SetName(button, name);
        ToolTip.SetTip(button, tooltip ?? name);
    }

    internal static ToolTip LabelIcon(Button button, string name, double horizontalOffset = 0)
    {
        var tooltip = new ToolTip { Classes = { "iconTip" }, Content = name };
        AutomationProperties.SetName(button, name);
        ToolTip.SetTip(button, tooltip);
        ToolTip.SetPlacement(button, PlacementMode.Top);
        ToolTip.SetHorizontalOffset(button, horizontalOffset);
        ToolTip.SetVerticalOffset(button, 16);
        ToolTip.SetShowDelay(button, 700);
        ToolTip.SetBetweenShowDelay(button, 300);
        ToolTip.SetShowOnDisabled(button, false);
        button.GotFocus += (_, args) =>
        {
            if (args.NavigationMethod is NavigationMethod.Tab or NavigationMethod.Directional)
                ToolTip.SetIsOpen(button, true);
        };
        button.LostFocus += (_, _) =>
        {
            if (!button.IsPointerOver) ToolTip.SetIsOpen(button, false);
        };
        button.PropertyChanged += (_, args) =>
        {
            if (args.Property == InputElement.IsEffectivelyEnabledProperty && !button.IsEffectivelyEnabled)
                ToolTip.SetIsOpen(button, false);
        };
        return tooltip;
    }

    internal static Control HostMark(double size = 28)
    {
        var (viewBox, outline, strokeWidth) = HostSvg.Value;
        var mark = new Path
        {
            Data = outline,
            Stroke = Brush.Parse("#19283F"),
            StrokeThickness = strokeWidth,
            StrokeLineCap = PenLineCap.Round,
            StrokeJoin = PenLineJoin.Round
        };
        var icon = SvgIcon(viewBox, size, out var canvas);
        Canvas.SetLeft(mark, -viewBox.X);
        Canvas.SetTop(mark, -viewBox.Y);
        canvas.Children.Add(mark);
        return icon;
    }

    internal static Control TailnetMark(Button owner = null, double size = 18)
    {
        var (viewBox, foreground, paths) = TailnetSvg.Value;
        var icon = SvgIcon(viewBox, size, out var canvas);
        foreach (var (dot, opacity) in paths)
        {
            var mark = new Path { Data = dot, Fill = foreground, Opacity = opacity };
            if (owner != null)
                mark[!Shape.FillProperty] = owner[!Button.ForegroundProperty];
            canvas.Children.Add(mark);
        }
        return icon;
    }

    private static Viewbox SvgIcon(Rect viewBox, double size, out Canvas canvas)
    {
        canvas = new Canvas { Width = viewBox.Width, Height = viewBox.Height };
        return new Viewbox
        {
            Width = size,
            Height = size,
            VerticalAlignment = VerticalAlignment.Center,
            Child = canvas,
            IsHitTestVisible = false
        };
    }

    private static XElement ReadSvg(string asset)
    {
        using var stream = AssetLoader.Open(new Uri($"avares://Aurora/Assets/{asset}"));
        return XDocument.Load(stream).Root;
    }

    private static Rect SvgViewBox(XElement svg) => Rect.Parse((string)svg.Attribute("viewBox"));

    private static double SvgNumber(XAttribute attribute) => double.Parse(attribute.Value, CultureInfo.InvariantCulture);
}
