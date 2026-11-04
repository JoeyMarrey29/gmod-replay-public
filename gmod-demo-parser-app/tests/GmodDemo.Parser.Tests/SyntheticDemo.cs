using System.Buffers.Binary;
using System.Text;
using GmodDemo.Parser;

namespace GmodDemo.Parser.Tests;

/// <summary>
/// Builds deliberately malformed byte sequences for error-handling tests.
/// </summary>
/// <remarks>
/// These are SYNTHETIC fixtures. They are not captured from Garry's Mod and must never be
/// treated as evidence about what real demos contain. Their only purpose is to drive the parser
/// down its failure paths, which real well-formed demos never exercise. Claims about the actual
/// format belong in <see cref="RealDemoTests"/>, which run against a captured file.
/// </remarks>
internal static class SyntheticDemo
{
    public static byte[] Header(
        ReadOnlySpan<byte> magic = default,
        int demoProtocol = 3,
        int networkProtocol = 24,
        string serverName = "test-server",
        string clientName = "test-client",
        string mapName = "test-map",
        string gameDirectory = "garrysmod",
        float playbackTime = 0f,
        int playbackTicks = 0,
        int playbackFrames = 0,
        int signonLength = 0)
    {
        var buffer = new byte[DemoFormat.HeaderSize];

        if (magic.IsEmpty)
        {
            magic = DemoFormat.GmodMagic;
        }

        magic.CopyTo(buffer);
        BinaryPrimitives.WriteInt32LittleEndian(buffer.AsSpan(8), demoProtocol);
        BinaryPrimitives.WriteInt32LittleEndian(buffer.AsSpan(12), networkProtocol);
        WriteFixedString(buffer, 16, serverName);
        WriteFixedString(buffer, 276, clientName);
        WriteFixedString(buffer, 536, mapName);
        WriteFixedString(buffer, 796, gameDirectory);
        BinaryPrimitives.WriteSingleLittleEndian(buffer.AsSpan(1056), playbackTime);
        BinaryPrimitives.WriteInt32LittleEndian(buffer.AsSpan(1060), playbackTicks);
        BinaryPrimitives.WriteInt32LittleEndian(buffer.AsSpan(1064), playbackFrames);
        BinaryPrimitives.WriteInt32LittleEndian(buffer.AsSpan(1068), signonLength);

        return buffer;
    }

    public static MemoryStream Stream(byte[] header, params byte[][] records)
    {
        var stream = new MemoryStream();
        stream.Write(header);
        foreach (var record in records)
        {
            stream.Write(record);
        }

        stream.Position = 0;
        return stream;
    }

    public static byte[] CommandHeader(DemoCommandType type, int tick)
    {
        var buffer = new byte[5];
        buffer[0] = (byte)type;
        BinaryPrimitives.WriteInt32LittleEndian(buffer.AsSpan(1), tick);
        return buffer;
    }

    /// <summary>A dem_consolecmd record whose declared length can be set independently of its data.</summary>
    public static byte[] ConsoleCmd(int tick, string command, int? declaredLength = null)
    {
        var text = Encoding.UTF8.GetBytes(command);
        var payload = new byte[text.Length + 1];
        text.CopyTo(payload, 0);

        var writer = new MemoryStream();
        writer.Write(CommandHeader(DemoCommandType.ConsoleCmd, tick));
        var length = new byte[4];
        BinaryPrimitives.WriteInt32LittleEndian(length, declaredLength ?? payload.Length);
        writer.Write(length);
        writer.Write(payload);
        return writer.ToArray();
    }

    /// <summary>A dem_datatables record with a declared length that need not match the data supplied.</summary>
    public static byte[] DataTables(int tick, int declaredLength, int actualBytes)
    {
        var writer = new MemoryStream();
        writer.Write(CommandHeader(DemoCommandType.DataTables, tick));
        var length = new byte[4];
        BinaryPrimitives.WriteInt32LittleEndian(length, declaredLength);
        writer.Write(length);
        writer.Write(new byte[actualBytes]);
        return writer.ToArray();
    }

    public static byte[] Stop(int tick) => CommandHeader(DemoCommandType.Stop, tick);

    public static byte[] SyncTick(int tick) => CommandHeader(DemoCommandType.SyncTick, tick);

    private static void WriteFixedString(byte[] buffer, int offset, string value)
    {
        var bytes = Encoding.UTF8.GetBytes(value);
        Array.Copy(bytes, 0, buffer, offset, Math.Min(bytes.Length, DemoFormat.MaxOsPath - 1));
    }
}
