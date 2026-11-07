using System.Text;

namespace StrataHistorian.Setup.Engine;

/// <summary>
/// OLE DB data link files (.udl). They use Microsoft's OLE DB Provider for ODBC Drivers (MSDASQL), which is part of
/// Windows, to reach a data source through this driver. Double-clicking one opens the Data Link Properties window.
/// </summary>
internal static class UdlFile
{
    /// <summary>
    /// Gets the folder of the data links. Public Documents, so every user can open and save them.
    /// </summary>
    /// <param name="system">The machine.</param>
    /// <returns>The folder.</returns>
    public static string Folder(ISystemOps system)
    {
        ArgumentNullException.ThrowIfNull(system);
        return Path.Join(system.CommonDocuments, Names.FolderName).Replace('/', '\\');
    }

    /// <summary>
    /// Gets the data link file of a data source.
    /// </summary>
    /// <param name="system">The machine.</param>
    /// <param name="dataSource">The data source name.</param>
    /// <returns>The file.</returns>
    public static string PathFor(ISystemOps system, string dataSource) => Folder(system) + @"\" + dataSource + ".udl";

    /// <summary>
    /// Gets the OLE DB connection string of a data source.
    /// </summary>
    /// <param name="dataSource">The data source name.</param>
    /// <returns>The connection string.</returns>
    public static string ConnectionString(string dataSource) => "Provider=MSDASQL.1;Persist Security Info=False;Data Source=" + dataSource;

    /// <summary>
    /// Builds a data link file: UTF-16 with a byte order mark, as Windows writes them.
    /// </summary>
    /// <param name="dataSource">The data source name.</param>
    /// <returns>The file's bytes.</returns>
    public static byte[] Build(string dataSource)
    {
        var text = "[oledb]\r\n; Everything after this line is an OLE DB initstring\r\n" + ConnectionString(dataSource) + "\r\n";
        return [.. Encoding.Unicode.GetPreamble(), .. Encoding.Unicode.GetBytes(text)];
    }
}
