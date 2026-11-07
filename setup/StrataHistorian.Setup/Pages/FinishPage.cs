using System.Diagnostics;
using StrataHistorian.Setup.Engine;
using StrataHistorian.Setup.Ui;

namespace StrataHistorian.Setup.Pages;

/// <summary>
/// The outcome: connection tests, what to open next, and the log.
/// </summary>
internal sealed class FinishPage : WizardPage
{
    private readonly Label _message = NewText(string.Empty);
    private readonly FlowLayoutPanel _tests = new() { FlowDirection = FlowDirection.TopDown, AutoSize = true, WrapContents = false, Margin = new Padding(0, 6, 0, 6) };
    private readonly FlowLayoutPanel _actions = new() { AutoSize = true, Margin = new Padding(0, 6, 0, 6) };
    private readonly Button _odbcAdmin = new() { Text = "Open ODBC Data Sources", AutoSize = true, BackColor = Theme.Brand, ForeColor = Color.White, FlatStyle = FlatStyle.Flat };
    private readonly Button _dataLink = new() { Text = "Open the OLE DB data link", AutoSize = true };
    private readonly Button _guide = new() { Text = "Open the usage guide", AutoSize = true };
    private readonly Label _logFile = NewText(string.Empty, Theme.Small, Theme.Muted);
    private readonly TextBox _log = new() { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Both, WordWrap = false, Width = ContentWidth, Height = 170, Font = Theme.Mono, BackColor = Color.White };
    private string _title = "Finished";
    private string? _odbcAdminPath;
    private string? _udlPath;
    private string? _guidePath;

    /// <summary>
    /// Initializes the page.
    /// </summary>
    public FinishPage()
    {
        _odbcAdmin.Click += (_, _) => Open(_odbcAdminPath);
        _dataLink.Click += (_, _) => Open(_udlPath);
        _guide.Click += (_, _) => Open(_guidePath);
        _actions.Controls.Add(_odbcAdmin);
        _actions.Controls.Add(_dataLink);
        _actions.Controls.Add(_guide);
        Stack.Controls.Add(_message);
        Stack.Controls.Add(_tests);
        Stack.Controls.Add(_actions);
        Stack.Controls.Add(_logFile);
        Stack.Controls.Add(_log);
    }

    /// <inheritdoc />
    public override string Title => _title;

    /// <summary>
    /// Shows the outcome of the run.
    /// </summary>
    /// <param name="session">The session.</param>
    /// <param name="succeeded">Whether the task finished without an error.</param>
    /// <param name="log">The log.</param>
    public void Show(SetupSession session, bool succeeded, string log)
    {
        ArgumentNullException.ThrowIfNull(session);
        _title = !succeeded ? "Setup didn't finish" : session.Action switch
        {
            SetupAction.Install => "The driver is installed",
            SetupAction.Update => "The driver is updated",
            SetupAction.Configure => "The data source is saved",
            _ => "The driver is uninstalled",
        };
        _message.ForeColor = succeeded ? Theme.Ink : Theme.Danger;
        _message.Text = !succeeded
            ? "Nothing more was changed after this error: " + (session.Error ?? "unknown") + Environment.NewLine + "Fix the cause and run setup again. The log below shows each step."
            : session.Action switch
            {
                SetupAction.Install => "Use the data source from Power BI (Get Data > ODBC), Excel, PowerShell, C# or any ODBC program. Add more data sources from the ODBC Data Sources tool or by running this setup again.",
                SetupAction.Uninstall => "The driver has been removed.",
                _ => string.Empty,
            };
        _message.Visible = _message.Text.Length > 0;

        _tests.Controls.Clear();
        foreach (var result in session.TestResults)
        {
            _tests.Controls.Add(NewText((result.Ok ? "✓ " : "✗ ") + result.Message, Theme.Body, result.Ok ? Theme.Success : Theme.Danger));
        }

        if (session.TestResults.Any(r => !r.Ok))
        {
            _tests.Controls.Add(NewText("The driver is installed; only the connection failed. Check the settings in ODBC Data Sources > System DSN > Configure, which has a Test button.", Theme.Small, Theme.Muted));
        }

        var installed = succeeded && session.Action != SetupAction.Uninstall;
        var source = session.Answers.CreateDataSource ? session.Answers.DataSource : null;
        _odbcAdminPath = session.System.System64.TrimEnd('\\') + @"\odbcad32.exe";
        _udlPath = source is { OleDbLink: true } ? UdlFile.PathFor(session.System, source.Name.Trim()) : null;
        _guidePath = session.State.InstallDir.Length > 0 ? session.State.InstallDir + @"\docs\usage-guide.md" : null;
        _actions.Visible = installed && !session.Demo;
        _dataLink.Visible = _udlPath is not null;
        _guide.Visible = _guidePath is not null && File.Exists(_guidePath);

        _logFile.Text = session.LogFile is null ? string.Empty : "Log saved to " + session.LogFile;
        _logFile.Visible = session.LogFile is not null;
        _log.Text = log;
        _log.SelectionStart = _log.TextLength;
        _log.ScrollToCaret();
    }

    private static void Open(string? path)
    {
        if (path is not null)
        {
            Process.Start(new ProcessStartInfo(path) { UseShellExecute = true })?.Dispose();
        }
    }
}
