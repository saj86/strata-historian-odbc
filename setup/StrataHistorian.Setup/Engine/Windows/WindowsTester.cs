using System.Data.Odbc;
using System.Data.OleDb;
using System.Runtime.Versioning;

namespace StrataHistorian.Setup.Engine.Windows;

/// <summary>
/// Tests connections through the real ODBC driver manager and OLE DB.
/// </summary>
[SupportedOSPlatform("windows")]
internal sealed class WindowsTester : IConnectionTester
{
    /// <inheritdoc />
    public TestResult TestOdbc(string connectionString)
    {
        try
        {
            using var connection = new OdbcConnection(connectionString) { ConnectionTimeout = 20 };
            connection.Open();
            return new TestResult(true, $"ODBC: connected through {connection.Driver}, version {connection.ServerVersion}, data server {connection.DataSource}.");
        }
        catch (OdbcException exception)
        {
            var error = exception.Errors.Count > 0 ? exception.Errors[0] : null;
            return new TestResult(false, "ODBC: " + (error is null ? exception.Message : $"{error.Message} (SQLSTATE {error.SQLState})"));
        }
        catch (InvalidOperationException exception)
        {
            return new TestResult(false, "ODBC: " + exception.Message);
        }
    }

    /// <inheritdoc />
    public TestResult TestOleDb(string odbcConnectionString)
    {
        ArgumentNullException.ThrowIfNull(odbcConnectionString);
        try
        {
            var inner = odbcConnectionString.Replace("\"", "\"\"", StringComparison.Ordinal);
            using var connection = new OleDbConnection("Provider=MSDASQL;Extended Properties=\"" + inner + "\"");
            connection.Open();
            return new TestResult(true, $"OLE DB: connected through {connection.Provider}, driver version {connection.ServerVersion}.");
        }
        catch (OleDbException exception)
        {
            return new TestResult(false, "OLE DB: " + exception.Message);
        }
        catch (InvalidOperationException exception)
        {
            return new TestResult(false, "OLE DB: " + exception.Message);
        }
    }
}
