using StrataHistorian.Setup.Ui;

namespace StrataHistorian.Setup.Pages;

/// <summary>
/// What an uninstall removes, and whether the data sources go too.
/// </summary>
internal sealed class UninstallPage : WizardPage
{
    private readonly Label _body = NewText(string.Empty);
    private readonly CheckBox _removeSources = new() { AutoSize = true, ForeColor = Theme.Danger, Font = Theme.Bold, Margin = new Padding(0, 12, 0, 0) };
    private readonly Label _sourcesHelp = NewText("Reports, gateways and programs that use these data sources will stop working. Leave this off to keep them for a later installation.", Theme.Small, Theme.Danger);

    /// <summary>
    /// Initializes the page.
    /// </summary>
    public UninstallPage()
    {
        _sourcesHelp.Margin = new Padding(20, 0, 0, 4);
        Stack.Controls.Add(_body);
        Stack.Controls.Add(_removeSources);
        Stack.Controls.Add(_sourcesHelp);
    }

    /// <inheritdoc />
    public override string Title => Texts.Uninstall;

    /// <inheritdoc />
    public override void Prepare(SetupSession session)
    {
        ArgumentNullException.ThrowIfNull(session);
        var state = session.State;
        _body.Text = $"Setup removes the driver from {state.InstallDir}, unregisters it from Windows, and removes its Start menu shortcuts and its Programs and Features entry.";
        _removeSources.Text = "Also remove the data sources that use this driver: " + string.Join(", ", state.DataSources);
        _removeSources.Visible = _sourcesHelp.Visible = state.DataSources.Count > 0;
    }

    /// <inheritdoc />
    public override void Commit(SetupSession session)
    {
        ArgumentNullException.ThrowIfNull(session);
        session.Answers.RemoveDataSources = _removeSources.Visible && _removeSources.Checked;
    }
}
