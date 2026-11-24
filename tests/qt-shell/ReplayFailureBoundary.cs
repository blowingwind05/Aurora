// SPDX-License-Identifier: GPL-3.0-only
using System;
using System.IO;
using System.Runtime.InteropServices;

namespace Aurora.Replay;

internal static class ReplayFailureBoundary
{
    private const uint FailCriticalErrors = 0x0001;
    private const uint NoCrashDialog = 0x0002;
    private const uint NoOpenFileDialog = 0x8000;

    // Error mode is local to this replay process and inherited by its own children.
    internal static void DisableCrashDialogs()
    {
        if (!OperatingSystem.IsWindows()) return;
        SetErrorMode(GetErrorMode() | FailCriticalErrors | NoCrashDialog | NoOpenFileDialog);
    }

    internal static int Report(Exception error)
    {
        var message = $"Replay failed (PID {Environment.ProcessId}): {error}";
        Console.Error.WriteLine(message);
        var path = Environment.GetEnvironmentVariable("AURORA_REPLAY_FAILURE_LOG");
        if (!string.IsNullOrEmpty(path))
        {
            try { File.AppendAllText(path, message + Environment.NewLine); }
            catch (Exception logError) when (logError is IOException or UnauthorizedAccessException or ArgumentException)
            { Console.Error.WriteLine($"Could not write the replay failure log: {logError.Message}"); }
        }
        return 1;
    }

    [DllImport("kernel32.dll")]
    private static extern uint GetErrorMode();

    [DllImport("kernel32.dll")]
    private static extern uint SetErrorMode(uint mode);

}
