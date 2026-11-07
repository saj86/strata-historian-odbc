using System.Globalization;
using StrataHistorian.Setup.Engine;
using StrataHistorian.Setup.Pages;

namespace StrataHistorian.Setup.Ui;

/// <summary>
/// The setup window: a header with the page title, the page, and Back / Next / Cancel, like a Windows installer.
/// </summary>
internal sealed class SetupForm : Form
{
    private readonly SetupSession _session;
    private readonly Label _title = new() { AutoSize = true, Font = Theme.Title, ForeColor = Theme.BrandDeep };
    private readonly Label _subtitle = new() { AutoSize = true, MaximumSize = new Size(590, 0), ForeColor = Theme.Muted };
    private readonly Label _step = new() { AutoSize = true, Font = Theme.Small, ForeColor = Theme.Muted };
    private readonly Label _product = new() { AutoSize = true, ForeColor = Theme.Muted, Font = Theme.Small, Location = new Point(20, 20) };
    private readonly Panel _content = new() { Dock = DockStyle.Fill };
    private readonly Button _back = new() { Text = Texts.Back, Width = 96, Height = 30 };
    private readonly Button _next = new() { Text = Texts.Next, Width = 120, Height = 30 };
    private readonly Button _cancel = new() { Text = Texts.Cancel, Width = 96, Height = 30 };
    private readonly WelcomePage _welcome = new();
    private readonly TaskPage _task = new();
    private readonly LicensePage _license = new();
    private readonly LocationPage _location = new();
    private readonly DataSourcePage _dataSource = new();
    private readonly UninstallPage _uninstall = new();
    private readonly SummaryPage _summary = new();
    private readonly ProgressPage _progress = new();
    private readonly FinishPage _finish = new();
    private List<WizardPage> _flow;
    private int _index;
    private bool _busy;
    private bool _finished;

    /// <summary>
    /// Initializes the window.
    /// </summary>
    /// <param name="session">The session.</param>
    public SetupForm(SetupSession session)
    {
        _session = session;
        _flow = [_welcome, _task];
        Text = Texts.WindowTitle + (session.Demo ? " (demo)" : string.Empty);
        Font = Theme.Body;
        BackColor = Theme.Background;
        if (Environment.ProcessPath is { } program)
        {
            Icon = Icon.ExtractAssociatedIcon(program);
        }

        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(800, 600);
        AutoScaleMode = AutoScaleMode.Dpi;

        Controls.Add(_content);
        Controls.Add(BuildFooter());
        Controls.Add(BuildHeader());
        foreach (var page in new WizardPage[] { _welcome, _task, _license, _location, _dataSource, _uninstall, _summary, _progress, _finish })
        {
            page.StateChanged += (_, _) => RefreshButtons();
        }

        _back.Click += (_, _) => Go(-1);
        _next.Click += async (_, _) => await NextAsync();
        _cancel.Click += (_, _) => Close();
        Shown += async (_, _) =>
        {
            await _welcome.LoadAsync(_session);
            _product.Text = Names.Product + (_session.Version.Length > 0 ? " " + _session.Version : string.Empty);
        };
        FormClosing += OnClosing;
        ShowPage();
    }

    private WizardPage Current => _flow[_index];

    private Panel BuildHeader()
    {
        var header = new Panel { Dock = DockStyle.Top, Height = 84, BackColor = Color.White };
        var logo = new PictureBox { Size = new Size(52, 52), SizeMode = PictureBoxSizeMode.Zoom, Location = new Point(20, 16) };
        using (var stream = typeof(SetupForm).Assembly.GetManifestResourceStream("StrataHistorian.Setup.logo.png"))
        {
            if (stream is not null)
            {
                logo.Image = Image.FromStream(stream);
            }
        }

        var text = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoSize = true, Location = new Point(84, 12), BackColor = Color.White };
        text.Controls.Add(_title);
        text.Controls.Add(_subtitle);
        _step.Location = new Point(700, 8);
        header.Controls.Add(logo);
        header.Controls.Add(text);
        header.Controls.Add(_step);
        header.Controls.Add(new Panel { Dock = DockStyle.Bottom, Height = 2, BackColor = Theme.BrandSoft });
        return header;
    }

    private Panel BuildFooter()
    {
        var footer = new Panel { Dock = DockStyle.Bottom, Height = 56, BackColor = Color.White };
        var buttons = new FlowLayoutPanel { FlowDirection = FlowDirection.RightToLeft, Dock = DockStyle.Right, Width = 360, Padding = new Padding(0, 12, 12, 0) };
        buttons.Controls.Add(_cancel);
        buttons.Controls.Add(_next);
        buttons.Controls.Add(_back);
        _next.BackColor = Theme.Brand;
        _next.ForeColor = Color.White;
        _next.FlatStyle = FlatStyle.Flat;
        footer.Controls.Add(buttons);
        footer.Controls.Add(_product);
        footer.Controls.Add(new Panel { Dock = DockStyle.Top, Height = 1, BackColor = Theme.BrandSoft });
        return footer;
    }

    private void ShowPage()
    {
        var page = Current;
        page.Prepare(_session);
        _content.Controls.Clear();
        _content.Controls.Add(page);
        _title.Text = page.Title;
        _subtitle.Text = page.Subtitle;
        var steps = _flow.Count(item => item is not ProgressPage and not FinishPage);
        _step.Text = page is ProgressPage or FinishPage || _flow.Count <= 2 ? string.Empty : string.Format(CultureInfo.CurrentCulture, Texts.StepOf, _index + 1, steps);
        AcceptButton = _next;
        RefreshButtons();
    }

    private void RefreshButtons()
    {
        var page = Current;
        _back.Visible = !_finished && page is not ProgressPage;
        _back.Enabled = !_busy && _index > 0;
        _next.Visible = !_finished && page is not ProgressPage;
        _next.Enabled = !_busy && page.CanGoNext;
        _next.Text = page.NextText ?? Texts.Next;
        _cancel.Text = _finished ? Texts.Close : Texts.Cancel;
        _cancel.Enabled = !_busy;
    }

    private void Go(int step)
    {
        _index = Math.Clamp(_index + step, 0, _flow.Count - 1);
        ShowPage();
    }

    private async Task NextAsync()
    {
        var page = Current;
        if (page.Check() is { } problem)
        {
            MessageBox.Show(problem, Texts.WindowTitle, MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }

        page.Commit(_session);
        if (page == _task)
        {
            _flow = _session.Action switch
            {
                SetupAction.Install => [_welcome, _task, _license, _location, _dataSource, _summary, _progress, _finish],
                SetupAction.Update => [_welcome, _task, _summary, _progress, _finish],
                SetupAction.Configure => [_welcome, _task, _dataSource, _summary, _progress, _finish],
                _ => [_welcome, _task, _uninstall, _summary, _progress, _finish],
            };
        }

        if (page == _summary)
        {
            await RunAsync();
            return;
        }

        Go(1);
    }

    private async Task RunAsync()
    {
        _busy = true;
        Go(1);
        var ok = await _session.RunAsync(_progress.ShowStage, _progress.AppendLine);
        _busy = false;
        _finished = true;
        _finish.Show(_session, ok, _progress.LogText);
        Go(1);
    }

    private void OnClosing(object? sender, FormClosingEventArgs e)
    {
        if (_busy)
        {
            MessageBox.Show(Texts.Busy, Texts.WindowTitle, MessageBoxButtons.OK, MessageBoxIcon.Information);
            e.Cancel = true;
            return;
        }

        if (!_finished && _index > 1 && MessageBox.Show(Texts.ConfirmExit, Texts.WindowTitle, MessageBoxButtons.YesNo, MessageBoxIcon.Question, MessageBoxDefaultButton.Button2) != DialogResult.Yes)
        {
            e.Cancel = true;
        }
    }
}
