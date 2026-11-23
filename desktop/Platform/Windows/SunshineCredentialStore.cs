using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

namespace Aurora;

internal sealed class SunshineLogin(string username, string password)
{
    public string Username { get; } = username;
    public string Password { get; } = password;
}

internal sealed class SunshineCredentialStore
{
    private const uint GenericCredential = 1;
    private const uint LocalComputer = 2;
    private const int NotFound = 1168;
    private const int MaxBlobSize = 2560;
    private readonly string target;

    public SunshineCredentialStore(string scope)
    {
        if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException();
        target = "Aurora:Sunshine:" + scope;
    }

    public SunshineLogin Read()
    {
        if (!CredRead(target, GenericCredential, 0, out var pointer))
        {
            var error = Marshal.GetLastWin32Error();
            if (error == NotFound) return null;
            throw new Win32Exception(error);
        }

        try
        {
            var credential = Marshal.PtrToStructure<NativeCredential>(pointer);
            if (credential.Blob == IntPtr.Zero || credential.BlobSize is 0 or > MaxBlobSize ||
                credential.BlobSize % sizeof(char) != 0 || string.IsNullOrEmpty(credential.UserName))
                throw new InvalidOperationException("Windows 凭据管理器中的 Sunshine 登录记录格式无效。");
            return new SunshineLogin(credential.UserName, Marshal.PtrToStringUni(credential.Blob, (int)credential.BlobSize / sizeof(char)));
        }
        finally { CredFree(pointer); }
    }

    public void Save(string username, string password)
    {
        SunshineAdmin.ValidateAccount(username, password);
        if (Encoding.Unicode.GetByteCount(password) > MaxBlobSize)
            throw new InvalidOperationException("密码过长，Windows 凭据管理器无法保存。");

        var bytes = Encoding.Unicode.GetBytes(password);
        var blob = GCHandle.Alloc(bytes, GCHandleType.Pinned);
        try
        {
            var credential = new NativeCredential
            {
                Type = GenericCredential, TargetName = target, UserName = username,
                Blob = blob.AddrOfPinnedObject(), BlobSize = (uint)bytes.Length, Persist = LocalComputer
            };
            if (!CredWrite(ref credential, 0))
                throw new Win32Exception(Marshal.GetLastWin32Error());
        }
        finally
        {
            CryptographicOperations.ZeroMemory(bytes);
            blob.Free();
        }
    }

    public void Forget()
    {
        if (CredDelete(target, GenericCredential, 0)) return;
        var error = Marshal.GetLastWin32Error();
        if (error != NotFound) throw new Win32Exception(error);
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct NativeCredential
    {
        public uint Flags;
        public uint Type;
        public string TargetName;
        public string Comment;
        public System.Runtime.InteropServices.ComTypes.FILETIME LastWritten;
        public uint BlobSize;
        public IntPtr Blob;
        public uint Persist;
        public uint AttributeCount;
        public IntPtr Attributes;
        public string TargetAlias;
        public string UserName;
    }

    [DllImport("advapi32.dll", EntryPoint = "CredReadW", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CredRead(string target, uint type, uint flags, out IntPtr credential);

    [DllImport("advapi32.dll", EntryPoint = "CredWriteW", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CredWrite(ref NativeCredential credential, uint flags);

    [DllImport("advapi32.dll", EntryPoint = "CredDeleteW", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CredDelete(string target, uint type, uint flags);

    [DllImport("advapi32.dll", ExactSpelling = true)]
    private static extern void CredFree(IntPtr buffer);
}
