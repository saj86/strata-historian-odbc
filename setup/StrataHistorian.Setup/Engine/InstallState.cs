namespace StrataHistorian.Setup.Engine;

/// <summary>
/// What is installed on this computer.
/// </summary>
internal sealed record InstallState
{
    /// <summary>Gets a value indicating whether this setup program installed the driver.</summary>
    public bool Installed { get; init; }

    /// <summary>
    /// Gets a value indicating whether the driver is registered by something else, such as the MSI or a script.
    /// The two must not both manage the driver, so setup then asks for that one to be removed first.
    /// </summary>
    public bool RegisteredElsewhere { get; init; }

    /// <summary>Gets where the registered driver file is, if any.</summary>
    public string DriverPath { get; init; } = string.Empty;

    /// <summary>Gets the program folder.</summary>
    public string InstallDir { get; init; } = string.Empty;

    /// <summary>Gets the installed version.</summary>
    public string Version { get; init; } = string.Empty;

    /// <summary>Gets the system data sources that use this driver.</summary>
    public IReadOnlyList<string> DataSources { get; init; } = [];

    /// <summary>Gets the state of the on-premises data gateway service, or null when it isn't installed.</summary>
    public string? GatewayState { get; init; }

    /// <summary>
    /// Reads the state.
    /// </summary>
    /// <param name="system">The machine.</param>
    /// <returns>The state.</returns>
    public static InstallState Read(ISystemOps system)
    {
        ArgumentNullException.ThrowIfNull(system);
        var installDir = system.ReadValue(Names.SetupKey, "InstallDir") ?? string.Empty;
        var driver = system.ReadValue(Names.DriverKey, "Driver") ?? string.Empty;
        var ours = installDir.Length > 0 && driver.Equals(installDir.TrimEnd('\\') + @"\" + Names.DriverFile, StringComparison.OrdinalIgnoreCase);
        var sources = system.ValueNames(Names.DataSourceListKey)
            .Where(name => string.Equals(system.ReadValue(Names.DataSourceListKey, name), Names.DriverName, StringComparison.OrdinalIgnoreCase))
            .Order(StringComparer.OrdinalIgnoreCase)
            .ToList();
        return new InstallState
        {
            Installed = ours,
            RegisteredElsewhere = driver.Length > 0 && !ours,
            DriverPath = driver,
            InstallDir = ours ? installDir : string.Empty,
            Version = ours ? system.ReadValue(Names.SetupKey, "Version") ?? string.Empty : string.Empty,
            DataSources = sources,
            GatewayState = system.ServiceState(Names.GatewayService),
        };
    }
}
