namespace StrataHistorian.Setup.Engine;

/// <summary>
/// Everything the setup program changes on the machine. <see cref="Windows.WindowsSystem"/> does it for real;
/// <see cref="MemorySystem"/> keeps it in memory for demo mode and the tests. Registry keys are under HKLM, 64-bit view.
/// </summary>
internal interface ISystemOps
{
    /// <summary>Gets the 64-bit Program Files folder.</summary>
    string ProgramFiles { get; }

    /// <summary>Gets the Start menu programs folder for all users.</summary>
    string CommonPrograms { get; }

    /// <summary>Gets the Public Documents folder.</summary>
    string CommonDocuments { get; }

    /// <summary>Gets the ProgramData folder.</summary>
    string CommonData { get; }

    /// <summary>Gets the 64-bit system folder, where odbcad32.exe is.</summary>
    string System64 { get; }

    /// <summary>Reads a string or number value, or null when absent.</summary>
    string? ReadValue(string key, string name);

    /// <summary>Lists a key's value names; empty when the key is absent.</summary>
    IReadOnlyList<string> ValueNames(string key);

    /// <summary>Lists a key's subkeys; empty when the key is absent.</summary>
    IReadOnlyList<string> SubKeyNames(string key);

    /// <summary>Writes a string value, creating the key.</summary>
    void WriteString(string key, string name, string value);

    /// <summary>Writes a DWORD value, creating the key.</summary>
    void WriteDword(string key, string name, int value);

    /// <summary>Deletes a value if present.</summary>
    void DeleteValue(string key, string name);

    /// <summary>Deletes a key and its subkeys if present.</summary>
    void DeleteKeyTree(string key);

    /// <summary>Gets whether a file exists.</summary>
    bool FileExists(string path);

    /// <summary>Gets whether a folder exists.</summary>
    bool DirectoryExists(string path);

    /// <summary>Gets whether a folder holds anything.</summary>
    bool DirectoryHasEntries(string path);

    /// <summary>Creates a folder and its parents.</summary>
    void CreateDirectory(string path);

    /// <summary>Writes a file, replacing it.</summary>
    void WriteFile(string path, byte[] content);

    /// <summary>Copies a file, replacing the target.</summary>
    void CopyFile(string source, string target);

    /// <summary>Deletes a file if present.</summary>
    void DeleteFile(string path);

    /// <summary>Deletes a folder and everything in it if present.</summary>
    void DeleteDirectoryTree(string path);

    /// <summary>Creates a Start menu shortcut.</summary>
    void CreateShortcut(string path, string target, string description);

    /// <summary>Gets a service's state, such as Running or Stopped, or null when the service doesn't exist.</summary>
    string? ServiceState(string name);

    /// <summary>Stops a service and waits for it.</summary>
    void StopService(string name);

    /// <summary>Starts a service and waits for it.</summary>
    void StartService(string name);
}
