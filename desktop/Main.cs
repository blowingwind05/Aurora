using System;
using System.Diagnostics;
using System.Drawing;
using System.Reflection;
using System.Threading.Tasks;
using System.Windows.Forms;

[assembly: AssemblyTitle("Aurora")]
[assembly: AssemblyProduct("Aurora 极光")]
[assembly: AssemblyDescription("Aurora Windows remote desktop")]
[assembly: AssemblyVersion("0.1.0.0")]

namespace Aurora
{
    internal static class Program
    {
        [STAThread]
        private static void Main()
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new AuroraWindow());
        }
    }

    internal sealed class AuroraWindow : Form
    {
        private readonly TableLayoutPanel layout = new TableLayoutPanel();
        private readonly ListView devices = new ListView();
        private readonly Label hostText;
        private readonly Label addressText;
        private readonly Label statusText;
        private readonly Button enableHost;
        private readonly Button settings;
        private readonly Button copyAddress;
        private readonly Button pair;
        private readonly Button stream;
        private readonly Button refresh;
        private readonly Timer clock = new Timer { Interval = 10000 };
        private NetworkSnapshot network;
        private HostStatus host;
        private bool refreshing;
        private bool installing;
        private Process moonlight;

        public AuroraWindow()
        {
            Text = "Aurora 极光";
            StartPosition = FormStartPosition.CenterScreen;
            ClientSize = new Size(790, 520);
            MinimumSize = new Size(680, 470);
            Font = new Font("Microsoft YaHei UI", 10);
            AutoScaleMode = AutoScaleMode.Dpi;
            layout.Dock = DockStyle.Fill;
            layout.ColumnCount = 1;
            layout.RowCount = 8;
            layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            layout.Padding = new Padding(18);
            foreach (var height in new[] { 46, 32, 32, 40, 36 }) layout.RowStyles.Add(new RowStyle(SizeType.Absolute, height));
            layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 40));
            layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 37));
            Controls.Add(layout);

            var title = AddLabel("极光 · 远程桌面", 0);
            title.Font = new Font(Font.FontFamily, 19, FontStyle.Bold);
            hostText = AddLabel("本机被控：检查中…", 1);
            addressText = AddLabel("本机 Tailscale 地址：检查中…", 2);
            var hostActions = AddActions(3);
            enableHost = AddButton(hostActions, "启用本机被控", Install);
            settings = AddButton(hostActions, "打开 Sunshine 设置 / 确认 PIN", delegate { OpenPage(Upstream.SettingsUrl); });
            copyAddress = AddButton(hostActions, "复制地址", CopyAddress);
            AddLabel("我的设备  ·  通过 Tailscale 自动发现", 4);

            devices.Dock = DockStyle.Fill;
            devices.View = View.Details;
            devices.FullRowSelect = true;
            devices.MultiSelect = false;
            devices.HideSelection = false;
            devices.Columns.Add("设备", 220);
            devices.Columns.Add("系统", 100);
            devices.Columns.Add("Tailscale 状态", 130);
            devices.Columns.Add("Tailscale 地址", 245);
            devices.SelectedIndexChanged += delegate { UpdateActions(); };
            devices.DoubleClick += delegate { Connect(false); };
            layout.Controls.Add(devices, 0, 5);

            var actions = AddActions(6);
            pair = AddButton(actions, "配对", delegate { Connect(true); });
            stream = AddButton(actions, "打开远程桌面", delegate { Connect(false); });
            refresh = AddButton(actions, "立即刷新", async delegate { await RefreshDevices(); });
            statusText = AddLabel("正在读取 Tailscale…", 7);
            UpdateActions();
            Shown += async delegate { clock.Start(); await RefreshDevices(); };
            clock.Tick += async delegate { await RefreshDevices(); };
            FormClosed += delegate
            {
                clock.Dispose();
                if (moonlight != null) moonlight.Dispose();
            };
        }

        private Label AddLabel(string text, int row)
        {
            var label = new Label { Text = text, Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft, AutoEllipsis = true };
            layout.Controls.Add(label, 0, row);
            return label;
        }

        private FlowLayoutPanel AddActions(int row)
        {
            var panel = new FlowLayoutPanel { Dock = DockStyle.Fill, WrapContents = false };
            layout.Controls.Add(panel, 0, row);
            return panel;
        }

        private static Button AddButton(FlowLayoutPanel panel, string text, EventHandler action)
        {
            var button = new Button { Text = text, AutoSize = true, MinimumSize = new Size(80, 30), Padding = new Padding(8, 0, 8, 0) };
            button.Click += action;
            panel.Controls.Add(button);
            return button;
        }

        private Device SelectedDevice
        {
            get { return devices.SelectedItems.Count == 0 ? null : (Device)devices.SelectedItems[0].Tag; }
        }

        private void UpdateActions()
        {
            var device = SelectedDevice;
            pair.Enabled = stream.Enabled = moonlight == null && device != null && device.Online &&
                string.Equals(device.Platform, "windows", StringComparison.OrdinalIgnoreCase);
            refresh.Enabled = !refreshing;
            enableHost.Enabled = !installing && host != null && !host.Installed;
            settings.Enabled = !installing && host != null && host.SettingsAvailable;
            copyAddress.Enabled = network != null && network.LocalAddress != null;
        }

        private async Task RefreshDevices()
        {
            if (refreshing || IsDisposed) return;
            refreshing = true;
            UpdateActions();
            var networkTask = Task.Run((Func<NetworkSnapshot>)Upstream.ReadNetwork);
            var hostTask = Task.Run((Func<HostStatus>)Upstream.ReadHost);
            string networkError = null;
            string hostError = null;
            try { network = await networkTask; }
            catch (Exception error) { network = null; networkError = error.Message; }
            try { host = await hostTask; }
            catch (Exception error) { host = null; hostError = error.Message; }
            if (IsDisposed) return;
            refreshing = false;

            var selected = SelectedDevice;
            devices.BeginUpdate();
            try
            {
                devices.Items.Clear();
                if (network != null)
                {
                    foreach (var device in network.Devices)
                    {
                        var item = new ListViewItem(new[] { device.Name, device.Platform, device.Online ? "在线" : "离线", device.Address.ToString() })
                        {
                            Tag = device
                        };
                        devices.Items.Add(item);
                        if (selected != null && selected.Id == device.Id) item.Selected = true;
                    }
                }
            }
            finally { devices.EndUpdate(); }
            addressText.Text = network != null && network.LocalAddress != null
                ? "本机 Tailscale 地址：" + network.LocalAddress
                : "本机 Tailscale 地址：不可用";
            hostText.Text = installing ? "本机被控：正在安装 Sunshine…" :
                hostError != null ? "本机被控：检测失败。" :
                host.SettingsAvailable ? "本机被控：可打开设置，首次使用请创建账号并确认 PIN。" :
                host.Installed ? "本机被控：Sunshine 已安装，但本机设置未就绪。" :
                "本机被控：尚未启用，可从极光安装 Sunshine。";
            statusText.Text = networkError != null ? "自动发现失败：" + networkError :
                devices.Items.Count == 0 ? "暂未发现同账号设备，会自动重试。" :
                "已发现 " + devices.Items.Count + " 台设备。在线不代表已启用远控；当前仅支持连接 Windows。";
            UpdateActions();
        }

        private void OpenPage(string address)
        {
            try { Process.Start(new ProcessStartInfo(address) { UseShellExecute = true }); }
            catch (Exception error) { ShowError(error.Message); }
        }

        private void CopyAddress(object sender, EventArgs args)
        {
            try { Clipboard.SetText(network.LocalAddress.ToString()); }
            catch (Exception error) { ShowError(error.Message); }
        }

        private async void Install(object sender, EventArgs args)
        {
            if (installing || host == null || host.Installed) return;
            if (MessageBox.Show(this, "将通过 Windows 应用安装程序下载并安装 Sunshine。安装可能需要管理员确认。是否继续？",
                "启用本机被控", MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;
            installing = true;
            hostText.Text = "本机被控：正在安装 Sunshine…";
            UpdateActions();
            try { await Task.Run((Action)Upstream.InstallSunshine); }
            catch (Exception error)
            {
                if (!IsDisposed && MessageBox.Show(this, "安装未完成：" + error.Message + "\n\n打开 Sunshine 官方发布页？",
                    "极光", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) == DialogResult.Yes) OpenPage(Upstream.ReleasesUrl);
            }
            installing = false;
            await RefreshDevices();
        }

        private async void Connect(bool pairing)
        {
            if (!stream.Enabled) return;
            try
            {
                var process = Upstream.LaunchMoonlight(SelectedDevice, pairing);
                moonlight = process;
                UpdateActions();
                var exit = await Task.Run(delegate { process.WaitForExit(); return process.ExitCode; });
                if (!IsDisposed && exit != 0) ShowError("Moonlight 操作未完成（退出码 " + exit + "）。");
            }
            catch (Exception error) { if (!IsDisposed) ShowError(error.Message); }
            finally
            {
                if (moonlight != null) moonlight.Dispose();
                moonlight = null;
                if (!IsDisposed) UpdateActions();
            }
        }

        private void ShowError(string message)
        {
            MessageBox.Show(this, message, "极光", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
    }
}
