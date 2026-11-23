using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Net.Http;
using System.Security.Cryptography;
using System.Threading;
using System.Threading.Tasks;

namespace Aurora;

internal enum GamepadInstallResult { Installed, RestartRequired, Canceled }

internal static class ViGEmBus
{
    private const string ServiceName = "ViGEmBus";
    private const string InstallerName = "ViGEmBus_1.22.0_x64_x86_arm64.exe";
    private const string InstallerUrl = "https://github.com/nefarius/ViGEmBus/releases/download/v1.22.0/" + InstallerName;
    // Fixed upstream release, independently recorded in the ViGEm.ViGEmBus WinGet manifest.
    private const string InstallerSha256 = "89220A7865076B342892F98865F3499FB7C4CFD673159E89D352C360FD014C6A";
    private const int InstallerSize = 6278576;
    private static readonly HttpClient Http = new();

    internal static WindowsServiceState ReadState() => WindowsService.Read(ServiceName);

    internal static async Task<GamepadInstallResult> InstallAsync(IProgress<string> progress)
    {
        if (!NeedsInstallation()) return GamepadInstallResult.Installed;

        var directory = Directory.CreateTempSubdirectory("aurora-vigembus-");
        var installer = Path.Combine(directory.FullName, InstallerName);
        try
        {
            var bundledInstaller = Path.Combine(AppContext.BaseDirectory, "Components", "ViGEmBus", InstallerName);
            if (File.Exists(bundledInstaller))
            {
                progress.Report("正在校验随包附带的 ViGEmBus…");
                File.Copy(bundledInstaller, installer);
            }
            else
            {
                progress.Report("正在下载 ViGEmBus…");
                using var timeout = new CancellationTokenSource(TimeSpan.FromMinutes(2));
                await DownloadInstaller(installer, timeout.Token).ConfigureAwait(false);
            }

            using var file = File.Open(installer, FileMode.Open, FileAccess.Read, FileShare.Read);
            var sha256 = await SHA256.HashDataAsync(file).ConfigureAwait(false);
            if (!CryptographicOperations.FixedTimeEquals(sha256, Convert.FromHexString(InstallerSha256)))
                throw new InvalidDataException("ViGEmBus 安装包校验失败，请重试。");

            if (!NeedsInstallation()) return GamepadInstallResult.Installed;
            progress.Report("正在安装 ViGEmBus，请确认 Windows 管理员提示。");
            using var process = Process.Start(new ProcessStartInfo
            {
                FileName = installer,
                WorkingDirectory = directory.FullName,
                Arguments = "/exenoui /qn /norestart",
                UseShellExecute = true,
                Verb = "runas"
            }) ?? throw new InvalidOperationException("无法打开 ViGEmBus 安装程序。");
            await process.WaitForExitAsync().ConfigureAwait(false);
            switch (process.ExitCode)
            {
                case 0 when !NeedsInstallation():
                    return GamepadInstallResult.Installed;
                case 0:
                    throw new InvalidOperationException("安装程序已结束，但 Windows 尚未检测到 ViGEmBus。");
                case 1641 or 3010:
                    return GamepadInstallResult.RestartRequired;
                case 1223 or 1602:
                    return GamepadInstallResult.Canceled;
                default:
                    throw new InvalidOperationException($"ViGEmBus 安装失败（{process.ExitCode}）。");
            }
        }
        catch (Win32Exception error) when (error.NativeErrorCode == 1223)
        {
            return GamepadInstallResult.Canceled;
        }
        catch (OperationCanceledException)
        {
            throw new TimeoutException("ViGEmBus 下载超时，请重试。");
        }
        catch (HttpRequestException error)
        {
            throw new IOException("无法从 GitHub 下载 ViGEmBus，请检查网络后重试。", error);
        }
        finally
        {
            try
            {
                File.Delete(installer);
                directory.Delete();
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException)
            {
                Debug.WriteLine(error);
            }
        }
    }

    private static bool NeedsInstallation() => ReadState() switch
    {
        WindowsServiceState.Missing => true,
        WindowsServiceState.Unknown => throw new InvalidOperationException("无法确认 Windows 的手柄驱动状态。"),
        _ => false
    };

    private static async Task DownloadInstaller(string destination, CancellationToken token)
    {
        using var response = await Http.GetAsync(InstallerUrl, HttpCompletionOption.ResponseHeadersRead, token).ConfigureAwait(false);
        response.EnsureSuccessStatusCode();
        await using var source = await response.Content.ReadAsStreamAsync(token).ConfigureAwait(false);
        await using var output = new FileStream(destination, FileMode.CreateNew, FileAccess.Write, FileShare.None);
        var buffer = new byte[64 * 1024];
        var size = 0;
        int count;
        while ((count = await source.ReadAsync(buffer, token).ConfigureAwait(false)) != 0)
        {
            size += count;
            if (size > InstallerSize) throw new InvalidDataException("ViGEmBus 安装包大小异常。");
            await output.WriteAsync(buffer.AsMemory(0, count), token).ConfigureAwait(false);
        }
        if (size != InstallerSize) throw new InvalidDataException("ViGEmBus 安装包下载不完整。");
    }
}
