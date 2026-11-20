using System;
using System.Buffers.Binary;
using System.IO;
using System.IO.Pipes;
using System.Net;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Win32.SafeHandles;

namespace Aurora;

internal static class TailscaleLoopback
{
    private const int TcpOwnerPidConnections = 4;
    private const int TcpTableRowSize = 24;
    private const string ProtectedTailscalePipe = @"ProtectedPrefix\Administrators\Tailscale\tailscaled";

    // Serve sends a PROXY header over loopback. Only the Windows Tailscale service may assert that identity.
    public static async Task<bool> IsFromService(IPEndPoint source, IPEndPoint destination, CancellationToken token)
    {
        if (!OperatingSystem.IsWindows() || !source.Address.Equals(IPAddress.Loopback) ||
            !destination.Address.Equals(IPAddress.Loopback)) return false;
        // The service supervisor starts a separate daemon, which owns both the LocalAPI pipe and Serve's sockets.
        // Windows permits only administrators to create pipes under this prefix.
        using var pipe = new NamedPipeClientStream(".", ProtectedTailscalePipe, PipeDirection.InOut,
            PipeOptions.Asynchronous, TokenImpersonationLevel.Identification);
        try { await pipe.ConnectAsync(750, token); }
        catch (Exception error) when (error is TimeoutException or IOException or UnauthorizedAccessException) { return false; }
        return GetNamedPipeServerProcessId(pipe.SafePipeHandle, out var daemonPid) && daemonPid > 0 &&
            HasLoopbackConnection(daemonPid, source.Port, destination.Port);
    }

    internal static bool HasLoopbackConnection(int expectedPid, int sourcePort, int destinationPort)
    {
        var size = 0;
        for (var attempt = 0; attempt < 3; attempt++)
        {
            if (size > 4 * 1024 * 1024) return false;
            var buffer = new byte[size];
            var result = GetExtendedTcpTable(buffer, ref size, false, 2, TcpOwnerPidConnections, 0);
            if (result == 122) continue; // ERROR_INSUFFICIENT_BUFFER
            if (result != 0 || size < sizeof(int) || size > buffer.Length) return false;
            var count = BinaryPrimitives.ReadInt32LittleEndian(buffer);
            if (count < 0 || count > (size - sizeof(int)) / TcpTableRowSize) return false;
            for (var index = 0; index < count; index++)
            {
                var row = buffer.AsSpan(sizeof(int) + index * TcpTableRowSize, TcpTableRowSize);
                if (BinaryPrimitives.ReadInt32LittleEndian(row[20..]) == expectedPid &&
                    row[4..8].SequenceEqual("\x7f\0\0\x01"u8) &&
                    row[12..16].SequenceEqual("\x7f\0\0\x01"u8) &&
                    BinaryPrimitives.ReadUInt16BigEndian(row[8..]) == sourcePort &&
                    BinaryPrimitives.ReadUInt16BigEndian(row[16..]) == destinationPort)
                    return true;
            }
            return false;
        }
        return false;
    }

    [DllImport("kernel32.dll")]
    private static extern bool GetNamedPipeServerProcessId(SafePipeHandle pipe, out int serverProcessId);
    [DllImport("iphlpapi.dll")]
    private static extern int GetExtendedTcpTable([Out] byte[] table, ref int size, bool order, int family, int tableClass, int reserved);
}
