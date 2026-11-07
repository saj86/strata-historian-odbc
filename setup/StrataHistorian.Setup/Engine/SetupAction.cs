namespace StrataHistorian.Setup.Engine;

/// <summary>
/// What the setup program does.
/// </summary>
internal enum SetupAction
{
    /// <summary>First installation on this computer.</summary>
    Install = 1,

    /// <summary>Installs this version over the installed one, keeping data sources.</summary>
    Update = 2,

    /// <summary>Adds or changes a data source and its OLE DB data link.</summary>
    Configure = 3,

    /// <summary>Removes the driver, and optionally its data sources.</summary>
    Uninstall = 4,
}
