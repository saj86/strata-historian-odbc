using System.Globalization;

namespace StrataHistorian.Setup.Engine;

/// <summary>
/// Installs, updates, configures and removes the driver. Every change goes through <see cref="ISystemOps"/>, so the same
/// steps run for real, in demo mode and in the tests.
/// </summary>
internal sealed class Installer
{
    private readonly ISystemOps _system;
    private readonly IPayload _payload;
    private readonly Action<int, string> _stage;
    private readonly Action<string> _log;

    /// <summary>
    /// Initializes the installer.
    /// </summary>
    /// <param name="system">The machine.</param>
    /// <param name="payload">The files to install.</param>
    /// <param name="stage">Reports the share done and the current step.</param>
    /// <param name="log">Writes a line to the log.</param>
    public Installer(ISystemOps system, IPayload payload, Action<int, string> stage, Action<string> log)
    {
        _system = system ?? throw new ArgumentNullException(nameof(system));
        _payload = payload ?? throw new ArgumentNullException(nameof(payload));
        _stage = stage ?? throw new ArgumentNullException(nameof(stage));
        _log = log ?? throw new ArgumentNullException(nameof(log));
    }

    /// <summary>Gets or sets the running setup program, copied to the program folder for later changes and removal.</summary>
    public string? SelfPath { get; set; }

    /// <summary>
    /// Installs on a computer without the driver.
    /// </summary>
    /// <param name="answers">The answers.</param>
    public void Install(SetupAnswers answers) => InstallFiles(answers, update: false);

    /// <summary>
    /// Installs this version over the installed one. Data sources and choices made at installation are kept.
    /// </summary>
    /// <param name="answers">The answers.</param>
    public void Update(SetupAnswers answers) => InstallFiles(answers, update: true);

    /// <summary>
    /// Adds or changes one data source and its OLE DB data link.
    /// </summary>
    /// <param name="answers">The answers.</param>
    public void Configure(SetupAnswers answers)
    {
        ArgumentNullException.ThrowIfNull(answers);
        var state = InstallState.Read(_system);
        if (!state.Installed)
        {
            throw new InvalidOperationException("The driver isn't installed by this setup program.");
        }

        _stage(30, "Saving the data source");
        SaveDataSource(answers, state.InstallDir);
        _stage(100, "Finished");
    }

    /// <summary>
    /// Removes the driver, its shortcuts and its Programs and Features entry, and optionally its data sources.
    /// </summary>
    /// <param name="answers">The answers.</param>
    public void Uninstall(SetupAnswers answers)
    {
        ArgumentNullException.ThrowIfNull(answers);
        var state = InstallState.Read(_system);
        if (!state.Installed)
        {
            throw new InvalidOperationException("The driver isn't installed by this setup program.");
        }

        var stopped = StopGateway(answers, state);
        try
        {
            if (answers.RemoveDataSources)
            {
                _stage(15, "Removing data sources");
                foreach (var name in state.DataSources)
                {
                    RemoveDataSource(name);
                }
            }

            _stage(35, "Removing Start menu shortcuts");
            _system.DeleteDirectoryTree(MenuFolder);

            _stage(50, "Unregistering the ODBC driver");
            _system.DeleteKeyTree(Names.DriverKey);
            _system.DeleteValue(Names.DriverListKey, Names.DriverName);
            _log("  Unregistered " + Names.DriverName);

            _stage(65, "Removing the Programs and Features entry");
            _system.DeleteKeyTree(Names.UninstallKey);
            _system.DeleteKeyTree(Names.SetupKey);

            _stage(80, "Removing program files");
            try
            {
                _system.DeleteDirectoryTree(state.InstallDir);
                _log("  Removed " + state.InstallDir);
            }
            catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
            {
                // The driver stays usable until the program holding it closes; the folder can then be deleted by hand.
                _log("  Some files are still in use and were left in " + state.InstallDir + ": " + exception.Message);
            }

            if (!answers.RemoveDataSources && state.DataSources.Count > 0)
            {
                _log("  Kept data sources: " + string.Join(", ", state.DataSources) + ". They stop working until the driver is installed again.");
            }
        }
        finally
        {
            StartGateway(stopped);
        }

        _stage(100, "Finished");
    }

    private string MenuFolder => _system.CommonPrograms.TrimEnd('\\') + @"\" + Names.FolderName;

    private void InstallFiles(SetupAnswers answers, bool update)
    {
        ArgumentNullException.ThrowIfNull(answers);
        _stage(5, "Checking this computer");
        var state = InstallState.Read(_system);
        if (state.RegisteredElsewhere)
        {
            throw new InvalidOperationException($"{Names.DriverName} is already registered from {state.DriverPath}, by the MSI or a script. Remove that installation first, from Programs and Features, then run setup again.");
        }

        if (update != state.Installed)
        {
            throw new InvalidOperationException(update ? "There is no installation to update." : "The driver is already installed. Choose Update instead.");
        }

        var dir = (update ? state.InstallDir : answers.InstallDir).Trim().TrimEnd('\\');
        if (dir.Length == 0)
        {
            throw new InvalidOperationException("No program folder was chosen.");
        }

        // An update keeps the choices made when the driver was installed.
        var docs = update ? _system.ReadValue(Names.SetupKey, "InstallDocs") != "0" : answers.InstallDocs;
        var shortcuts = update ? _system.ReadValue(Names.SetupKey, "Shortcuts") != "0" : answers.Shortcuts;

        var stopped = StopGateway(answers, state);
        try
        {
            _stage(15, "Copying files to " + dir);
            if (update)
            {
                _system.DeleteDirectoryTree(dir + @"\docs");
                _system.DeleteDirectoryTree(dir + @"\samples");
            }

            CopyFiles(dir, docs, state);

            _stage(45, "Registering the ODBC driver");
            RegisterDriver(dir);

            if (shortcuts)
            {
                _stage(55, "Creating Start menu shortcuts");
                CreateShortcuts(dir, docs);
            }

            if (!update && answers.CreateDataSource)
            {
                _stage(65, "Creating the data source");
                SaveDataSource(answers, dir);
            }

            _stage(80, "Adding the driver to Programs and Features");
            RegisterProgram(dir, docs, shortcuts);
        }
        finally
        {
            StartGateway(stopped);
        }

        _stage(100, "Finished");
    }

    private void CopyFiles(string dir, bool docs, InstallState state)
    {
        _system.CreateDirectory(dir);
        foreach (var file in _payload.Files)
        {
            if (!docs && (file.Path.StartsWith(@"docs\", StringComparison.OrdinalIgnoreCase) || file.Path.StartsWith(@"samples\", StringComparison.OrdinalIgnoreCase)))
            {
                continue;
            }

            var target = dir + @"\" + file.Path;
            try
            {
                _system.WriteFile(target, file.Read());
            }
            catch (IOException exception) when (file.Path.Equals(Names.DriverFile, StringComparison.OrdinalIgnoreCase))
            {
                var gateway = state.GatewayState is null ? string.Empty : " If the on-premises data gateway uses it, let setup restart the gateway.";
                throw new InvalidOperationException(Names.DriverFile + " is in use. Close Power BI Desktop, Excel and any other program using the driver, then try again." + gateway, exception);
            }

            _log("  " + file.Path);
        }

        if (SelfPath is not null && _system.FileExists(SelfPath))
        {
            var maintenance = dir + @"\" + Names.MaintenanceExe;
            if (!string.Equals(Path.GetFullPath(SelfPath), maintenance, StringComparison.OrdinalIgnoreCase))
            {
                _system.CopyFile(SelfPath, maintenance);
                _log("  " + Names.MaintenanceExe);
            }
        }
    }

    private void RegisterDriver(string dir)
    {
        var dll = dir + @"\" + Names.DriverFile;
        var key = Names.DriverKey;
        _system.WriteString(Names.DriverListKey, Names.DriverName, "Installed");
        _system.WriteString(key, "Driver", dll);
        _system.WriteString(key, "Setup", dll);  // the driver's own DLL shows the data source settings window
        _system.WriteString(key, "APILevel", "1");
        _system.WriteString(key, "ConnectFunctions", "YYN");
        _system.WriteString(key, "DriverODBCVer", "03.52");
        _system.WriteString(key, "FileUsage", "0");
        _system.WriteString(key, "SQLLevel", "0");
        _system.WriteDword(key, "UsageCount", 1);
        _log("  Registered " + Names.DriverName + " as " + dll);
    }

    private void CreateShortcuts(string dir, bool docs)
    {
        var menu = MenuFolder;
        _system.DeleteDirectoryTree(menu);
        _system.CreateDirectory(menu);
        _system.CreateShortcut(menu + @"\ODBC Data Sources (64-bit).lnk", _system.System64.TrimEnd('\\') + @"\odbcad32.exe", "Add or change data sources");
        _system.CreateShortcut(menu + @"\OLE DB data links.lnk", UdlFile.Folder(_system), "OLE DB data links for the data sources");
        if (SelfPath is not null)
        {
            _system.CreateShortcut(menu + @"\Change or remove " + Names.Product + ".lnk", dir + @"\" + Names.MaintenanceExe, "Update, set up data sources, or uninstall");
        }

        if (docs)
        {
            _system.CreateShortcut(menu + @"\Usage guide.lnk", dir + @"\docs\usage-guide.md", "How to install and use the driver");
            _system.CreateShortcut(menu + @"\Documents and examples.lnk", dir, "The installed documents and examples");
        }

        _log("  Start menu: " + menu);
    }

    private void RegisterProgram(string dir, bool docs, bool shortcuts)
    {
        var size = _payload.Files.Sum(f => f.Length) / 1024;
        var exe = dir + @"\" + Names.MaintenanceExe;
        var key = Names.UninstallKey;
        _system.WriteString(key, "DisplayName", Names.Product);
        _system.WriteString(key, "DisplayVersion", _payload.Version);
        _system.WriteString(key, "Publisher", Names.Publisher);
        _system.WriteString(key, "InstallLocation", dir);
        _system.WriteString(key, "URLInfoAbout", Names.HomePage);
        _system.WriteDword(key, "EstimatedSize", (int)Math.Min(int.MaxValue, size));
        _system.WriteDword(key, "NoRepair", 1);
        if (SelfPath is not null)
        {
            _system.WriteString(key, "DisplayIcon", exe);
            _system.WriteString(key, "UninstallString", "\"" + exe + "\" --uninstall");
            _system.WriteString(key, "ModifyPath", "\"" + exe + "\"");
        }

        _system.WriteString(Names.SetupKey, "InstallDir", dir);
        _system.WriteString(Names.SetupKey, "Version", _payload.Version);
        _system.WriteString(Names.SetupKey, "InstallDocs", docs ? "1" : "0");
        _system.WriteString(Names.SetupKey, "Shortcuts", shortcuts ? "1" : "0");
        _log("  Programs and Features: " + Names.Product + " " + _payload.Version);
    }

    private void SaveDataSource(SetupAnswers answers, string dir)
    {
        var settings = answers.DataSource;
        var problem = settings.Validate();
        if (problem is not null)
        {
            throw new InvalidOperationException(problem);
        }

        var name = settings.Name.Trim();
        var key = Names.OdbcIni + @"\" + name;
        var owner = _system.ReadValue(Names.DataSourceListKey, name);
        if (owner is not null && !owner.Equals(Names.DriverName, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException($"A data source named {name} already exists and uses {owner}. Choose another name.");
        }

        if (answers.EditingDataSource is { } old && !old.Equals(name, StringComparison.OrdinalIgnoreCase))
        {
            RemoveDataSource(old);
        }

        // Exactly the desired values: anything else, such as a UID or PWD added by hand, is removed.
        var desired = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase) { ["Driver"] = dir + @"\" + Names.DriverFile };
        foreach (var (attribute, value) in settings.Attributes())
        {
            if (value.Length > 0)
            {
                desired[attribute] = value;
            }
        }

        foreach (var existing in _system.ValueNames(key))
        {
            if (!desired.ContainsKey(existing))
            {
                _system.DeleteValue(key, existing);
            }
        }

        foreach (var (attribute, value) in desired)
        {
            _system.WriteString(key, attribute, value);
        }

        _system.WriteString(Names.DataSourceListKey, name, Names.DriverName);
        _log(string.Format(CultureInfo.InvariantCulture, "  System data source {0}: {1}", name, settings.TestData ? "test data (fixture)" : settings.BaseUrl + ", " + settings.DataServer));

        var link = UdlFile.PathFor(_system, name);
        if (settings.OleDbLink)
        {
            _system.WriteFile(link, UdlFile.Build(name));
            _log("  OLE DB data link: " + link);
        }
        else
        {
            _system.DeleteFile(link);
        }
    }

    private void RemoveDataSource(string name)
    {
        _system.DeleteKeyTree(Names.OdbcIni + @"\" + name);
        _system.DeleteValue(Names.DataSourceListKey, name);
        _system.DeleteFile(UdlFile.PathFor(_system, name));
        _log("  Removed data source " + name);
    }

    private bool StopGateway(SetupAnswers answers, InstallState state)
    {
        if (!answers.RestartGateway || state.GatewayState != "Running")
        {
            return false;
        }

        _stage(8, "Stopping the on-premises data gateway");
        _system.StopService(Names.GatewayService);
        _log("  Stopped " + Names.GatewayService);
        return true;
    }

    private void StartGateway(bool stopped)
    {
        if (!stopped)
        {
            return;
        }

        _stage(92, "Starting the on-premises data gateway");
        _system.StartService(Names.GatewayService);
        _log("  Started " + Names.GatewayService);
    }
}
