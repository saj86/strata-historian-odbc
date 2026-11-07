namespace StrataHistorian.Setup.Engine;

/// <summary>
/// The names and registry keys the setup program uses. Driver registration matches installer/strataodbc.wxs.
/// </summary>
internal static class Names
{
    /// <summary>The product name.</summary>
    public const string Product = "Strata Historian ODBC Driver";

    /// <summary>The publisher shown in Programs and Features.</summary>
    public const string Publisher = "Strata Historian project";

    /// <summary>The ODBC driver name, as data sources and connection strings refer to it.</summary>
    public const string DriverName = "Strata Historian ODBC Driver";

    /// <summary>The driver file.</summary>
    public const string DriverFile = "strataodbc.dll";

    /// <summary>The copy of the setup program kept in the program folder for later changes and removal.</summary>
    public const string MaintenanceExe = "StrataSetup.exe";

    /// <summary>The Start menu folder and the folder name under Program Files and Public Documents.</summary>
    public const string FolderName = "Strata Historian ODBC";

    /// <summary>The on-premises data gateway's Windows service.</summary>
    public const string GatewayService = "PBIEgwService";

    /// <summary>Registered ODBC drivers.</summary>
    public const string OdbcInst = @"SOFTWARE\ODBC\ODBCINST.INI";

    /// <summary>System data sources.</summary>
    public const string OdbcIni = @"SOFTWARE\ODBC\ODBC.INI";

    /// <summary>The Programs and Features entry.</summary>
    public const string UninstallKey = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\StrataHistorianOdbc";

    /// <summary>What this setup program installed.</summary>
    public const string SetupKey = @"SOFTWARE\Strata Historian ODBC\Setup";

    /// <summary>The project's home page.</summary>
    public const string HomePage = "https://github.com/saj86/strata-historian-odbc";

    /// <summary>Gets the driver's registry key.</summary>
    public static string DriverKey => OdbcInst + @"\" + DriverName;

    /// <summary>Gets the list of registered drivers.</summary>
    public static string DriverListKey => OdbcInst + @"\ODBC Drivers";

    /// <summary>Gets the list of system data sources.</summary>
    public static string DataSourceListKey => OdbcIni + @"\ODBC Data Sources";
}
