using System.Globalization;

namespace StrataHistorian.Setup.Engine;

/// <summary>
/// One data source (System DSN). The rules match the driver's own settings window (src/odbc/dsn_form.cpp) and its
/// connection rules, and UID and PWD are never stored.
/// </summary>
internal sealed class DataSourceSettings
{
    private const string Forbidden = "[]{}(),;?*=!@\\";

    /// <summary>Gets or sets the data source name.</summary>
    public string Name { get; set; } = "StrataPI";

    /// <summary>Gets or sets the description.</summary>
    public string Description { get; set; } = string.Empty;

    /// <summary>Gets or sets a value indicating whether the data source reads synthetic test data instead of PI.</summary>
    public bool TestData { get; set; }

    /// <summary>Gets or sets the PI Web API address.</summary>
    public string BaseUrl { get; set; } = string.Empty;

    /// <summary>Gets or sets the PI data server.</summary>
    public string DataServer { get; set; } = string.Empty;

    /// <summary>Gets or sets the sign-in: Negotiate, Basic or Anonymous.</summary>
    public string Auth { get; set; } = "Negotiate";

    /// <summary>Gets or sets the longest window in days.</summary>
    public int MaxWindowDays { get; set; } = 31;

    /// <summary>Gets or sets the events per request.</summary>
    public int PageSize { get; set; } = 1000;

    /// <summary>Gets or sets a value indicating whether Power BI Navigator may show a capped preview.</summary>
    public bool NavigatorPreview { get; set; }

    /// <summary>Gets or sets a value indicating whether an OLE DB data link (.udl) is created for this data source.</summary>
    public bool OleDbLink { get; set; } = true;

    /// <summary>
    /// Checks the settings.
    /// </summary>
    /// <returns>A message for the person filling them in, or null when they can be saved.</returns>
    public string? Validate()
    {
        var name = Name.Trim();
        if (name.Length == 0)
        {
            return "Enter a data source name.";
        }

        if (name.Length > 32)
        {
            return "The data source name can be at most 32 characters.";
        }

        if (name.IndexOfAny(Forbidden.ToCharArray()) >= 0)
        {
            return "The data source name can't contain any of these characters: [ ] { } ( ) , ; ? * = ! @ \\";
        }

        if (!TestData)
        {
            if (!Uri.TryCreate(BaseUrl.Trim(), UriKind.Absolute, out var url))
            {
                return "Enter the PI Web API URL, for example https://piweb.example.local/piwebapi.";
            }

            var loopback = url.Scheme == Uri.UriSchemeHttp && (url.Host == "127.0.0.1" || url.Host.Equals("localhost", StringComparison.OrdinalIgnoreCase));
            if (url.Scheme != Uri.UriSchemeHttps && !loopback)
            {
                return "The PI Web API URL must start with https://.";
            }

            if (!string.IsNullOrEmpty(url.UserInfo))
            {
                return "The PI Web API URL must not contain a user name or password.";
            }

            if (DataServer.Trim().Length == 0)
            {
                return "Enter the PI data server name.";
            }

            if (Auth is not ("Negotiate" or "Basic" or "Anonymous"))
            {
                return "Choose a sign-in method.";
            }
        }

        if (MaxWindowDays is < 1 or > 3660)
        {
            return "The longest window must be from 1 to 3660 days.";
        }

        if (PageSize is < 1 or > 150000)
        {
            return "Events per request must be from 1 to 150000.";
        }

        return null;
    }

    /// <summary>
    /// Gets the values stored in the data source, in order. An empty value means the value is removed.
    /// </summary>
    /// <returns>The values; never UID or PWD.</returns>
    public IReadOnlyList<(string Name, string Value)> Attributes() =>
    [
        ("Description", Description.Trim()),
        ("Backend", TestData ? "fixture" : "piwebapi"),
        ("BaseUrl", TestData ? string.Empty : BaseUrl.Trim()),
        ("DataServer", TestData ? string.Empty : DataServer.Trim()),
        ("Auth", TestData ? string.Empty : Auth),
        ("MaxWindowDays", MaxWindowDays.ToString(CultureInfo.InvariantCulture)),
        ("PageSize", PageSize.ToString(CultureInfo.InvariantCulture)),
        ("NavigatorPreview", NavigatorPreview ? "1" : "0"),
    ];

    /// <summary>
    /// Builds an ODBC connection string that uses the settings without saving them, for a test before saving.
    /// </summary>
    /// <param name="user">The user name, or empty.</param>
    /// <param name="password">The password, or empty.</param>
    /// <returns>The connection string.</returns>
    public string TestConnectionString(string user, string password)
    {
        var parts = new List<string> { "DRIVER={" + Names.DriverName + "}" };
        foreach (var (name, value) in Attributes())
        {
            if (value.Length > 0 && name != "Description")
            {
                parts.Add(name + "=" + Brace(value));
            }
        }

        if (user.Length > 0)
        {
            parts.Add("UID=" + Brace(user));
        }

        if (password.Length > 0)
        {
            parts.Add("PWD=" + Brace(password));
        }

        parts.Add("RequestTimeoutMs=15000");
        parts.Add("MaxRetries=0");
        return string.Join(';', parts) + ";";
    }

    /// <summary>
    /// Reads a data source back.
    /// </summary>
    /// <param name="system">The machine.</param>
    /// <param name="name">The data source name.</param>
    /// <returns>Its settings.</returns>
    public static DataSourceSettings Read(ISystemOps system, string name)
    {
        ArgumentNullException.ThrowIfNull(system);
        var key = Names.OdbcIni + @"\" + name;
        string Get(string value) => system.ReadValue(key, value) ?? string.Empty;
        int Number(string value, int fallback) => int.TryParse(Get(value), NumberStyles.Integer, CultureInfo.InvariantCulture, out var n) ? n : fallback;
        var auth = Get("Auth");
        var preview = Get("NavigatorPreview");
        return new DataSourceSettings
        {
            Name = name,
            Description = Get("Description"),
            TestData = Get("Backend").Equals("fixture", StringComparison.OrdinalIgnoreCase),
            BaseUrl = Get("BaseUrl"),
            DataServer = Get("DataServer"),
            Auth = auth.Equals("Basic", StringComparison.OrdinalIgnoreCase) ? "Basic" : auth.Equals("Anonymous", StringComparison.OrdinalIgnoreCase) ? "Anonymous" : "Negotiate",
            MaxWindowDays = Number("MaxWindowDays", 31),
            PageSize = Number("PageSize", 1000),
            NavigatorPreview = preview == "1" || preview.Equals("yes", StringComparison.OrdinalIgnoreCase) || preview.Equals("true", StringComparison.OrdinalIgnoreCase),
            OleDbLink = system.FileExists(UdlFile.PathFor(system, name)),
        };
    }

    private static string Brace(string value) => value.IndexOfAny([';', '{', '}', ' ', '=']) >= 0 ? "{" + value.Replace("}", "}}", StringComparison.Ordinal) + "}" : value;
}
