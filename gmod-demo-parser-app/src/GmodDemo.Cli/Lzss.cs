using System.Buffers.Binary;

namespace GmodDemo.Parser;

/// <summary>
/// Valve's CLZSS codec from Source SDK 2013 <c>tier1/lzss.cpp</c> (<c>SafeUncompress</c>).
/// </summary>
/// <remarks>
/// The on-disk header is the 4-byte magic <c>LZSS</c> followed by a little-endian uncompressed
/// size. String-table payloads wrap that with an extra pair of sizes; <see cref="Decompress"/>
/// accepts either form.
/// </remarks>
public static class Lzss
{
    public static ReadOnlySpan<byte> Magic => "LZSS"u8;

    public const int HeaderSize = 8;

    public static bool HasMagic(ReadOnlySpan<byte> data) =>
        data.Length >= 4 && data[0] == (byte)'L' && data[1] == (byte)'Z' && data[2] == (byte)'S' && data[3] == (byte)'S';

    /// <summary>
    /// Decompresses a Valve LZSS blob. <paramref name="data"/> may start with the <c>LZSS</c>
    /// magic or with the 4-byte actual-size field (the form used after a string-table magic check).
    /// Returns null if the input is truncated, malformed, or would exceed
    /// <paramref name="maxOutputBytes"/>.
    /// </summary>
    public static byte[]? Decompress(ReadOnlySpan<byte> data, int maxOutputBytes = 16 * 1024 * 1024)
    {
        if (HasMagic(data))
        {
            data = data[4..];
        }

        if (data.Length < 4)
        {
            return null;
        }

        var actualSize = BinaryPrimitives.ReadUInt32LittleEndian(data);
        if (actualSize == 0 || actualSize > (uint)maxOutputBytes)
        {
            return null;
        }

        return DecompressBody(data[4..], (int)actualSize);
    }

    private static byte[]? DecompressBody(ReadOnlySpan<byte> input, int actualSize)
    {
        var output = new byte[actualSize];
        var read = 0;
        var written = 0;
        var cmdByte = 0;
        var getCmdByte = 0;

        while (true)
        {
            if (getCmdByte == 0)
            {
                if (read >= input.Length)
                {
                    return null;
                }

                cmdByte = input[read++];
            }

            getCmdByte = (getCmdByte + 1) & 7;

            if ((cmdByte & 1) != 0)
            {
                if (read + 2 > input.Length)
                {
                    return null;
                }

                var position = input[read++] << 4;
                position |= input[read] >> 4;
                var count = (input[read++] & 0x0F) + 1;

                // A match of length 1 is the terminator, not a copy.
                if (count == 1)
                {
                    break;
                }

                if (position < 0 || position >= written || written + count > actualSize)
                {
                    return null;
                }

                var source = written - position - 1;
                for (var i = 0; i < count; i++)
                {
                    output[written++] = output[source + i];
                }
            }
            else
            {
                if (read >= input.Length || written + 1 > actualSize)
                {
                    return null;
                }

                output[written++] = input[read++];
            }

            cmdByte >>= 1;
        }

        return written == actualSize ? output : null;
    }
}
