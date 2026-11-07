using System.Runtime.Versioning;
using System.ServiceProcess;
using Microsoft.Win32;

namespace StrataHistorian.Setup.Engine.Windows;

/// <summary>
/// The real machine: HKLM in the 64-bit view, the file system, Start menu shortcuts and services.
/// </summary>
[SupportedOSPlatform("windows")]
internal sealed class WindowsSystem : ISystemOps
{
    private static readonly TimeSpan ServiceWait = TimeSpan.FromSeconds(60);

    /// <inheritdoc />
    public string ProgramFiles => Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles);

    /// <inheritdoc />
    public string CommonPrograms => Environment.GetFolderPath(Environment.SpecialFolder.CommonPrograms);

    /// <inheritdoc />
    public string CommonDocuments => Environment.GetFolderPath(Environment.SpecialFolder.CommonDocuments);

    /// <inheritdoc />
    public string CommonData => Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData);

    /// <inheritdoc />
    public string System64 => Environment.GetFolderPath(Environment.SpecialFolder.System);

    /// <inheritdoc />
    public string? ReadValue(string key, string name)
    {
        using var k = Open(key, writable: false);
        return k?.GetValue(name)?.ToString();
    }

    /// <inheritdoc />
    public IReadOnlyList<string> ValueNames(string key)
    {
        using var k = Open(key, writable: false);
        return k?.GetValueNames() ?? [];
    }

    /// <inheritdoc />
    public IReadOnlyList<string> SubKeyNames(string key)
    {
        using var k = Open(key, writable: false);
        return k?.GetSubKeyNames() ?? [];
    }

    /// <inheritdoc />
    public void WriteString(string key, string name, string value)
    {
        using var k = Create(key);
        k.SetValue(name, value, RegistryValueKind.String);
    }

    /// <inheritdoc />
    public void WriteDword(string key, string name, int value)
    {
        using var k = Create(key);
        k.SetValue(name, value, RegistryValueKind.DWord);
    }

    /// <inheritdoc />
    public void DeleteValue(string key, string name)
    {
        using var k = Open(key, writable: true);
        k?.DeleteValue(name, throwOnMissingValue: false);
    }

    /// <inheritdoc />
    public void DeleteKeyTree(string key)
    {
        using var root = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
        root.DeleteSubKeyTree(key, throwOnMissingSubKey: false);
    }

    /// <inheritdoc />
    public bool FileExists(string path) => File.Exists(path);

    /// <inheritdoc />
    public bool DirectoryExists(string path) => Directory.Exists(path);

    /// <inheritdoc />
    public bool DirectoryHasEntries(string path) => Directory.Exists(path) && Directory.EnumerateFileSystemEntries(path).Any();

    /// <inheritdoc />
    public void CreateDirectory(string path) => Directory.CreateDirectory(path);

    /// <inheritdoc />
    public void WriteFile(string path, byte[] content)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllBytes(path, content);
    }

    /// <inheritdoc />
    public void CopyFile(string source, string target)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        File.Copy(source, target, overwrite: true);
    }

    /// <inheritdoc />
    public void DeleteFile(string path)
    {
        if (File.Exists(path))
        {
            File.Delete(path);
        }
    }

    /// <inheritdoc />
    public void DeleteDirectoryTree(string path)
    {
        if (Directory.Exists(path))
        {
            Directory.Delete(path, recursive: true);
        }
    }

    /// <inheritdoc />
    public void CreateShortcut(string path, string target, string description)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var shellType = Type.GetTypeFromProgID("WScript.Shell") ?? throw new InvalidOperationException("Windows Script Host isn't available.");
        dynamic shell = Activator.CreateInstance(shellType)!;
        try
        {
            dynamic link = shell.CreateShortcut(path);
            link.TargetPath = target;
            link.Description = description;
            link.Save();
        }
        finally
        {
            System.Runtime.InteropServices.Marshal.FinalReleaseComObject(shell);
        }
    }

    /// <inheritdoc />
    public string? ServiceState(string name)
    {
        try
        {
            using var service = new ServiceController(name);
            return service.Status.ToString();
        }
        catch (InvalidOperationException)
        {
            return null;  // not installed
        }
    }

    /// <inheritdoc />
    public void StopService(string name)
    {
        using var service = new ServiceController(name);
        service.Stop();
        service.WaitForStatus(ServiceControllerStatus.Stopped, ServiceWait);
    }

    /// <inheritdoc />
    public void StartService(string name)
    {
        using var service = new ServiceController(name);
        service.Start();
        service.WaitForStatus(ServiceControllerStatus.Running, ServiceWait);
    }

    private static RegistryKey? Open(string key, bool writable)
    {
        using var root = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
        return root.OpenSubKey(key, writable);
    }

    private static RegistryKey Create(string key)
    {
        using var root = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
        return root.CreateSubKey(key, writable: true);
    }
}
