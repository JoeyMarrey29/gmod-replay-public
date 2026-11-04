namespace GmodDemo.Parser;

/// <summary>
/// Layout constants for the Source 1 demo container as used by Garry's Mod.
/// </summary>
public static class DemoFormat
{
    public const int HeaderSize = 1072;
    public const int MagicSize = 8;

    /// <summary>Path-like header fields are fixed-width MAX_OSPATH buffers, NUL-padded.</summary>
    public const int MaxOsPath = 260;

    /// <summary>Stock Source magic, per Valve's demoformat.h.</summary>
    public static ReadOnlySpan<byte> SourceMagic => "HL2DEMO\0"u8;

    /// <summary>
    /// Garry's Mod stamps a renamed magic. Confirmed by inspection of a demo recorded on the
    /// 2026-08 build: bytes 0-7 are 47 4D 4F 44 45 4D 4F 00. The rest of the container is
    /// byte-for-byte identical to stock demo protocol 3.
    /// </summary>
    public static ReadOnlySpan<byte> GmodMagic => "GMODEMO\0"u8;

    /// <summary>Demo protocol 3. Protocol 4 adds a player-slot byte and a splitscreen command-info array.</summary>
    public const int SupportedDemoProtocol = 3;

    /// <summary>Garry's Mod has reported network protocol 24 across all builds to date.</summary>
    public const int ExpectedNetworkProtocol = 24;

    /// <summary>flags(4) + six 3-float vectors. Protocol 3 writes exactly one of these per packet.</summary>
    public const int CommandInfoSize = 76;

    /// <summary>
    /// Rejection threshold for length fields. GMod raises NET_MAX_PAYLOAD well above the stock
    /// value (260,826 bytes observed in a single packet), so this is deliberately loose: it exists
    /// only to stop a corrupt length from driving a huge allocation, not to validate real packets.
    /// </summary>
    public const int MaxPayloadBytes = 16 * 1024 * 1024;
}

/// <summary>
/// Demo protocol 3 command values. Protocol 4 inserts dem_customdata at 8 and shifts
/// StringTables to 9; that variant is not applicable to Garry's Mod.
/// </summary>
public enum DemoCommandType : byte
{
    Signon = 1,
    Packet = 2,
    SyncTick = 3,
    ConsoleCmd = 4,
    UserCmd = 5,
    DataTables = 6,
    Stop = 7,
    StringTables = 8,
}

[Flags]
public enum DemoCommandInfoFlags
{
    Normal = 0,
    UseOrigin2 = 1 << 0,
    UseAngles2 = 1 << 1,
    NoInterp = 1 << 2,
}
