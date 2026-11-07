namespace StrataHistorian.Setup.Ui;

/// <summary>
/// Colours and fonts of the setup program: blue on a light grey-blue, with Segoe UI.
/// </summary>
internal static class Theme
{
    /// <summary>The brand colour.</summary>
    public static readonly Color Brand = Color.FromArgb(0x1F, 0x5F, 0x8B);

    /// <summary>The dark brand colour of titles.</summary>
    public static readonly Color BrandDeep = Color.FromArgb(0x16, 0x3F, 0x5C);

    /// <summary>The soft brand colour of lines and highlighted areas.</summary>
    public static readonly Color BrandSoft = Color.FromArgb(0xE4, 0xEE, 0xF5);

    /// <summary>The window background.</summary>
    public static readonly Color Background = Color.FromArgb(0xF7, 0xF9, 0xFB);

    /// <summary>Body text.</summary>
    public static readonly Color Ink = Color.FromArgb(0x22, 0x28, 0x2E);

    /// <summary>Secondary text.</summary>
    public static readonly Color Muted = Color.FromArgb(0x5B, 0x67, 0x72);

    /// <summary>Success.</summary>
    public static readonly Color Success = Color.FromArgb(0x1E, 0x6E, 0x3C);

    /// <summary>Warnings.</summary>
    public static readonly Color Warning = Color.FromArgb(0x7A, 0x4E, 0x12);

    /// <summary>Errors and destructive choices.</summary>
    public static readonly Color Danger = Color.FromArgb(0xA2, 0x3B, 0x2A);

    /// <summary>Body text.</summary>
    public static readonly Font Body = new("Segoe UI", 9f);

    /// <summary>Bold body text.</summary>
    public static readonly Font Bold = new("Segoe UI", 9f, FontStyle.Bold);

    /// <summary>Page titles.</summary>
    public static readonly Font Title = new("Segoe UI", 12f, FontStyle.Bold);

    /// <summary>Help text.</summary>
    public static readonly Font Small = new("Segoe UI", 8f);

    /// <summary>The log.</summary>
    public static readonly Font Mono = new("Consolas", 9f);
}
