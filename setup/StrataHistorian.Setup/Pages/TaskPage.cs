using StrataHistorian.Setup.Engine;
using StrataHistorian.Setup.Ui;

namespace StrataHistorian.Setup.Pages;

/// <summary>
/// Chooses the task: install when the driver isn't installed; otherwise update, set up a data source, or uninstall.
/// </summary>
internal sealed class TaskPage : WizardPage
{
    private readonly Dictionary<SetupAction, RadioButton> _options = [];
    private readonly Dictionary<SetupAction, Label> _help = [];

    /// <summary>
    /// Initializes the page.
    /// </summary>
    public TaskPage()
    {
        Add(SetupAction.Install, Texts.Install, "Copies the driver, registers it with Windows and the ODBC Data Sources tool, and adds it to Programs and Features.");
        Add(SetupAction.Update, Texts.Update, string.Empty);
        Add(SetupAction.Configure, Texts.Configure, "Adds or changes a system data source, with its OLE DB data link, and tests the connection.");
        Add(SetupAction.Uninstall, Texts.Uninstall, "Removes the driver, its shortcuts and its Programs and Features entry. Data sources are kept unless you choose to remove them.");
    }

    /// <inheritdoc />
    public override string Title => "What would you like to do?";

    /// <inheritdoc />
    public override bool CanGoNext => _options.Values.Any(option => option.Checked && option.Enabled);

    /// <inheritdoc />
    public override void Prepare(SetupSession session)
    {
        ArgumentNullException.ThrowIfNull(session);
        var installed = session.State.Installed;
        _help[SetupAction.Update].Text = installed
            ? $"Replaces version {session.State.Version} with version {session.Version}. Data sources and settings are kept."
            : "Replaces the installed version with this one. Data sources and settings are kept.";
        foreach (var (action, option) in _options)
        {
            option.Enabled = action == SetupAction.Install ? !installed : installed;
        }

        if (session.RequestedAction is { } requested && _options[requested].Enabled)
        {
            _options[requested].Checked = true;
            session.RequestedAction = null;
        }

        if (!_options.Values.Any(option => option.Checked && option.Enabled))
        {
            _options[installed ? SetupAction.Configure : SetupAction.Install].Checked = true;
        }
    }

    /// <inheritdoc />
    public override void Commit(SetupSession session)
    {
        ArgumentNullException.ThrowIfNull(session);
        var chosen = _options.First(pair => pair.Value.Checked).Key;
        if (chosen != session.Action)
        {
            session.ResetAnswers();
        }

        session.Action = chosen;
    }

    private void Add(SetupAction action, string title, string help)
    {
        var option = new RadioButton { Text = title, Font = Theme.Bold, AutoSize = true, Margin = new Padding(0, 10, 0, 0) };
        option.CheckedChanged += (_, _) => OnStateChanged();
        _options[action] = option;
        Stack.Controls.Add(option);
        var text = NewText(help, Theme.Small, Theme.Muted);
        text.Margin = new Padding(20, 0, 0, 4);
        _help[action] = text;
        Stack.Controls.Add(text);
    }
}
