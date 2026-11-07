using System.Globalization;
using StrataHistorian.Setup.Engine;
using StrataHistorian.Setup.Ui;

namespace StrataHistorian.Setup.Pages;

/// <summary>
/// The program folder and the optional parts of a new installation.
/// </summary>
internal sealed class LocationPage : WizardPage
{
    private readonly TextBox _folder = NewBox();
    private readonly Label _space = NewText(string.Empty, Theme.Small, Theme.Muted);
    private readonly CheckBox _docs = NewCheck("Documents and examples (usage guide, PowerShell, C# and Power BI examples, Power BI project)", true);
    private readonly CheckBox _shortcuts = NewCheck("Start menu shortcuts", true);
    private ISystemOps? _system;

    /// <summary>
    /// Initializes the page.
    /// </summary>
    public LocationPage()
    {
        var browse = new Button { Text = Texts.Browse, AutoSize = true };
        browse.Click += (_, _) =>
        {
            using var dialog = new FolderBrowserDialog { ShowNewFolderButton = true, InitialDirectory = Directory.Exists(_folder.Text) ? _folder.Text : string.Empty };
            if (dialog.ShowDialog(FindForm()) == DialogResult.OK)
            {
                _folder.Text = Path.Combine(dialog.SelectedPath, Names.FolderName);
            }
        };
        var row = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = Padding.Empty };
        _folder.Width = 420;
        row.Controls.Add(_folder);
        row.Controls.Add(browse);
        _folder.TextChanged += (_, _) => ShowSpace();

        var grid = NewGrid();
        AddField(grid, "Program folder:", row, "The driver is registered from here. Use a local folder, such as one under Program Files.");
        Stack.Controls.Add(grid);
        Stack.Controls.Add(_space);
        Stack.Controls.Add(NewText("Also install:", Theme.Bold));
        Stack.Controls.Add(_docs);
        Stack.Controls.Add(_shortcuts);
    }

    /// <inheritdoc />
    public override string Title => "Where to install";

    /// <inheritdoc />
    public override string Subtitle => "Choose the program folder and what to install.";

    /// <inheritdoc />
    public override void Prepare(SetupSession session)
    {
        ArgumentNullException.ThrowIfNull(session);
        _system = session.System;
        if (_folder.Text.Length == 0)
        {
            _folder.Text = session.Answers.InstallDir.Length > 0 ? session.Answers.InstallDir : session.DefaultInstallDir;
        }

        ShowSpace();
    }

    /// <inheritdoc />
    public override string? Check()
    {
        var folder = Clean(_folder.Text);
        if (!Path.IsPathFullyQualified(folder) || folder.StartsWith(@"\\", StringComparison.Ordinal))
        {
            return "Choose a folder on a local drive, such as C:\\Program Files\\Strata Historian ODBC.";
        }

        if (string.Equals(folder, Path.GetPathRoot(folder)?.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
        {
            return "Choose a folder, not the root of a drive.";
        }

        // Uninstalling deletes the program folder, so it must be new, empty, or an earlier copy of this driver.
        if (_system is not null && _system.DirectoryHasEntries(folder) && !_system.FileExists(folder + @"\" + Names.DriverFile))
        {
            return $"{folder} already holds other files. Choose a new or empty folder.";
        }

        return null;
    }

    /// <inheritdoc />
    public override void Commit(SetupSession session)
    {
        ArgumentNullException.ThrowIfNull(session);
        session.Answers.InstallDir = Clean(_folder.Text);
        session.Answers.InstallDocs = _docs.Checked;
        session.Answers.Shortcuts = _shortcuts.Checked;
    }

    private static string Clean(string text) => text.Trim().TrimEnd('\\');

    private void ShowSpace()
    {
        _space.Text = string.Empty;
        try
        {
            var root = Path.IsPathFullyQualified(_folder.Text.Trim()) ? Path.GetPathRoot(_folder.Text.Trim()) : null;
            if (string.IsNullOrEmpty(root) || root.StartsWith(@"\\", StringComparison.Ordinal) || !OperatingSystem.IsWindows())
            {
                return;
            }

            var drive = new DriveInfo(root);
            if (drive.IsReady)
            {
                _space.Text = string.Format(CultureInfo.CurrentCulture, "Free space on {0}: {1:0.0} GB. The driver needs less than 10 MB.", drive.Name.TrimEnd('\\'), drive.AvailableFreeSpace / 1073741824d);
            }
        }
        catch (Exception exception) when (exception is ArgumentException or IOException or UnauthorizedAccessException)
        {
            _space.Text = string.Empty;
        }
    }
}
