using System.Text;
using StrataHistorian.Setup.Ui;

namespace StrataHistorian.Setup.Pages;

/// <summary>
/// The licence terms and their acceptance.
/// </summary>
internal sealed class LicensePage : WizardPage
{
    private readonly TextBox _text = new() { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, Width = ContentWidth, Height = 300, BackColor = Color.White };
    private readonly CheckBox _accept = NewCheck("I accept these terms", false);

    /// <summary>
    /// Initializes the page.
    /// </summary>
    public LicensePage()
    {
        _accept.CheckedChanged += (_, _) => OnStateChanged();
        Stack.Controls.Add(_text);
        Stack.Controls.Add(_accept);
    }

    /// <inheritdoc />
    public override string Title => "Licence terms";

    /// <inheritdoc />
    public override string Subtitle => "Please read these terms, including the note about PI licensing.";

    /// <inheritdoc />
    public override bool CanGoNext => _accept.Checked;

    /// <inheritdoc />
    public override void Prepare(SetupSession session)
    {
        ArgumentNullException.ThrowIfNull(session);
        var file = session.Payload?.Files.FirstOrDefault(f => f.Path.Equals("License.txt", StringComparison.OrdinalIgnoreCase));
        _text.Text = file is null ? "No licence text was found in this setup program." : Encoding.UTF8.GetString(file.Read()).Replace("\r\n", "\n", StringComparison.Ordinal).Replace("\n", "\r\n", StringComparison.Ordinal);
        _text.SelectionStart = 0;
    }
}
