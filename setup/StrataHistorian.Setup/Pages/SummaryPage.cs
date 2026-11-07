using System.Text;
using StrataHistorian.Setup.Engine;
using StrataHistorian.Setup.Ui;

namespace StrataHistorian.Setup.Pages;

/// <summary>
/// What is about to happen, before the run starts. Passwords are never shown.
/// </summary>
internal sealed class SummaryPage : WizardPage
{
    private readonly TextBox _text = new() { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, Width = ContentWidth, Height = 280, BackColor = Color.White, BorderStyle = BorderStyle.FixedSingle };
    private readonly CheckBox _gateway = NewCheck("Stop the on-premises data gateway during setup, and start it again afterwards", true);
    private readonly Label _gatewayHelp = NewText("The gateway keeps the driver loaded, so it must stop for the driver to be replaced. Scheduled refreshes running at that moment will fail and can be run again.", Theme.Small, Theme.Muted);
    private SetupAction _action;

    /// <summary>
    /// Initializes the page.
    /// </summary>
    public SummaryPage()
    {
        _gatewayHelp.Margin = new Padding(20, 0, 0, 4);
        Stack.Controls.Add(_text);
        Stack.Controls.Add(_gateway);
        Stack.Controls.Add(_gatewayHelp);
    }

    /// <inheritdoc />
    public override string Title => "Ready";

    /// <inheritdoc />
    public override string Subtitle => "Check what setup is about to do.";

    /// <inheritdoc />
    public override string? NextText => _action switch
    {
        SetupAction.Install => "Install",
        SetupAction.Update => "Update",
        SetupAction.Configure => "Save",
        _ => "Uninstall",
    };

    /// <inheritdoc />
    public override void Prepare(SetupSession session)
    {
        ArgumentNullException.ThrowIfNull(session);
        _action = session.Action;
        var a = session.Answers;
        var state = session.State;
        var text = new StringBuilder();
        void Line(string label, string value) => text.Append(label).Append(": ").AppendLine(value);

        Line("Task", _action switch
        {
            SetupAction.Install => Texts.Install,
            SetupAction.Update => Texts.Update,
            SetupAction.Configure => Texts.Configure,
            _ => Texts.Uninstall,
        });
        Line("Version", _action == SetupAction.Update ? $"{state.Version} to {session.Version}" : _action == SetupAction.Install ? session.Version : state.Version);
        Line("Program folder", _action == SetupAction.Install ? a.InstallDir : state.InstallDir);
        if (_action == SetupAction.Install)
        {
            Line("Documents and examples", a.InstallDocs ? "Yes" : "No");
            Line("Start menu shortcuts", a.Shortcuts ? "Yes" : "No");
        }

        if (_action is SetupAction.Install or SetupAction.Configure)
        {
            if (a.CreateDataSource)
            {
                var s = a.DataSource;
                if (a.EditingDataSource is { } old && !old.Equals(s.Name, StringComparison.OrdinalIgnoreCase))
                {
                    Line("Rename data source", $"{old} to {s.Name}");
                }

                Line("Data source", s.Name + (s.TestData ? " (synthetic test data, not PI)" : string.Empty));
                if (!s.TestData)
                {
                    Line("PI Web API", s.BaseUrl);
                    Line("PI data server", s.DataServer);
                    Line("Sign-in", s.Auth);
                }

                Line("Limits", $"{s.MaxWindowDays} days per query, {s.PageSize} events per request" + (s.NavigatorPreview ? ", Navigator preview allowed" : string.Empty));
                Line("OLE DB data link", s.OleDbLink ? UdlFile.PathFor(session.System, s.Name) : "No");
                Line("Connection test", "ODBC" + (s.OleDbLink ? " and OLE DB" : string.Empty) + (a.TestUser.Length > 0 ? ", as " + a.TestUser : string.Empty));
            }
            else
            {
                Line("Data source", "None now. Set one up later from the Start menu or the ODBC Data Sources tool.");
            }
        }

        if (_action == SetupAction.Uninstall)
        {
            Line("Data sources", state.DataSources.Count == 0 ? "None" : (a.RemoveDataSources ? "Remove " : "Keep ") + string.Join(", ", state.DataSources));
        }

        _text.Text = text.ToString();
        _gateway.Visible = _gatewayHelp.Visible = state.GatewayState == "Running" && _action != SetupAction.Configure;
    }

    /// <inheritdoc />
    public override void Commit(SetupSession session)
    {
        ArgumentNullException.ThrowIfNull(session);
        session.Answers.RestartGateway = _gateway.Visible && _gateway.Checked;
    }
}
