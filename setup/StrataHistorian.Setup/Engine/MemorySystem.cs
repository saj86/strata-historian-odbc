namespace StrataHistorian.Setup.Engine;

/// <summary>
/// A machine kept in memory: demo mode and the tests use it, so nothing real is changed.
/// </summary>
internal sealed class MemorySystem : ISystemOps
{
    private readonly Dictionary<string, Dictionary<string, string>> _keys = new(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, byte[]> _files = new(StringComparer.OrdinalIgnoreCase);
    private readonly HashSet<string> _folders = new(StringComparer.OrdinalIgnoreCase);

    /// <inheritdoc />
    public string ProgramFiles => @"C:\Program Files";

    /// <inheritdoc />
    public string CommonPrograms => @"C:\ProgramData\Microsoft\Windows\Start Menu\Programs";

    /// <inheritdoc />
    public string CommonDocuments => @"C:\Users\Public\Documents";

    /// <inheritdoc />
    public string CommonData => @"C:\ProgramData";

    /// <inheritdoc />
    public string System64 => @"C:\Windows\System32";

    /// <summary>Gets the shortcuts: path to target.</summary>
    public Dictionary<string, string> Shortcuts { get; } = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>Gets the services and their states.</summary>
    public Dictionary<string, string> Services { get; } = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>Gets the service operations in order, such as "stop PBIEgwService".</summary>
    public List<string> ServiceLog { get; } = [];

    /// <summary>Gets or sets a file that can't be replaced, to simulate a driver that is in use.</summary>
    public string? LockedFile { get; set; }

    /// <summary>Gets the paths of every file.</summary>
    public IReadOnlyCollection<string> Files => _files.Keys;

    /// <summary>Reads a file's content.</summary>
    public byte[] ReadFile(string path) => _files[path];

    /// <inheritdoc />
    public string? ReadValue(string key, string name) => _keys.TryGetValue(key, out var values) && values.TryGetValue(name, out var value) ? value : null;

    /// <inheritdoc />
    public IReadOnlyList<string> ValueNames(string key) => _keys.TryGetValue(key, out var values) ? [.. values.Keys] : [];

    /// <inheritdoc />
    public IReadOnlyList<string> SubKeyNames(string key)
    {
        var prefix = key + @"\";
        return [.. _keys.Keys.Where(k => k.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)).Select(k => k[prefix.Length..].Split('\\')[0]).Distinct(StringComparer.OrdinalIgnoreCase)];
    }

    /// <inheritdoc />
    public void WriteString(string key, string name, string value) => Key(key)[name] = value;

    /// <inheritdoc />
    public void WriteDword(string key, string name, int value) => Key(key)[name] = value.ToString(System.Globalization.CultureInfo.InvariantCulture);

    /// <inheritdoc />
    public void DeleteValue(string key, string name)
    {
        if (_keys.TryGetValue(key, out var values))
        {
            values.Remove(name);
        }
    }

    /// <inheritdoc />
    public void DeleteKeyTree(string key)
    {
        foreach (var k in _keys.Keys.Where(k => k.Equals(key, StringComparison.OrdinalIgnoreCase) || k.StartsWith(key + @"\", StringComparison.OrdinalIgnoreCase)).ToList())
        {
            _keys.Remove(k);
        }
    }

    /// <inheritdoc />
    public bool FileExists(string path) => _files.ContainsKey(path);

    /// <inheritdoc />
    public bool DirectoryExists(string path) => _folders.Contains(path.TrimEnd('\\'));

    /// <inheritdoc />
    public bool DirectoryHasEntries(string path)
    {
        var prefix = path.TrimEnd('\\') + @"\";
        return _files.Keys.Any(f => f.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) || _folders.Any(f => f.StartsWith(prefix, StringComparison.OrdinalIgnoreCase));
    }

    /// <inheritdoc />
    public void CreateDirectory(string path)
    {
        var current = path.TrimEnd('\\');
        while (!string.IsNullOrEmpty(current) && current.Length > 3)
        {
            _folders.Add(current);
            current = Path.GetDirectoryName(current.Replace('\\', Path.DirectorySeparatorChar))?.Replace(Path.DirectorySeparatorChar, '\\') ?? string.Empty;
        }
    }

    /// <inheritdoc />
    public void WriteFile(string path, byte[] content)
    {
        if (string.Equals(path, LockedFile, StringComparison.OrdinalIgnoreCase))
        {
            throw new IOException($"The process cannot access the file '{path}' because it is being used by another process.");
        }

        CreateDirectory(Parent(path));
        _files[path] = content;
    }

    /// <inheritdoc />
    public void CopyFile(string source, string target) => WriteFile(target, _files.TryGetValue(source, out var bytes) ? bytes : [1, 2, 3]);

    /// <inheritdoc />
    public void DeleteFile(string path) => _files.Remove(path);

    /// <inheritdoc />
    public void DeleteDirectoryTree(string path)
    {
        var root = path.TrimEnd('\\');
        var prefix = root + @"\";
        foreach (var f in _files.Keys.Where(f => f.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)).ToList())
        {
            _files.Remove(f);
        }

        _folders.RemoveWhere(f => f.Equals(root, StringComparison.OrdinalIgnoreCase) || f.StartsWith(prefix, StringComparison.OrdinalIgnoreCase));
        foreach (var s in Shortcuts.Keys.Where(s => s.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)).ToList())
        {
            Shortcuts.Remove(s);
        }
    }

    /// <inheritdoc />
    public void CreateShortcut(string path, string target, string description)
    {
        CreateDirectory(Parent(path));
        Shortcuts[path] = target;
    }

    /// <inheritdoc />
    public string? ServiceState(string name) => Services.TryGetValue(name, out var state) ? state : null;

    /// <inheritdoc />
    public void StopService(string name)
    {
        ServiceLog.Add("stop " + name);
        Services[name] = "Stopped";
    }

    /// <inheritdoc />
    public void StartService(string name)
    {
        ServiceLog.Add("start " + name);
        Services[name] = "Running";
    }

    private static string Parent(string path) => path[..Math.Max(0, path.LastIndexOf('\\'))];

    private Dictionary<string, string> Key(string key)
    {
        if (!_keys.TryGetValue(key, out var values))
        {
            values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            _keys[key] = values;
        }

        return values;
    }
}
