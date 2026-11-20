using System;
using System.Diagnostics;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;

namespace Aurora;

internal sealed class TailscalePage : UserControl, IDisposable
{
    private readonly TextBlock status = new() { FontSize = 17, FontWeight = FontWeight.SemiBold };
    private readonly TextBlock details = new() { TextWrapping = TextWrapping.Wrap };
    private readonly TextBlock feedback = new() { TextWrapping = TextWrapping.Wrap };
    private readonly Button action = new();
    private readonly Button refresh = new() { Content = "刷新状态" };
    private readonly DispatcherTimer timer = new() { Interval = TimeSpan.FromSeconds(3) };
    private TailscaleStatus current = new(TailscaleState.Unknown);
    private bool busy;
    private bool closed;
    private bool confirmInstall;

    public TailscalePage()
    {
        var panel = new StackPanel { Margin = new Thickness(5, 6), Spacing = 12 };
        panel.Children.Add(new TextBlock { Text = "异地连接", FontSize = 23, FontWeight = FontWeight.SemiBold });
        var intro = new TextBlock
        {
            Text = "使用本机的 Tailscale 连接异地设备。同一局域网不需要登录 Tailscale。",
            TextWrapping = TextWrapping.Wrap
        };
        intro.Classes.Add("muted");
        panel.Children.Add(intro);
        var content = new StackPanel { Spacing = 7 };
        content.Children.Add(status);
        content.Children.Add(details);
        panel.Children.Add(new Border { Background = Brushes.White, Padding = new Thickness(14), CornerRadius = new CornerRadius(10), Child = content });
        var actions = new WrapPanel();
        var official = new Button { Content = "官方帮助", Margin = new Thickness(0, 0, 7, 0) };
        foreach (var button in new[] { action, refresh, official }) button.Classes.Add("action");
        action.Classes.Add("accent");
        action.Margin = refresh.Margin = new Thickness(0, 0, 7, 0);
        actions.Children.Add(action);
        actions.Children.Add(refresh);
        actions.Children.Add(official);
        panel.Children.Add(actions);
        feedback.Classes.Add("muted");
        panel.Children.Add(feedback);
        Content = new ScrollViewer { Content = panel };

        var download = OperatingSystem.IsMacOS() ? "https://tailscale.com/download/mac" : "https://tailscale.com/download/windows";
        official.Click += (_, _) => Open("https://tailscale.com/docs/install");
        action.Click += async (_, _) => await Run(async () =>
        {
            switch (current.State)
            {
                case TailscaleState.NotInstalled:
                    if (!OperatingSystem.IsWindows()) { Open(download); return; }
                    if (!confirmInstall)
                    {
                        confirmInstall = true;
                        Render();
                        return;
                    }
                    confirmInstall = false;
                    feedback.Text = "正在下载并启动官方安装器。Windows 可能要求管理员确认。";
                    await Task.Run(Upstream.InstallTailscale);
                    if (closed) return;
                    feedback.Text = "安装流程已结束，正在检查连接状态。";
                    break;
                case TailscaleState.NeedsLogin:
                    if (current.LoginUri == null)
                    {
                        await Task.Run(Upstream.RequestTailscaleConnection);
                        await Read();
                    }
                    if (closed) return;
                    if (current.LoginUri != null)
                    {
                        Open(current.LoginUri.AbsoluteUri);
                        feedback.Text = "请在浏览器完成 Tailscale 登录；极光会自动刷新。";
                    }
                    else if (current.State != TailscaleState.Connected)
                        feedback.Text = "正在等待系统生成登录入口。可以稍后再点「登录 Tailscale」。";
                    return;
                case TailscaleState.Stopped:
                    await Task.Run(Upstream.RequestTailscaleConnection);
                    break;
            }
            if (!closed) await Read();
        }, showErrors: true);
        refresh.Click += async (_, _) => await Run(Read, showErrors: true);
        timer.Tick += async (_, _) => await Run(Read);
        Render();
    }

    public void SetActive(bool active)
    {
        if (active && !timer.IsEnabled)
        {
            timer.Start();
            _ = Run(Read, showErrors: true);
        }
        else if (!active) timer.Stop();
    }

    public void Dispose()
    {
        closed = true;
        timer.Stop();
    }

    private async Task Read()
    {
        try { current = await Task.Run(Upstream.ReadTailscale); }
        catch
        {
            current = new(TailscaleState.Unknown);
            throw;
        }
        finally { if (!closed) Render(); }
    }

    private void Render()
    {
        if (current.State != TailscaleState.NotInstalled) confirmInstall = false;
        (status.Text, details.Text, action.Content) = current.State switch
        {
            TailscaleState.NotInstalled => ("尚未安装 Tailscale",
                confirmInstall ? "将通过 WinGet 启动 Tailscale 官方安装器。" : "安装后，登录与你其他设备相同的账号。",
                OperatingSystem.IsWindows() ? (confirmInstall ? "确认安装" : "安装 Tailscale") : "前往官方下载"),
            TailscaleState.NeedsLogin => ("需要登录", "在 Tailscale 官方页面登录。极光不会接触账号密码。", "登录 Tailscale"),
            TailscaleState.NeedsApproval => ("等待设备批准", "你的 Tailscale 网络需要管理员批准这台设备。批准后会自动更新。", ""),
            TailscaleState.Stopped => ("连接已暂停", "恢复系统客户端的连接，不重置现有设置。", "恢复连接"),
            TailscaleState.Starting => ("正在启动 Tailscale", "稍等片刻，极光会自动刷新。", ""),
            TailscaleState.Connected => ("已连接 Tailscale",
                string.IsNullOrWhiteSpace(current.TailnetName) ? "同账号设备会出现在「我的设备」。" :
                    $"已加入 {current.TailnetName}。同账号设备会出现在「我的设备」。", ""),
            _ => ("暂时无法读取 Tailscale", "请确认系统客户端和后台服务已启动，再刷新状态。", "")
        };
        action.IsVisible = action.Content is string text && text.Length != 0;
    }

    private async Task Run(Func<Task> work, bool showErrors = false)
    {
        if (busy || closed) return;
        busy = true;
        action.IsEnabled = refresh.IsEnabled = false;
        if (showErrors) feedback.Text = "";
        try { await work(); }
        catch (Exception error)
        {
            if (!closed && showErrors) feedback.Text = error.Message;
        }
        finally
        {
            busy = false;
            if (!closed) action.IsEnabled = refresh.IsEnabled = true;
        }
    }

    private void Open(string address)
    {
        try { using var browser = Process.Start(new ProcessStartInfo(address) { UseShellExecute = true }); }
        catch (Exception error) { feedback.Text = "无法打开：" + error.Message; }
    }
}
