namespace StrataHistorian.Setup.Engine;

/// <summary>
/// Tries a connection through the installed driver.
/// </summary>
internal interface IConnectionTester
{
    /// <summary>
    /// Connects through ODBC.
    /// </summary>
    /// <param name="connectionString">An ODBC connection string.</param>
    /// <returns>Whether it worked, and a message.</returns>
    TestResult TestOdbc(string connectionString);

    /// <summary>
    /// Connects through OLE DB, using Microsoft's OLE DB Provider for ODBC Drivers.
    /// </summary>
    /// <param name="odbcConnectionString">The ODBC connection string to pass to the provider.</param>
    /// <returns>Whether it worked, and a message.</returns>
    TestResult TestOleDb(string odbcConnectionString);
}

/// <summary>
/// The outcome of a connection test.
/// </summary>
/// <param name="Ok">Whether the connection worked.</param>
/// <param name="Message">What happened.</param>
internal sealed record TestResult(bool Ok, string Message);
