namespace StrataHistorian.Setup.Engine;

/// <summary>
/// The answers given on the wizard's pages. Test credentials are never stored.
/// </summary>
internal sealed class SetupAnswers
{
    /// <summary>Gets or sets the program folder of a new installation.</summary>
    public string InstallDir { get; set; } = string.Empty;

    /// <summary>Gets or sets a value indicating whether the documents and examples are installed.</summary>
    public bool InstallDocs { get; set; } = true;

    /// <summary>Gets or sets a value indicating whether Start menu shortcuts are created.</summary>
    public bool Shortcuts { get; set; } = true;

    /// <summary>Gets or sets a value indicating whether a data source is created or changed.</summary>
    public bool CreateDataSource { get; set; } = true;

    /// <summary>Gets or sets the data source; when changing an existing one, its name before the change.</summary>
    public DataSourceSettings DataSource { get; set; } = new();

    /// <summary>Gets or sets the existing data source being changed, or null for a new one.</summary>
    public string? EditingDataSource { get; set; }

    /// <summary>Gets or sets a value indicating whether the gateway service is stopped during the run and started again.</summary>
    public bool RestartGateway { get; set; } = true;

    /// <summary>Gets or sets a value indicating whether uninstalling also removes this driver's data sources.</summary>
    public bool RemoveDataSources { get; set; }

    /// <summary>Gets or sets the user name for the test after installing; never stored.</summary>
    public string TestUser { get; set; } = string.Empty;

    /// <summary>Gets or sets the password for the test after installing; never stored.</summary>
    public string TestPassword { get; set; } = string.Empty;
}
