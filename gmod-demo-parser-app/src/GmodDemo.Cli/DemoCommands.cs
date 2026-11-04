using System.Buffers.Binary;

namespace GmodDemo.Parser;

public readonly record struct Vec3(float X, float Y, float Z)
{
    public static Vec3 Read(ReadOnlySpan<byte> buffer) => new(
        BinaryPrimitives.ReadSingleLittleEndian(buffer),
        BinaryPrimitives.ReadSingleLittleEndian(buffer[4..]),
        BinaryPrimitives.ReadSingleLittleEndian(buffer[8..]));
}

/// <summary>
/// The 76-byte per-packet recorder camera block (democmdinfo_t).
/// </summary>
public sealed record DemoCommandInfo(
    DemoCommandInfoFlags Flags,
    Vec3 ViewOrigin,
    Vec3 ViewAngles,
    Vec3 LocalViewAngles,
    Vec3 ViewOrigin2,
    Vec3 ViewAngles2,
    Vec3 LocalViewAngles2)
{
    /// <summary>
    /// The engine selects the resampled origin when UseOrigin2 is set, so consumers wanting the
    /// actual camera path must honour the flag rather than reading ViewOrigin directly.
    /// </summary>
    public Vec3 EffectiveViewOrigin =>
        Flags.HasFlag(DemoCommandInfoFlags.UseOrigin2) ? ViewOrigin2 : ViewOrigin;

    public Vec3 EffectiveViewAngles =>
        Flags.HasFlag(DemoCommandInfoFlags.UseAngles2) ? ViewAngles2 : ViewAngles;

    public Vec3 EffectiveLocalViewAngles =>
        Flags.HasFlag(DemoCommandInfoFlags.UseAngles2) ? LocalViewAngles2 : LocalViewAngles;

    public static DemoCommandInfo Parse(ReadOnlySpan<byte> buffer) => new(
        (DemoCommandInfoFlags)BinaryPrimitives.ReadInt32LittleEndian(buffer),
        Vec3.Read(buffer[4..]),
        Vec3.Read(buffer[16..]),
        Vec3.Read(buffer[28..]),
        Vec3.Read(buffer[40..]),
        Vec3.Read(buffer[52..]),
        Vec3.Read(buffer[64..]));
}

/// <summary>
/// A single record in the demo command stream. <see cref="Offset"/> is the file position of the
/// command byte, which makes any command locatable in a hex editor during debugging.
/// </summary>
public abstract record DemoCommand(DemoCommandType Type, int Tick, long Offset);

/// <summary>
/// dem_signon or dem_packet. Both share identical framing; signon records carry the initial
/// state burst and are all written before the header's SignonLength boundary.
/// </summary>
/// <param name="Payload">
/// The undecoded net-message bitstream. Preserved verbatim because message decoding is not
/// yet implemented; interpreting it requires a bit reader, not a byte reader.
/// </param>
public sealed record PacketCommand(
    DemoCommandType Type,
    int Tick,
    long Offset,
    DemoCommandInfo Info,
    int SequenceIn,
    int SequenceOut,
    byte[] Payload) : DemoCommand(Type, Tick, Offset);

/// <summary>dem_synctick. Carries no payload; instructs the client to resync its clock.</summary>
public sealed record SyncTickCommand(int Tick, long Offset)
    : DemoCommand(DemoCommandType.SyncTick, Tick, Offset);

/// <summary>dem_consolecmd. The only command whose payload is plain text.</summary>
/// <param name="PayloadBytes">
/// On-disk payload size, which includes the NUL terminator and so exceeds the decoded string
/// length. Kept so byte accounting stays comparable across command types.
/// </param>
public sealed record ConsoleCommand(int Tick, long Offset, string Command, int PayloadBytes)
    : DemoCommand(DemoCommandType.ConsoleCmd, Tick, Offset);

/// <summary>dem_usercmd. The payload is a bit-packed delta-encoded CUserCmd, not yet decoded.</summary>
public sealed record UserCommand(int Tick, long Offset, int CommandNumber, byte[] Payload)
    : DemoCommand(DemoCommandType.UserCmd, Tick, Offset);

/// <summary>dem_datatables. Bitstream of send tables plus class info; not yet decoded.</summary>
public sealed record DataTablesCommand(int Tick, long Offset, byte[] Payload)
    : DemoCommand(DemoCommandType.DataTables, Tick, Offset);

/// <summary>
/// dem_stringtables. Not yet decoded.
/// </summary>
/// <remarks>
/// This block is written into a fixed engine buffer and is silently truncated when it overflows,
/// so its contents cannot be assumed complete. A payload of exactly 524288 bytes is the observed
/// truncation signature; see <see cref="DemoSummary.StringTablesLikelyTruncated"/>.
/// </remarks>
public sealed record StringTablesCommand(int Tick, long Offset, byte[] Payload)
    : DemoCommand(DemoCommandType.StringTables, Tick, Offset);

/// <summary>dem_stop. Terminates the stream.</summary>
public sealed record StopCommand(int Tick, long Offset)
    : DemoCommand(DemoCommandType.Stop, Tick, Offset);
