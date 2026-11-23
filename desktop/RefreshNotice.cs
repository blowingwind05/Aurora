#nullable enable

using System;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Animation;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Styling;
using Avalonia.Threading;
using Avalonia.VisualTree;

namespace Aurora;

internal sealed class RefreshNotice : Decorator
{
    private const int MotionMs = 240;
    private const int FadeMs = 160;
    private const int FadeInDelayMs = 40;

    private static readonly StyledProperty<double> OpenFractionProperty =
        AvaloniaProperty.Register<RefreshNotice, double>("OpenFraction");

    private readonly DispatcherTimer expiration = new() { Interval = TimeSpan.FromSeconds(3) };
    private CancellationTokenSource? motion;

    static RefreshNotice() => AffectsMeasure<RefreshNotice>(OpenFractionProperty);

    public RefreshNotice()
    {
        ClipToBounds = true;
        VerticalAlignment = VerticalAlignment.Top;
        IsVisible = false;
        Opacity = 0;
        expiration.Tick += (_, _) => Clear(animate: true);
    }

    internal string? Message { get; private set; }

    internal void ShowPersistent(string? message) => Show(message, timed: false);

    internal void ShowRefreshSuccess(string? message) => Show(message, timed: true);

    internal void Clear(bool animate = false)
    {
        expiration.Stop();
        var wasOpen = Message != null;
        Message = null;

        if (!animate || !CanAnimate())
        {
            CancelMotion();
            Finish(open: false);
        }
        else if (wasOpen)
        {
            StartMotion(open: false);
        }
    }

    protected override Size MeasureOverride(Size availableSize)
    {
        if (Child is not { } child) return default;

        // Measure wrapped text at its natural height while the slot clips the reveal.
        child.Measure(new Size(availableSize.Width, double.PositiveInfinity));
        var size = child.DesiredSize;
        return new Size(size.Width, size.Height * GetValue(OpenFractionProperty));
    }

    protected override Size ArrangeOverride(Size finalSize)
    {
        if (Child is { } child)
            child.Arrange(new Rect(0, 0, finalSize.Width, child.DesiredSize.Height));

        return finalSize;
    }

    protected override void OnDetachedFromVisualTree(VisualTreeAttachmentEventArgs e)
    {
        Clear();
        base.OnDetachedFromVisualTree(e);
    }

    private void Show(string? message, bool timed)
    {
        if (string.IsNullOrWhiteSpace(message))
        {
            Clear(animate: true);
            return;
        }

        if (Child is not TextBlock text)
            throw new InvalidOperationException("RefreshNotice requires a TextBlock child.");

        expiration.Stop();
        var wasOpen = Message != null;
        Message = message;
        if (AutomationProperties.GetLiveSetting(text) == AutomationLiveSetting.Off)
            AutomationProperties.SetLiveSetting(text, AutomationLiveSetting.Polite);

        IsVisible = true;
        text.Text = message;
        if (!CanAnimate())
        {
            CancelMotion();
            Finish(open: true);
        }
        else if (!wasOpen)
        {
            StartMotion(open: true);
        }

        if (timed) expiration.Start();
    }

    private bool CanAnimate() =>
        IsEffectivelyVisible && this.IsAttachedToVisualTree() && !ReduceMotion();

    private void StartMotion(bool open)
    {
        var height = GetValue(OpenFractionProperty);
        var opacity = Opacity;
        CancelMotion();

        var target = open ? 1d : 0d;
        if (height == target && opacity == target)
        {
            Finish(open);
            return;
        }

        SetValue(OpenFractionProperty, height);
        Opacity = opacity;
        var animation = new Animation { Duration = TimeSpan.FromMilliseconds(MotionMs), FillMode = FillMode.Forward };
        animation.Children.Add(Frame(0, OpenFractionProperty, height));
        animation.Children.Add(Frame(MotionMs, OpenFractionProperty, target, new KeySpline(.25, .7, .25, 1)));
        animation.Children.Add(Frame(0, OpacityProperty, opacity));
        var fadeDelay = open ? FadeInDelayMs : 0;
        if (fadeDelay > 0)
            animation.Children.Add(Frame(fadeDelay, OpacityProperty, opacity));
        animation.Children.Add(Frame(fadeDelay + FadeMs, OpacityProperty, target, new KeySpline(.25, .1, .25, 1)));
        animation.Children.Add(Frame(MotionMs, OpacityProperty, target));

        var run = new CancellationTokenSource();
        motion = run;
        _ = RunMotion(animation, open, run);
    }

    private static KeyFrame Frame(int milliseconds, AvaloniaProperty property, double value, KeySpline? easing = null)
    {
        var frame = new KeyFrame
        {
            KeyTime = TimeSpan.FromMilliseconds(milliseconds),
            Setters = { new Setter(property, value) }
        };
        if (easing != null) frame.KeySpline = easing;
        return frame;
    }

    private async Task RunMotion(Animation animation, bool open, CancellationTokenSource run)
    {
        using (run)
        {
            try
            {
                await animation.RunAsync(this, run.Token);
            }
            finally
            {
                if (ReferenceEquals(motion, run))
                {
                    motion = null;
                    Finish(open);
                }
            }
        }
    }

    private void CancelMotion()
    {
        var previous = motion;
        motion = null;
        previous?.Cancel();
    }

    private void Finish(bool open)
    {
        SetValue(OpenFractionProperty, open ? 1d : 0d);
        Opacity = open ? 1d : 0d;
        IsVisible = open;
        if (!open && Child is TextBlock text) text.Text = null;
    }

    internal static bool ReduceMotion()
    {
        if (OperatingSystem.IsWindows())
        {
            const uint spiGetClientAreaAnimation = 0x1042;
            return SystemParametersInfo(spiGetClientAreaAnimation, 0, out var enabled, 0) != 0 && enabled == 0;
        }

        if (OperatingSystem.IsMacOS())
        {
            var workspace = SendObject(GetClass("NSWorkspace"), RegisterSelector("sharedWorkspace"));
            var reduceMotion = RegisterSelector("accessibilityDisplayShouldReduceMotion");
            return RespondsToSelector(workspace, RegisterSelector("respondsToSelector:"), reduceMotion) != 0
                && SendBool(workspace, reduceMotion) != 0;
        }

        return false;
    }

    [DllImport("user32.dll", EntryPoint = "SystemParametersInfoW", ExactSpelling = true)]
    private static extern int SystemParametersInfo(uint action, uint parameter, out int value, uint flags);

    private const string ObjectiveC = "/usr/lib/libobjc.A.dylib";

    [DllImport(ObjectiveC, EntryPoint = "objc_getClass")]
    private static extern IntPtr GetClass(string name);

    [DllImport(ObjectiveC, EntryPoint = "sel_registerName")]
    private static extern IntPtr RegisterSelector(string name);

    [DllImport(ObjectiveC, EntryPoint = "objc_msgSend")]
    private static extern IntPtr SendObject(IntPtr receiver, IntPtr selector);

    [DllImport(ObjectiveC, EntryPoint = "objc_msgSend")]
    private static extern byte SendBool(IntPtr receiver, IntPtr selector);

    [DllImport(ObjectiveC, EntryPoint = "objc_msgSend")]
    private static extern byte RespondsToSelector(IntPtr receiver, IntPtr selector, IntPtr argument);
}
