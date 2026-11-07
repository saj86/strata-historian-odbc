using StrataHistorian.Setup.Engine;
using StrataHistorian.Setup.Ui;

namespace StrataHistorian.Setup.Pages;

/// <summary>
/// The first page: what the program does, its version, and what is installed on this computer.
/// </summary>
internal sealed class WelcomePage : WizardPage
{
    private readonly Label _version = NewText(string.Empty, Theme.Bold);
    private readonly Label _status = NewText("Checking this computer…", null, Theme.Muted);
    private readonly Label _demo = NewText("Demo mode: nothing on this computer is changed, and no server is contacted.", Theme.Bold, Theme.Warning);
    private bool _ready;

    /// <summary>
    /// Initializes the page.
    /// </summary>
    public WelcomePage()
    {
        _demo.Visible = false;
        Stack.Controls.Add(_demo);
        Stack.Controls.Add(NewText(
            "This program installs the Strata Historian ODBC Driver, which lets Power BI, Excel, PowerShell, C# and other " +
            "programs read PI data through PI Web API with read-only SQL. It registers the driver with Windows, can set up a " +
            "data source and an OLE DB data link for it, and tests the connection."));
        Stack.Controls.Add(NewText("Later, run it again from the Start menu or from Programs and Features to update the driver, set up more data sources, or uninstall it.", Theme.Small, Theme.Muted));
        _version.Margin = new Padding(0, 14, 0, 4);
        Stack.Controls.Add(_version);
        Stack.Controls.Add(_status);
    }

    /// <inheritdoc />
    public override string Title => "Welcome";

    /// <inheritdoc />
    public override string Subtitle => "Strata Historian ODBC Driver for PI data";

    /// <inheritdoc />
    public override bool CanGoNext => _ready;

    /// <summary>
    /// Reads the payload and what is installed.
    /// </summary>
    /// <param name="session">The session.</param>
    /// <returns>A task that completes when the page shows the state.</returns>
    public async Task LoadAsync(SetupSession session)
    {
        ArgumentNullException.ThrowIfNull(session);
        _demo.Visible = session.Demo;
        if (!session.Demo && !Payload.IsAvailable)
        {
            Problem("This setup program was built without the driver. Build it with scripts/build-setup.sh.");
            return;
        }

        try
        {
            await session.LoadAsync();
        }
        catch (Exception exception) when (exception is InvalidOperationException or IOException or InvalidDataException or UnauthorizedAccessException or System.Security.SecurityException)
        {
            Problem("Setup couldn't read this computer's settings: " + exception.Message);
            return;
        }

        _version.Text = "This setup installs version " + session.Version + ".";
        var state = session.State;
        if (state.RegisteredElsewhere)
        {
            Problem($"The driver is already registered from {state.DriverPath}, by the MSI installer or a script. " +
                    "Uninstall it from Settings > Apps (or Programs and Features), then run this setup again.");
            return;
        }

        _status.ForeColor = Theme.Ink;
        _status.Text = state.Installed
            ? $"Installed: version {state.Version} in {state.InstallDir}." + (state.DataSources.Count > 0 ? " Data sources: " + string.Join(", ", state.DataSources) + "." : " No data sources yet.")
            : "The driver isn't installed on this computer yet.";
        _ready = true;
        OnStateChanged();
    }

    private void Problem(string text)
    {
        _status.ForeColor = Theme.Danger;
        _status.Text = text;
        OnStateChanged();
    }
}
