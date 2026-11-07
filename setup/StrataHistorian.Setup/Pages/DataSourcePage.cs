using System.Globalization;
using StrataHistorian.Setup.Engine;
using StrataHistorian.Setup.Ui;

namespace StrataHistorian.Setup.Pages;

/// <summary>
/// A system data source and its OLE DB data link, with ODBC and OLE DB connection tests. Credentials entered here are
/// used only for testing and are never saved.
/// </summary>
internal sealed class DataSourcePage : WizardPage
{
    private const string NewDataSource = "(new data source)";
    private static readonly string[] AuthValues = ["Negotiate", "Basic", "Anonymous"];
    private readonly CheckBox _create = NewCheck("Set up a data source now", true);
    private readonly ComboBox _existing = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 340 };
    private readonly TableLayoutPanel _pickRow = NewGrid();
    private readonly Panel _fields = new() { AutoSize = true, Margin = Padding.Empty };
    private readonly TextBox _name = NewBox("StrataPI");
    private readonly TextBox _description = NewBox();
    private readonly RadioButton _piWebApi = new() { Text = "PI Web API", AutoSize = true, Checked = true };
    private readonly RadioButton _testData = new() { Text = "Synthetic test data (not PI)", AutoSize = true };
    private readonly TextBox _url = NewBox("https://");
    private readonly TextBox _server = NewBox();
    private readonly ComboBox _auth = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 340 };
    private readonly NumericUpDown _days = new() { Minimum = 1, Maximum = 3660, Value = 31, Width = 80 };
    private readonly NumericUpDown _page = new() { Minimum = 1, Maximum = 150000, Value = 1000, Width = 80, Increment = 100 };
    private readonly CheckBox _preview = NewCheck("Allow a small preview in Power BI Navigator (leave off on gateways)", false);
    private readonly CheckBox _oleDb = NewCheck("Create an OLE DB data link (.udl) for this data source", true);
    private readonly TextBox _user = NewBox();
    private readonly TextBox _password = NewBox(password: true);
    private readonly Button _testOdbc = new() { Text = "Test ODBC", AutoSize = true };
    private readonly Button _testOleDb = new() { Text = "Test OLE DB", AutoSize = true };
    private readonly Label _testNote = NewText(string.Empty, Theme.Small, Theme.Muted);
    private SetupSession? _session;

    /// <summary>
    /// Initializes the page.
    /// </summary>
    public DataSourcePage()
    {
        _auth.Items.AddRange(["Windows sign-in (Negotiate)", "User name and password (Basic)", "No sign-in (Anonymous)"]);
        _auth.SelectedIndex = 0;
        _create.CheckedChanged += (_, _) => _fields.Enabled = _create.Checked;
        _piWebApi.CheckedChanged += (_, _) => UpdateEnabled();
        _existing.SelectedIndexChanged += (_, _) => LoadExisting();
        _testOdbc.Click += (_, _) => RunTest(oleDb: false);
        _testOleDb.Click += (_, _) => RunTest(oleDb: true);

        AddField(_pickRow, "Data source:", _existing);

        var source = new FlowLayoutPanel { AutoSize = true, Margin = Padding.Empty };
        source.Controls.Add(_piWebApi);
        source.Controls.Add(_testData);
        var limits = new FlowLayoutPanel { AutoSize = true, Margin = Padding.Empty };
        limits.Controls.Add(new Label { Text = "Longest window (days):", AutoSize = true, Margin = new Padding(0, 6, 4, 0) });
        limits.Controls.Add(_days);
        limits.Controls.Add(new Label { Text = "Events per request:", AutoSize = true, Margin = new Padding(16, 6, 4, 0) });
        limits.Controls.Add(_page);
        var tests = new FlowLayoutPanel { AutoSize = true, Margin = Padding.Empty };
        tests.Controls.Add(_testOdbc);
        tests.Controls.Add(_testOleDb);

        var grid = NewGrid();
        AddField(grid, "Name:", _name, "Reports and gateways refer to the data source by this name. Use the same name on every gateway.");
        AddField(grid, "Description:", _description);
        AddField(grid, "Data from:", source);
        AddField(grid, "PI Web API URL:", _url, "For example https://piweb.example.local/piwebapi");
        AddField(grid, "PI data server:", _server);
        AddField(grid, "Sign-in:", _auth);
        AddField(grid, "Limits:", limits);
        AddField(grid, string.Empty, _preview);
        AddField(grid, "OLE DB:", _oleDb, "Uses Microsoft's OLE DB Provider for ODBC Drivers, which is part of Windows. Experimental.");
        AddField(grid, "Test user name:", _user, "Used only for the test and never saved. Leave empty for Windows sign-in.");
        AddField(grid, "Test password:", _password);
        AddField(grid, string.Empty, tests);
        AddField(grid, string.Empty, _testNote);
        _fields.Controls.Add(grid);

        Stack.Controls.Add(_create);
        Stack.Controls.Add(_pickRow);
        Stack.Controls.Add(_fields);
    }

    /// <inheritdoc />
    public override string Title => "Data source";

    /// <inheritdoc />
    public override string Subtitle => "The system data source that reports and programs connect to. Credentials are never stored in it.";

    /// <inheritdoc />
    public override void Prepare(SetupSession session)
    {
        ArgumentNullException.ThrowIfNull(session);
        _session = session;
        var configure = session.Action == SetupAction.Configure;
        _create.Visible = !configure;
        _pickRow.Visible = configure;
        if (configure && _existing.Items.Count == 0)
        {
            _existing.Items.Add(NewDataSource);
            foreach (var name in session.State.DataSources)
            {
                _existing.Items.Add(name);
            }

            _existing.SelectedIndex = session.State.DataSources.Count > 0 ? 1 : 0;
        }

        var installed = session.State.Installed && !session.Demo || session.Demo && configure;
        _testOdbc.Enabled = _testOleDb.Enabled = installed;
        _testNote.Text = installed
            ? "Tests use the settings above without saving them."
            : "Setup tests the connection after installing, with the test user name and password if given.";
        UpdateEnabled();
    }

    /// <inheritdoc />
    public override string? Check()
    {
        if (!_create.Checked && _create.Visible)
        {
            return null;
        }

        var settings = Read();
        if (settings.Validate() is { } problem)
        {
            return problem;
        }

        var owner = _session?.System.ReadValue(Names.DataSourceListKey, settings.Name.Trim());
        return owner is not null && !owner.Equals(Names.DriverName, StringComparison.OrdinalIgnoreCase)
            ? $"A data source named {settings.Name.Trim()} already exists and uses {owner}. Choose another name."
            : null;
    }

    /// <inheritdoc />
    public override void Commit(SetupSession session)
    {
        ArgumentNullException.ThrowIfNull(session);
        session.Answers.CreateDataSource = !_create.Visible || _create.Checked;
        session.Answers.DataSource = Read();
        session.Answers.EditingDataSource = _pickRow.Visible && _existing.SelectedIndex > 0 ? (string)_existing.SelectedItem! : null;
        session.Answers.TestUser = _user.Text.Trim();
        session.Answers.TestPassword = _password.Text;
    }

    private DataSourceSettings Read() => new()
    {
        Name = _name.Text.Trim(),
        Description = _description.Text.Trim(),
        TestData = _testData.Checked,
        BaseUrl = _url.Text.Trim(),
        DataServer = _server.Text.Trim(),
        Auth = AuthValues[Math.Max(0, _auth.SelectedIndex)],
        MaxWindowDays = (int)_days.Value,
        PageSize = (int)_page.Value,
        NavigatorPreview = _preview.Checked,
        OleDbLink = _oleDb.Checked,
    };

    private void LoadExisting()
    {
        if (_session is null)
        {
            return;
        }

        var settings = _existing.SelectedIndex > 0 ? DataSourceSettings.Read(_session.System, (string)_existing.SelectedItem!) : new DataSourceSettings();
        _name.Text = settings.Name;
        _description.Text = settings.Description;
        _testData.Checked = settings.TestData;
        _piWebApi.Checked = !settings.TestData;
        _url.Text = settings.BaseUrl.Length > 0 ? settings.BaseUrl : "https://";
        _server.Text = settings.DataServer;
        _auth.SelectedIndex = Math.Max(0, Array.IndexOf(AuthValues, settings.Auth));
        _days.Value = Math.Clamp(settings.MaxWindowDays, 1, 3660);
        _page.Value = Math.Clamp(settings.PageSize, 1, 150000);
        _preview.Checked = settings.NavigatorPreview;
        _oleDb.Checked = settings.OleDbLink || _existing.SelectedIndex == 0;
    }

    private void UpdateEnabled()
    {
        foreach (var control in new Control[] { _url, _server, _auth, _user, _password })
        {
            control.Enabled = _piWebApi.Checked;
        }
    }

    private void RunTest(bool oleDb)
    {
        if (_session is null)
        {
            return;
        }

        var settings = Read();
        if (settings.Name.Length == 0)
        {
            settings.Name = "Test";
        }

        if (settings.Validate() is { } problem)
        {
            Tell(problem, MessageBoxIcon.Information);
            return;
        }

        UseWaitCursor = true;
        try
        {
            var connection = settings.TestConnectionString(_user.Text.Trim(), _password.Text);
            var result = oleDb ? _session.Tester.TestOleDb(connection) : _session.Tester.TestOdbc(connection);
            Tell(result.Message, result.Ok ? MessageBoxIcon.Information : MessageBoxIcon.Warning);
        }
        finally
        {
            UseWaitCursor = false;
        }

        _ = CultureInfo.CurrentCulture;
    }
}
