using Avalonia;
using Avalonia.Controls.Primitives;
using Avalonia.Controls.Templates;
using Avalonia.Data.Converters;
using Avalonia.Media;

namespace Aurora;

internal sealed class ActionLabel : AccessText
{
    private static readonly IDataTemplate Template = new FuncDataTemplate<string>((text, _) => new ActionLabel { Text = text });

    public static readonly IValueConverter TemplateForContent = new FuncValueConverter<object, IDataTemplate>(
        content => content is string ? Template : null);

    protected override void RenderTextLayout(DrawingContext context, Point origin)
    {
        var layout = TextLayout;
        if (layout.Extent > 0 && double.IsFinite(layout.Extent))
        {
            // TextBlock normally draws at the top of its stretched content box.
            // Center the ink using the shaped glyph bounds while preserving the layout and access key.
            var inkCenter = layout.Height + layout.OverhangAfter - layout.Extent / 2;
            origin = new Point(origin.X, (Bounds.Height + Padding.Top - Padding.Bottom) / 2 - inkCenter);
        }
        base.RenderTextLayout(context, origin);
    }
}
