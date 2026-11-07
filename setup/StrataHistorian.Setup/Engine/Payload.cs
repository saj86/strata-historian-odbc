using System.IO.Compression;
using System.Reflection;

namespace StrataHistorian.Setup.Engine;

/// <summary>
/// The payload embedded in the setup program (payload.zip): strataodbc.dll, VERSION.txt, License.txt, docs and samples.
/// </summary>
internal sealed class Payload : IPayload
{
    private const string ResourceName = "StrataHistorian.Setup.payload.zip";

    private Payload(string version, IReadOnlyList<PayloadFile> files)
    {
        Version = version;
        Files = files;
    }

    /// <summary>Gets a value indicating whether this build carries the payload.</summary>
    public static bool IsAvailable => Assembly.GetExecutingAssembly().GetManifestResourceInfo(ResourceName) is not null;

    /// <inheritdoc />
    public string Version { get; }

    /// <inheritdoc />
    public IReadOnlyList<PayloadFile> Files { get; }

    /// <summary>
    /// Reads the embedded payload.
    /// </summary>
    /// <returns>The payload.</returns>
    public static Payload Load()
    {
        using var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(ResourceName)
            ?? throw new InvalidOperationException("This setup program was built without the driver.");
        return FromZip(stream);
    }

    /// <summary>
    /// Reads a payload zip. Paths that would leave the program folder are refused.
    /// </summary>
    /// <param name="zip">The zip.</param>
    /// <returns>The payload.</returns>
    public static Payload FromZip(Stream zip)
    {
        ArgumentNullException.ThrowIfNull(zip);
        using var copy = new MemoryStream();
        zip.CopyTo(copy);
        var bytes = copy.ToArray();
        using var archive = new ZipArchive(new MemoryStream(bytes), ZipArchiveMode.Read);
        var files = new List<PayloadFile>();
        var version = string.Empty;
        foreach (var entry in archive.Entries)
        {
            if (entry.FullName.EndsWith('/'))
            {
                continue;
            }

            var path = entry.FullName.Replace('/', '\\');
            if (path.StartsWith('\\') || path.Contains(':', StringComparison.Ordinal) || path.Split('\\').Contains(".."))
            {
                throw new InvalidDataException($"Unexpected path in the payload: {entry.FullName}");
            }

            var name = entry.FullName;
            byte[] Read()
            {
                using var source = new ZipArchive(new MemoryStream(bytes), ZipArchiveMode.Read);
                using var content = source.GetEntry(name)!.Open();
                using var buffer = new MemoryStream();
                content.CopyTo(buffer);
                return buffer.ToArray();
            }

            files.Add(new PayloadFile(path, Read, entry.Length));
            if (path.Equals("VERSION.txt", StringComparison.OrdinalIgnoreCase))
            {
                version = System.Text.Encoding.UTF8.GetString(Read()).Trim();
            }
        }

        if (!files.Any(f => f.Path.Equals(Names.DriverFile, StringComparison.OrdinalIgnoreCase)))
        {
            throw new InvalidDataException("The payload has no " + Names.DriverFile + ".");
        }

        return new Payload(version, files);
    }
}
