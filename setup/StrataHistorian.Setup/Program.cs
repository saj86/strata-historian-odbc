using StrataHistorian.Setup.Engine;
using StrataHistorian.Setup.Engine.Windows;
using StrataHistorian.Setup.Ui;

namespace StrataHistorian.Setup;

/// <summary>
/// The entry point of the setup program.
/// </summary>
internal static class Program
{
    private const string FromTemp = "--from-temp";

    /// <summary>
    /// Shows the setup wizard. <c>--demo</c> simulates a computer without the driver, <c>--demo-installed</c> one with
    /// it; neither changes anything. <c>--uninstall</c> opens with Uninstall chosen, as Programs and Features does.
    /// </summary>
    /// <param name="args">The command-line arguments.</param>
    [STAThread]
    private static void Main(string[] args)
    {
        var demoInstalled = args.Contains("--demo-installed", StringComparer.OrdinalIgnoreCase);
        var demo = demoInstalled || args.Contains("--demo", StringComparer.OrdinalIgnoreCase);
        if (!demo && RelaunchFromTemp(args))
        {
            return;
        }

        ApplicationConfiguration.Initialize();
        var session = new SetupSession { Demo = demo, DemoInstalled = demoInstalled };
        if (args.Contains("--uninstall", StringComparer.OrdinalIgnoreCase))
        {
            session.RequestedAction = SetupAction.Uninstall;
        }

        using var form = new SetupForm(session);
        Application.Run(form);
    }

    /// <summary>
    /// The copy kept in the program folder can't replace or delete that folder while it runs from it, so it starts a
    /// temporary copy of itself instead.
    /// </summary>
    private static bool RelaunchFromTemp(string[] args)
    {
        if (args.Contains(FromTemp, StringComparer.OrdinalIgnoreCase) || Environment.ProcessPath is not { } self || !OperatingSystem.IsWindows())
        {
            return false;
        }

        var installDir = new WindowsSystem().ReadValue(Names.SetupKey, "InstallDir");
        if (string.IsNullOrEmpty(installDir) || !string.Equals(Path.GetDirectoryName(self), installDir.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        var folder = Path.Combine(Path.GetTempPath(), "StrataSetup-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(folder);
        var copy = Path.Combine(folder, Names.MaintenanceExe);
        File.Copy(self, copy);
        var start = new System.Diagnostics.ProcessStartInfo(copy) { UseShellExecute = false };
        foreach (var arg in args)
        {
            start.ArgumentList.Add(arg);
        }

        start.ArgumentList.Add(FromTemp);
        System.Diagnostics.Process.Start(start)?.Dispose();
        return true;
    }
}
