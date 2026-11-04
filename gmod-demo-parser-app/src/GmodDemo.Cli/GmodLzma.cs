using System.Buffers.Binary;
using SevenZip.Compression.LZMA;

namespace GmodDemo.Parser;

/// <summary>
/// GMod <c>client_lua_files</c> userdata: 20-byte SHA-1, then an LZMA stream
/// (standard 5-byte properties + 8-byte unpack size). Same blob layout the client
/// writes under its Lua cache; this parser only reads the bytes from the demo.
/// </summary>
public static class GmodLzma
{
    public const int Sha1Bytes = 20;

    public static byte[]? DecompressLuaCache(ReadOnlySpan<byte> userdata)
    {
        if (userdata.Length < Sha1Bytes + 14)
        {
            return null;
        }

        var payload = userdata[Sha1Bytes..];
        for (var i = 0; i + 13 < payload.Length; i++)
        {
            if (payload[i] != 0x5D)
            {
                continue;
            }

            var unpackSize = BinaryPrimitives.ReadInt64LittleEndian(payload[(i + 5)..]);
            if (unpackSize is <= 0 or > 2_000_000)
            {
                continue;
            }

            var decoded = Decode(payload[i..], unpackSize);
            if (decoded is { Length: > 0 })
            {
                return decoded;
            }
        }

        return null;
    }

    public static byte[]? Decode(ReadOnlySpan<byte> lzma, long unpackSize)
    {
        if (lzma.Length < 14 || unpackSize is <= 0 or > 32_000_000)
        {
            return null;
        }

        return DecodeCoded(lzma[..5], lzma[13..], unpackSize);
    }

    public static byte[]? DecompressLzmaFile(ReadOnlySpan<byte> data)
    {
        if (data.Length >= 14 && data[0] == 0x5D)
        {
            var unpackSize = BinaryPrimitives.ReadInt64LittleEndian(data[5..]);
            var decoded = Decode(data, unpackSize);
            if (decoded is not null)
            {
                return decoded;
            }
        }

        return UtilDecompress(data);
    }

    /// <summary>
    /// GMod <c>util.Decompress</c>: 4-byte unpack size, 5-byte LZMA properties, then the
    /// coded stream (no embedded unpack-size field). Also accepts an 8-byte size prefix.
    /// </summary>
    public static byte[]? UtilDecompress(ReadOnlySpan<byte> data)
    {
        if (data.Length < 9)
        {
            return null;
        }

        var size32 = BinaryPrimitives.ReadUInt32LittleEndian(data);
        if (size32 is > 0 and <= 32_000_000 && data.Length > 9)
        {
            var decoded = DecodeCoded(data.Slice(4, 5), data[9..], size32);
            if (decoded is { Length: > 0 })
            {
                return decoded;
            }
        }

        if (data.Length < 14)
        {
            return DecompressLuaCache(data);
        }

        var unpackSize = BinaryPrimitives.ReadInt64LittleEndian(data);
        if (unpackSize is <= 0 or > 32_000_000)
        {
            return DecompressLuaCache(data);
        }

        try
        {
            var decoded = DecodeCoded(data.Slice(8, 5), data[13..], unpackSize);
            if (decoded is { Length: > 0 })
            {
                return decoded;
            }

            return Decode(data[8..], unpackSize) ?? DecompressLuaCache(data);
        }
        catch (Exception)
        {
            return Decode(data[8..], unpackSize) ?? DecompressLuaCache(data);
        }
    }

    private static byte[]? DecodeCoded(ReadOnlySpan<byte> properties, ReadOnlySpan<byte> coded, long unpackSize)
    {
        if (properties.Length < 5 || coded.Length == 0 || unpackSize is <= 0 or > 32_000_000)
        {
            return null;
        }

        try
        {
            var decoder = new Decoder();
            decoder.SetDecoderProperties(properties.ToArray());
            using var input = new MemoryStream(coded.ToArray());
            using var output = new MemoryStream((int)unpackSize);
            decoder.Code(input, output, input.Length, unpackSize, progress: null!);
            return output.Length > 0 ? output.ToArray() : null;
        }
        catch (Exception)
        {
            return null;
        }
    }
}
