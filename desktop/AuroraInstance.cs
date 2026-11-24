using System;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Win32.SafeHandles;

namespace Aurora;

internal sealed class AuroraInstance : IDisposable
{
    private readonly Mutex mutex;
    private readonly string pipeName;
    private readonly CancellationTokenSource shutdown = new();
    private bool ownsMutex;
    private Task listener;

    internal AuroraInstance(string applicationName)
    {
        var windows = OperatingSystem.IsWindows();
        var scope = $"{applicationName}:{Environment.UserDomainName}:{Environment.UserName}";
        if (windows)
        {
            using var process = Process.GetCurrentProcess();
            scope += ":" + process.SessionId;
        }
        pipeName = "aurora-" + Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(scope)))[..20];
        mutex = new Mutex(initiallyOwned: false, windows ? @"Local\" + pipeName : pipeName);
    }

    internal bool TryAcquire()
    {
        try { ownsMutex = mutex.WaitOne(0); }
        catch (AbandonedMutexException) { ownsMutex = true; }
        return ownsMutex;
    }

    internal bool TryActivatePrimary()
    {
        try
        {
            using var pipe = new NamedPipeClientStream(".", pipeName, PipeDirection.Out, PipeOptions.CurrentUserOnly);
            pipe.Connect(3000);
            if (OperatingSystem.IsWindows() && GetNamedPipeServerProcessId(pipe.SafePipeHandle, out var processId))
                AllowSetForegroundWindow(processId);
            pipe.WriteByte(1);
            return true;
        }
        catch (Exception error) when (error is IOException or TimeoutException or UnauthorizedAccessException)
        {
            Trace.TraceWarning("Could not activate the existing Aurora window: {0}", error.Message);
            return false;
        }
    }

    internal void Listen(Action activate)
    {
        listener = ListenAsync(activate);
    }

    private async Task ListenAsync(Action activate)
    {
        try
        {
            var request = new byte[1];
            while (!shutdown.IsCancellationRequested)
            {
                using var pipe = new NamedPipeServerStream(pipeName, PipeDirection.In, 1, PipeTransmissionMode.Byte,
                    PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
                await pipe.WaitForConnectionAsync(shutdown.Token).ConfigureAwait(false);
                using var readTimeout = CancellationTokenSource.CreateLinkedTokenSource(shutdown.Token);
                readTimeout.CancelAfter(TimeSpan.FromSeconds(1));
                try
                {
                    if (await pipe.ReadAsync(request, readTimeout.Token).ConfigureAwait(false) == 1 && request[0] == 1)
                        activate();
                }
                catch (OperationCanceledException) when (readTimeout.IsCancellationRequested) { }
                catch (IOException) { }
            }
        }
        catch (OperationCanceledException) when (shutdown.IsCancellationRequested) { }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            Trace.TraceWarning("Could not listen for Aurora window activation: {0}", error.Message);
        }
    }

    public void Dispose()
    {
        shutdown.Cancel();
        listener?.GetAwaiter().GetResult();
        if (ownsMutex) mutex.ReleaseMutex();
        mutex.Dispose();
        shutdown.Dispose();
    }

    [DllImport("kernel32.dll")]
    private static extern bool GetNamedPipeServerProcessId(SafePipeHandle pipe, out uint processId);

    [DllImport("user32.dll")]
    private static extern bool AllowSetForegroundWindow(uint processId);
}
