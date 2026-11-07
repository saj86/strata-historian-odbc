using System.Text;
using StrataHistorian.Setup.Ui;

namespace StrataHistorian.Setup.Pages;

/// <summary>
/// The run: the current step, a progress bar, and the log as it is written.
/// </summary>
internal sealed class ProgressPage : WizardPage
{
    private readonly Label _stage = NewText("Starting…", Theme.Bold);
    private readonly ProgressBar _bar = new() { Width = ContentWidth, Height = 22, Minimum = 0, Maximum = 100, Style = ProgressBarStyle.Continuous };
    private readonly Button _toggle = new() { Text = "Hide details", AutoSize = true };
    private readonly TextBox _log = new() { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Both, WordWrap = false, Width = ContentWidth, Height = 260, Font = Theme.Mono, BackColor = Color.White };
    private readonly StringBuilder _all = new();

    /// <summary>
    /// Initializes the page.
    /// </summary>
    public ProgressPage()
    {
        _toggle.Click += (_, _) =>
        {
            _log.Visible = !_log.Visible;
            _toggle.Text = _log.Visible ? "Hide details" : "Show details";
        };
        Stack.Controls.Add(_stage);
        Stack.Controls.Add(_bar);
        Stack.Controls.Add(_toggle);
        Stack.Controls.Add(_log);
    }

    /// <inheritdoc />
    public override string Title => "Working";

    /// <inheritdoc />
    public override string Subtitle => "Please wait. This takes a few seconds.";

    /// <summary>Gets the whole log.</summary>
    public string LogText
    {
        get
        {
            lock (_all)
            {
                return _all.ToString();
            }
        }
    }

    /// <summary>
    /// Shows a step; safe to call from any thread.
    /// </summary>
    /// <param name="percent">The share done.</param>
    /// <param name="text">The step.</param>
    public void ShowStage(int percent, string text)
    {
        if (InvokeRequired)
        {
            BeginInvoke(() => ShowStage(percent, text));
            return;
        }

        _bar.Value = Math.Clamp(Math.Max(_bar.Value, percent), 0, 100);
        _stage.Text = text;
    }

    /// <summary>
    /// Shows a log line; safe to call from any thread.
    /// </summary>
    /// <param name="line">The line.</param>
    public void AppendLine(string line)
    {
        if (InvokeRequired)
        {
            BeginInvoke(() => AppendLine(line));
            return;
        }

        lock (_all)
        {
            _all.AppendLine(line);
        }

        _log.AppendText(line + Environment.NewLine);
    }
}
