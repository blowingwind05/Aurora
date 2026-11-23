using System;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using Avalonia.Controls;
using Avalonia.Platform;

namespace Aurora;

internal sealed class WindowsStreamSurface(string sessionName) : NativeControlHost
{
    private const uint Child = 0x40000000;
    private const uint ClipChildren = 0x02000000;
    private const uint ClipSiblings = 0x04000000;
    private const uint BlackRectangle = 0x00000004;
    private readonly TaskCompletionSource<nint> handle = new(TaskCreationOptions.RunContinuationsAsynchronously);

    internal Task<nint> Handle => handle.Task;

    protected override IPlatformHandle CreateNativeControlCore(IPlatformHandle parent)
    {
        if (parent.HandleDescriptor == "HWND")
        {
            var hwnd = CreateWindowEx(0, "STATIC", null, Child | ClipChildren | ClipSiblings | BlackRectangle,
                0, 0, 1, 1, parent.Handle, 0, 0, 0);
            if (hwnd != 0)
            {
                if (SetProp(hwnd, sessionName, 1))
                {
                    handle.TrySetResult(hwnd);
                    return new PlatformHandle(hwnd, "HWND");
                }
                DestroyWindow(hwnd);
            }
        }
        handle.TrySetResult(0);
        return base.CreateNativeControlCore(parent);
    }

    protected override void DestroyNativeControlCore(IPlatformHandle control)
    {
        if (handle.Task.IsCompletedSuccessfully && control.Handle == handle.Task.Result)
        {
            RemoveProp(control.Handle, sessionName);
            DestroyWindow(control.Handle);
        }
        else base.DestroyNativeControlCore(control);
    }

    [DllImport("user32.dll", EntryPoint = "CreateWindowExW", CharSet = CharSet.Unicode)]
    private static extern nint CreateWindowEx(uint extendedStyle, string className, string title, uint style,
        int x, int y, int width, int height, nint parent, nint menu, nint instance, nint parameter);

    [DllImport("user32.dll", EntryPoint = "SetPropW", CharSet = CharSet.Unicode)]
    private static extern bool SetProp(nint window, string name, nint value);

    [DllImport("user32.dll", EntryPoint = "RemovePropW", CharSet = CharSet.Unicode)]
    private static extern nint RemoveProp(nint window, string name);

    [DllImport("user32.dll")]
    private static extern bool DestroyWindow(nint window);
}
