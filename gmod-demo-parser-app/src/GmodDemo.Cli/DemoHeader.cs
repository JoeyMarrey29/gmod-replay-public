using System.Buffers.Binary;
using System.Text;

namespace GmodDemo.Parser;

/// <summary>Which magic the file was stamped with.</summary>
public enum DemoVariant
{
    /// <summary>Garry's Mod: "GMODEMO\0".</summary>
    GarrysMod,

    /// <summary>Stock Source engine: "HL2DEMO\0".</summary>
    Source,
}

/// <summary>
/// The fixed 1072-byte demo header. All scalars are little-endian.
/// </summary>
public sealed record DemoHeader(
    DemoVariant Variant,
    string Magic,
    int DemoProtocol,
    int NetworkProtocol,
    string ServerName,
    string ClientName,
    string MapName,
    string GameDirectory,
    float PlaybackTime,
    int PlaybackTicks,
    int PlaybackFrames,
    int SignonLength)
{
    /// <summary>
    /// Ticks per second, derived rather than stored. Returns null when playback time is zero,
    /// which happens if recording ended without the header being patched on close.
    /// </summary>
    public double? TickRate => PlaybackTime > 0 ? PlaybackTicks / PlaybackTime : null;

    public static DemoHeader Parse(ReadOnlySpan<byte> buffer)
    {
        if (buffer.Length < DemoFormat.HeaderSize)
        {
            throw new DemoFormatException(
                0,
                $"File is too short to contain a demo header: got {buffer.Length} bytes, need {DemoFormat.HeaderSize}.");
        }

        var magic = buffer[..DemoFormat.MagicSize];
        DemoVariant variant;
        if (magic.SequenceEqual(DemoFormat.GmodMagic))
        {
            variant = DemoVariant.GarrysMod;
        }
        else if (magic.SequenceEqual(DemoFormat.SourceMagic))
        {
            variant = DemoVariant.Source;
        }
        else
        {
            throw new DemoFormatException(
                0,
                $"Unrecognised demo magic {Convert.ToHexString(magic)} ('{Describe(magic)}'). " +
                "Expected 'GMODEMO\\0' (Garry's Mod) or 'HL2DEMO\\0' (Source).");
        }

        var demoProtocol = BinaryPrimitives.ReadInt32LittleEndian(buffer[8..]);
        var networkProtocol = BinaryPrimitives.ReadInt32LittleEndian(buffer[12..]);

        if (demoProtocol != DemoFormat.SupportedDemoProtocol)
        {
            throw new DemoFormatException(
                8,
                $"Unsupported demo protocol {demoProtocol}; this parser implements protocol " +
                $"{DemoFormat.SupportedDemoProtocol} only. Protocol 4 uses different command framing.");
        }

        return new DemoHeader(
            variant,
            Describe(magic),
            demoProtocol,
            networkProtocol,
            ReadFixedString(buffer, 16),
            ReadFixedString(buffer, 276),
            ReadFixedString(buffer, 536),
            ReadFixedString(buffer, 796),
            BinaryPrimitives.ReadSingleLittleEndian(buffer[1056..]),
            BinaryPrimitives.ReadInt32LittleEndian(buffer[1060..]),
            BinaryPrimitives.ReadInt32LittleEndian(buffer[1064..]),
            BinaryPrimitives.ReadInt32LittleEndian(buffer[1068..]));
    }

    private static string ReadFixedString(ReadOnlySpan<byte> buffer, int offset)
    {
        var field = buffer.Slice(offset, DemoFormat.MaxOsPath);
        var end = field.IndexOf((byte)0);
        if (end >= 0)
        {
            field = field[..end];
        }

        return Encoding.UTF8.GetString(field);
    }

    private static string Describe(ReadOnlySpan<byte> magic)
    {
        var sb = new StringBuilder(magic.Length);
        foreach (var b in magic)
        {
            if (b == 0)
            {
                break;
            }

            sb.Append(b is >= 32 and < 127 ? (char)b : '?');
        }

        return sb.ToString();
    }
}
