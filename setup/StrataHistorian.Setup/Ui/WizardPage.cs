namespace StrataHistorian.Setup.Ui;

/// <summary>
/// One page of the wizard. Pages are laid out top to bottom in <see cref="Stack"/>; the window shows the title and
/// the buttons.
/// </summary>
internal abstract class WizardPage : UserControl
{
    /// <summary>The width of text and fields on a page.</summary>
    protected const int ContentWidth = 700;

    /// <summary>
    /// Initializes the page.
    /// </summary>
    protected WizardPage()
    {
        Dock = DockStyle.Fill;
        BackColor = Theme.Background;
        Font = Theme.Body;
        ForeColor = Theme.Ink;
        Padding = new Padding(28, 16, 28, 8);
        Stack = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoScroll = true };
        Controls.Add(Stack);
    }

    /// <summary>Raised when the Next button should be enabled or disabled again.</summary>
    public event EventHandler? StateChanged;

    /// <summary>Gets the page title.</summary>
    public abstract string Title { get; }

    /// <summary>Gets the line under the title.</summary>
    public virtual string Subtitle => string.Empty;

    /// <summary>Gets the text of the Next button, or null for "Next".</summary>
    public virtual string? NextText => null;

    /// <summary>Gets a value indicating whether the user can go on.</summary>
    public virtual bool CanGoNext => true;

    /// <summary>Gets the column the page's controls are added to.</summary>
    protected FlowLayoutPanel Stack { get; }

    /// <summary>
    /// Shows the session's values; called every time the page is shown.
    /// </summary>
    /// <param name="session">The session.</param>
    public virtual void Prepare(SetupSession session)
    {
    }

    /// <summary>
    /// Checks the answers before going on.
    /// </summary>
    /// <returns>A message to show, or null to go on.</returns>
    public virtual string? Check() => null;

    /// <summary>
    /// Writes the answers to the session.
    /// </summary>
    /// <param name="session">The session.</param>
    public virtual void Commit(SetupSession session)
    {
    }

    /// <summary>
    /// Raises <see cref="StateChanged"/>.
    /// </summary>
    protected void OnStateChanged() => StateChanged?.Invoke(this, EventArgs.Empty);

    /// <summary>
    /// Creates a wrapping label.
    /// </summary>
    /// <param name="text">The text.</param>
    /// <param name="font">The font, or null for body text.</param>
    /// <param name="color">The colour, or null for body text.</param>
    /// <returns>The label.</returns>
    protected static Label NewText(string text, Font? font = null, Color? color = null) => new()
    {
        Text = text,
        AutoSize = true,
        MaximumSize = new Size(ContentWidth, 0),
        Font = font ?? Theme.Body,
        ForeColor = color ?? Theme.Ink,
        Margin = new Padding(0, 4, 0, 4),
    };

    /// <summary>
    /// Creates a text box.
    /// </summary>
    /// <param name="value">The initial text.</param>
    /// <param name="password">Whether the text is hidden.</param>
    /// <returns>The text box.</returns>
    protected static TextBox NewBox(string? value = null, bool password = false) => new() { Text = value ?? string.Empty, Width = 340, UseSystemPasswordChar = password };

    /// <summary>
    /// Creates a check box.
    /// </summary>
    /// <param name="text">The text.</param>
    /// <param name="value">Whether it is ticked.</param>
    /// <returns>The check box.</returns>
    protected static CheckBox NewCheck(string text, bool value) => new() { Text = text, Checked = value, AutoSize = true, Margin = new Padding(0, 6, 0, 0) };

    /// <summary>
    /// Creates a two-column grid of labelled fields.
    /// </summary>
    /// <returns>The grid.</returns>
    protected static TableLayoutPanel NewGrid()
    {
        var grid = new TableLayoutPanel { ColumnCount = 2, AutoSize = true, Margin = new Padding(0, 4, 0, 4) };
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 190));
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, ContentWidth - 190));
        return grid;
    }

    /// <summary>
    /// Adds a labelled field, with optional help under it.
    /// </summary>
    /// <param name="grid">The grid.</param>
    /// <param name="label">The label.</param>
    /// <param name="field">The field.</param>
    /// <param name="help">Help text, or null.</param>
    protected static void AddField(TableLayoutPanel grid, string label, Control field, string? help = null)
    {
        ArgumentNullException.ThrowIfNull(grid);
        ArgumentNullException.ThrowIfNull(field);
        grid.RowCount++;
        grid.Controls.Add(new Label { Text = label, AutoSize = true, MaximumSize = new Size(180, 0), Margin = new Padding(0, 7, 0, 0) }, 0, grid.RowCount - 1);
        if (help is null)
        {
            grid.Controls.Add(field, 1, grid.RowCount - 1);
            return;
        }

        var cell = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoSize = true, Margin = Padding.Empty };
        cell.Controls.Add(field);
        cell.Controls.Add(new Label { Text = help, AutoSize = true, MaximumSize = new Size(ContentWidth - 200, 0), Font = Theme.Small, ForeColor = Theme.Muted });
        grid.Controls.Add(cell, 1, grid.RowCount - 1);
    }

    /// <summary>
    /// Shows a message box.
    /// </summary>
    /// <param name="text">The text.</param>
    /// <param name="icon">The icon.</param>
    protected static void Tell(string text, MessageBoxIcon icon) =>
        MessageBox.Show(text, Texts.WindowTitle, MessageBoxButtons.OK, icon);
}
