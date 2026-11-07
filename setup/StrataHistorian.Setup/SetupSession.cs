using System.Globalization;
using System.Text;
using StrataHistorian.Setup.Engine;
using StrataHistorian.Setup.Engine.Windows;

namespace StrataHistorian.Setup;

/// <summary>
/// Everything the wizard knows: the payload, what is installed, the chosen task, the answers and the outcome. In demo
/// mode the machine is kept in memory, so every page and step runs without changing anything.
/// </summary>
internal sealed class SetupSession
{
    private readonly StringBuilder _log = new();

    /// <summary>Gets a value indicating whether this is demo mode.</summary>
    public bool Demo { get; init; }

    /// <summary>Gets a value indicating whether the demo computer already has the driver.</summary>
    public bool DemoInstalled { get; init; }

    /// <summary>Gets or sets the task chosen on the command line, if any.</summary>
    public SetupAction? RequestedAction { get; set; }

    /// <summary>Gets the machine.</summary>
    public ISystemOps System { get; private set; } = new MemorySystem();

    /// <summary>Gets the files to install, once read.</summary>
    public IPayload? Payload { get; private set; }

    /// <summary>Gets what is installed, once read.</summary>
    public InstallState State { get; private set; } = new();

    /// <summary>Gets or sets the chosen task.</summary>
    public SetupAction Action { get; set; } = SetupAction.Install;

    /// <summary>Gets the answers.</summary>
    public SetupAnswers Answers { get; private set; } = new();

    /// <summary>Gets the connection tests run after the task.</summary>
    public IReadOnlyList<TestResult> TestResults { get; private set; } = [];

    /// <summary>Gets the error of a failed run.</summary>
    public string? Error { get; private set; }

    /// <summary>Gets the file the log was saved to.</summary>
    public string? LogFile { get; private set; }

    /// <summary>Gets the connection tester.</summary>
    public IConnectionTester Tester { get; private set; } = new DemoTester();

    /// <summary>Gets the version this program installs.</summary>
    public string Version => Payload?.Version ?? string.Empty;

    /// <summary>
    /// Reads the payload and what is installed.
    /// </summary>
    /// <returns>A task that completes when the state is known.</returns>
    public async Task LoadAsync()
    {
        if (Payload is not null)
        {
            return;
        }

        if (Demo)
        {
            var system = new MemorySystem();
            system.Services[Names.GatewayService] = "Running";
            System = system;
            Payload = DemoPayload();
            if (DemoInstalled)
            {
                var answers = new SetupAnswers { InstallDir = DefaultInstallDir };
                answers.DataSource.BaseUrl = "https://piweb.example.local/piwebapi";
                answers.DataSource.DataServer = "PISRV01";
                new Installer(system, Payload, (_, _) => { }, _ => { }) { SelfPath = null }.Install(answers);
            }
        }
        else
        {
            System = new WindowsSystem();
            Tester = new WindowsTester();
            Payload = await Task.Run(Engine.Payload.Load).ConfigureAwait(false);
        }

        Refresh();
    }

    /// <summary>Gets the default program folder.</summary>
    public string DefaultInstallDir => System.ProgramFiles.TrimEnd('\\') + @"\" + Names.FolderName;

    /// <summary>
    /// Reads what is installed again.
    /// </summary>
    public void Refresh() => State = InstallState.Read(System);

    /// <summary>
    /// Starts the answers again for another task.
    /// </summary>
    public void ResetAnswers() => Answers = new SetupAnswers { InstallDir = DefaultInstallDir };

    /// <summary>
    /// Runs the chosen task, then tests the data source when one was set up.
    /// </summary>
    /// <param name="stage">Reports the share done and the step, on a background thread.</param>
    /// <param name="line">Reports a log line, on a background thread.</param>
    /// <returns>True when the task finished without an error.</returns>
    public async Task<bool> RunAsync(Action<int, string> stage, Action<string> line)
    {
        ArgumentNullException.ThrowIfNull(stage);
        ArgumentNullException.ThrowIfNull(line);
        Error = null;
        TestResults = [];
        void Log(string text)
        {
            lock (_log)
            {
                _log.AppendLine(text);
            }

            line(text);
        }

        void Stage(int percent, string text)
        {
            Log("==> " + text);
            stage(percent, text);
            if (Demo)
            {
                Thread.Sleep(350);
            }
        }

        Log(string.Format(CultureInfo.InvariantCulture, "{0} {1}: {2} ({3:yyyy-MM-dd HH:mm:ss})", Names.Product, Version, Action, DateTime.Now));
        var installer = new Installer(System, Payload ?? throw new InvalidOperationException("No payload."), Stage, Log)
        {
            SelfPath = Demo ? null : Environment.ProcessPath,
        };

        var ok = await Task.Run(() =>
        {
            try
            {
                switch (Action)
                {
                    case SetupAction.Install: installer.Install(Answers); break;
                    case SetupAction.Update: installer.Update(Answers); break;
                    case SetupAction.Configure: installer.Configure(Answers); break;
                    default: installer.Uninstall(Answers); break;
                }

                return true;
            }
            catch (Exception exception) when (exception is InvalidOperationException or IOException or UnauthorizedAccessException or InvalidDataException or global::System.ComponentModel.Win32Exception or global::System.Runtime.InteropServices.COMException or global::System.ServiceProcess.TimeoutException)
            {
                Error = exception.Message;
                Log("SETUP FAILED: " + exception.Message);
                return false;
            }
        }).ConfigureAwait(false);

        if (ok && Action is SetupAction.Install or SetupAction.Configure && Answers.CreateDataSource)
        {
            stage(96, "Testing the data source");
            TestResults = await Task.Run(TestSavedDataSource).ConfigureAwait(false);
            foreach (var result in TestResults)
            {
                Log((result.Ok ? "  OK   " : "  FAIL ") + result.Message);
            }
        }

        Refresh();
        SaveLog();
        return ok;
    }

    private List<TestResult> TestSavedDataSource()
    {
        var settings = Answers.DataSource;
        var connection = "DSN=" + settings.Name.Trim() + ";";
        if (Answers.TestUser.Length > 0)
        {
            connection += "UID={" + Answers.TestUser.Replace("}", "}}", StringComparison.Ordinal) + "};";
        }

        if (Answers.TestPassword.Length > 0)
        {
            connection += "PWD={" + Answers.TestPassword.Replace("}", "}}", StringComparison.Ordinal) + "};";
        }

        var results = new List<TestResult> { Tester.TestOdbc(connection) };
        if (settings.OleDbLink)
        {
            results.Add(Tester.TestOleDb(connection));
        }

        return results;
    }

    private void SaveLog()
    {
        if (Demo)
        {
            return;
        }

        try
        {
            var folder = Path.Combine(System.CommonData, Names.FolderName, "logs");
            Directory.CreateDirectory(folder);
            var file = Path.Combine(folder, "setup-" + DateTime.Now.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture) + ".log");
            lock (_log)
            {
                File.WriteAllText(file, _log.ToString(), new UTF8Encoding(false));
            }

            LogFile = file;
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            LogFile = null;
        }
    }

    private static Payload DemoPayload()
    {
        using var buffer = new MemoryStream();
        using (var zip = new global::System.IO.Compression.ZipArchive(buffer, global::System.IO.Compression.ZipArchiveMode.Create, leaveOpen: true))
        {
            foreach (var (path, text) in new[] { ("strataodbc.dll", "demo"), ("VERSION.txt", "0.0.0-demo"), ("License.txt", "Demo licence."), ("docs/usage-guide.md", "guide") })
            {
                using var writer = new StreamWriter(zip.CreateEntry(path).Open());
                writer.Write(text);
            }
        }

        buffer.Position = 0;
        return Engine.Payload.FromZip(buffer);
    }

    private sealed class DemoTester : IConnectionTester
    {
        public TestResult TestOdbc(string connectionString) => new(true, "ODBC: connected (demo, nothing was contacted).");

        public TestResult TestOleDb(string odbcConnectionString) => new(true, "OLE DB: connected through MSDASQL (demo, nothing was contacted).");
    }
}
