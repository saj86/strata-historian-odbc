namespace StrataHistorian.Setup.Engine;

/// <summary>
/// The files this setup program installs.
/// </summary>
internal interface IPayload
{
    /// <summary>Gets the version, such as 0.2.0.</summary>
    string Version { get; }

    /// <summary>Gets the files: path relative to the program folder, with \ separators, and content.</summary>
    IReadOnlyList<PayloadFile> Files { get; }
}

/// <summary>
/// One file to install.
/// </summary>
/// <param name="Path">The path relative to the program folder.</param>
/// <param name="Read">Reads the content.</param>
/// <param name="Length">The size in bytes.</param>
internal sealed record PayloadFile(string Path, Func<byte[]> Read, long Length);
