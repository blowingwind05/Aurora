using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Runtime.Versioning;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Win32.SafeHandles;

namespace Aurora;

internal sealed class SunshineCertificateAccessException()
    : InvalidOperationException("普通权限读不到 Sunshine 证书。请先授权读取公开证书指纹。");

[SupportedOSPlatform("windows")]
internal static class SunshineTrust
{
    public static string Scope(string certificatePath, int port) =>
        Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(port + "/" + certificatePath.ToUpperInvariant())));

    private static string PinPath(string scope) => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Aurora", "Sunshine", scope + ".sha256");

    public static byte[] ReadPin(string scope)
    {
        var path = PinPath(scope);
        try
        {
            var text = File.ReadAllText(path).Trim();
            return text.Length == 64 ? Convert.FromHexString(text) : null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or FormatException) { return null; }
    }

    public static void SavePin(string scope, byte[] pin)
    {
        var path = PinPath(scope);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, Convert.ToHexString(pin));
    }

    public static async Task<byte[]> ReadElevated()
    {
        var name = "aurora-sunshine-" + Guid.NewGuid().ToString("N");
        using var identity = WindowsIdentity.GetCurrent();
        var security = new PipeSecurity();
        security.AddAccessRule(new PipeAccessRule(identity.User!, PipeAccessRights.FullControl, AccessControlType.Allow));
        security.AddAccessRule(new PipeAccessRule(new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null),
            PipeAccessRights.Write | PipeAccessRights.ReadPermissions | PipeAccessRights.Synchronize, AccessControlType.Allow));
        using var pipe = NamedPipeServerStreamAcl.Create(name, PipeDirection.In, 1, PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous, 0, 0, security);
        using var source = Assembly.GetExecutingAssembly().GetManifestResourceStream("Aurora.ReadSunshineCertificate.ps1")
            ?? throw new InvalidOperationException("缺少 Sunshine 证书读取组件。");
        using var reader = new StreamReader(source);
        var script = (await reader.ReadToEndAsync()).Replace("__PIPE_NAME__", name, StringComparison.Ordinal);
        var powershell = Path.Combine(Environment.SystemDirectory, @"WindowsPowerShell\v1.0\powershell.exe");
        using var process = await Task.Run(() => Process.Start(new ProcessStartInfo(powershell,
            "-NoProfile -NonInteractive -EncodedCommand " + Convert.ToBase64String(Encoding.Unicode.GetBytes(script)))
        {
            UseShellExecute = true, Verb = "runas", WindowStyle = ProcessWindowStyle.Hidden
        })) ?? throw new InvalidOperationException("无法启动证书读取。");
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(12));
        try
        {
            await pipe.WaitForConnectionAsync(timeout.Token);
            if (!GetNamedPipeClientProcessId(pipe.SafePipeHandle, out var pid) || pid != (uint)process.Id)
                throw new InvalidOperationException("无法验证证书读取进程。没有导入指纹。");
            var hash = new byte[64];
            await pipe.ReadExactlyAsync(hash, timeout.Token);
            await process.WaitForExitAsync(timeout.Token);
            if (process.ExitCode != 0) throw new InvalidOperationException("未能读取可验证的 Sunshine 公开证书。");
            return Convert.FromHexString(Encoding.ASCII.GetString(hash));
        }
        catch (Exception error) when (error is OperationCanceledException or IOException or FormatException)
        {
            throw new InvalidOperationException("未能导入公开证书。请确认管理员提示，并检查 Sunshine 服务和证书。", error);
        }
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetNamedPipeClientProcessId(SafePipeHandle pipe, out uint clientProcessId);
}
