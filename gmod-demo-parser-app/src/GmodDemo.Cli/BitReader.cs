using System.Runtime.CompilerServices;
using System.Text;

namespace GmodDemo.Parser;

/// <summary>Which multiplayer coordinate packing <see cref="BitReader.ReadBitCoordMp"/> should use.</summary>
public enum CoordMpMode
{
    None = 0,
    LowPrecision = 1,
    Integral = 2,
}

/// <summary>
/// Bit-level reader matching Valve's <c>bf_read</c> semantics: bits are consumed
/// least-significant-bit first within each byte, and multi-bit fields are filled LSB first.
/// </summary>
/// <remarks>
/// Overflow is sticky and non-throwing, mirroring the engine: once the reader runs past the end
/// it sets <see cref="IsOverflowed"/> and yields zeros. Net messages are packed back to back with
/// no delimiters, so a desync produces nonsense rather than an exception; callers must check
/// <see cref="IsOverflowed"/> and the remaining bit count rather than relying on errors.
/// </remarks>
public ref struct BitReader
{
    private readonly ReadOnlySpan<byte> _data;
    private readonly int _totalBits;
    private int _bitsRead;

    public BitReader(ReadOnlySpan<byte> data)
    {
        _data = data;
        _totalBits = data.Length * 8;
        _bitsRead = 0;
    }

    public readonly int TotalBits => _totalBits;

    public readonly int BitsRead => _bitsRead;

    public readonly int BitsLeft => _totalBits - _bitsRead;

    public bool IsOverflowed { get; private set; }

    public void ClearOverflow() => IsOverflowed = false;

    /// <summary>Marks the reader overflowed. Used when a declared length exceeds what remains.</summary>
    public void SetOverflow() => IsOverflowed = true;

    public bool Seek(int bitPosition)
    {
        if (bitPosition < 0 || bitPosition > _totalBits)
        {
            IsOverflowed = true;
            return false;
        }

        _bitsRead = bitPosition;
        return true;
    }

    public bool SeekRelative(int bitCount) => Seek(_bitsRead + bitCount);

    public int ReadOneBit()
    {
        if (_bitsRead + 1 > _totalBits)
        {
            IsOverflowed = true;
            return 0;
        }

        var value = (_data[_bitsRead >> 3] >> (_bitsRead & 7)) & 1;
        _bitsRead++;
        return value;
    }

    public bool ReadBool() => ReadOneBit() != 0;

    /// <summary>Reads a field without consuming it.</summary>
    public uint PeekUBitLong(int numBits)
    {
        var savedPosition = _bitsRead;
        var savedOverflow = IsOverflowed;

        var value = ReadUBitLong(numBits);

        _bitsRead = savedPosition;
        IsOverflowed = savedOverflow;
        return value;
    }

    public uint ReadUBitLong(int numBits)
    {
        if (numBits == 0)
        {
            return 0;
        }

        if ((uint)(numBits - 1) >= 32)
        {
            throw new ArgumentOutOfRangeException(nameof(numBits), numBits, "Must be between 1 and 32.");
        }

        if (_bitsRead + numBits > _totalBits)
        {
            IsOverflowed = true;
            _bitsRead = _totalBits;
            return 0;
        }

        var byteIndex = _bitsRead >> 3;
        var bitOffset = _bitsRead & 7;

        // At most 5 bytes can span a 32-bit field starting at an arbitrary bit offset.
        ulong window = 0;
        var bytesNeeded = (bitOffset + numBits + 7) >> 3;
        for (var i = 0; i < bytesNeeded; i++)
        {
            window |= (ulong)_data[byteIndex + i] << (i * 8);
        }

        window >>= bitOffset;
        _bitsRead += numBits;

        var mask = numBits == 32 ? uint.MaxValue : (1u << numBits) - 1;
        return (uint)(window & mask);
    }

    /// <summary>Reads a two's-complement signed field of the given width.</summary>
    public int ReadSBitLong(int numBits)
    {
        var value = ReadUBitLong(numBits);
        if (numBits == 32)
        {
            return (int)value;
        }

        // Sign-extend from the field's top bit.
        var signBit = 1u << (numBits - 1);
        return (value & signBit) != 0 ? (int)(value | ~(signBit - 1)) : (int)value;
    }

    public byte ReadByte() => (byte)ReadUBitLong(8);

    public sbyte ReadChar() => (sbyte)ReadUBitLong(8);

    public ushort ReadWord() => (ushort)ReadUBitLong(16);

    public short ReadShort() => (short)ReadSBitLong(16);

    public int ReadLong() => (int)ReadUBitLong(32);

    public float ReadFloat() => BitConverter.UInt32BitsToSingle(ReadUBitLong(32));

    /// <summary>Raw IEEE-754 bits, matching the engine's ReadBitFloat.</summary>
    public float ReadBitFloat() => ReadFloat();

    public long ReadLongLong()
    {
        // The engine orders the halves by host endianness; on little-endian the low dword is first.
        ulong low = ReadUBitLong(32);
        ulong high = ReadUBitLong(32);
        return (long)((high << 32) | low);
    }

    /// <summary>
    /// Protobuf-style LEB128, read a byte at a time from the current bit position, so it is not
    /// byte-aligned. Truncates at five bytes rather than erroring, as the engine does.
    /// </summary>
    public uint ReadVarInt32()
    {
        uint result = 0;
        var count = 0;
        uint b;

        do
        {
            if (count == 5)
            {
                return result;
            }

            b = ReadUBitLong(8);
            result |= (b & 0x7F) << (7 * count);
            count++;
        }
        while ((b & 0x80) != 0);

        return result;
    }

    public int ReadSignedVarInt32()
    {
        var value = ReadVarInt32();
        return (int)(value >> 1) ^ -(int)(value & 1);
    }

    /// <summary>
    /// Two-bit selector followed by 4, 8, 12 or 32 value bits. This is the Source 1 encoding;
    /// it is not the same as the Source 2 / CS:GO field-path variant.
    /// </summary>
    public uint ReadUBitVar()
    {
        var selector = ReadUBitLong(2);
        return selector switch
        {
            0 => ReadUBitLong(4),
            1 => ReadUBitLong(8),
            2 => ReadUBitLong(12),
            _ => ReadUBitLong(32),
        };
    }

    /// <summary>
    /// Reads a NUL-terminated string. Always consumes through the terminator even if the result
    /// is truncated at <paramref name="maxLength"/>, so truncation cannot desynchronise the stream.
    /// </summary>
    public string ReadString(int maxLength = 4096)
    {
        var builder = new StringBuilder();
        while (true)
        {
            if (BitsLeft < 8)
            {
                IsOverflowed = true;
                break;
            }

            var value = ReadByte();
            if (value == 0)
            {
                break;
            }

            if (builder.Length < maxLength)
            {
                builder.Append((char)value);
            }
        }

        return builder.ToString();
    }

    public byte[] ReadBytes(int count)
    {
        var buffer = new byte[count];
        for (var i = 0; i < count; i++)
        {
            buffer[i] = ReadByte();
        }

        return buffer;
    }

    /// <summary>Copies <paramref name="bitCount"/> bits out, LSB first, padding the final byte.</summary>
    public byte[] ReadBitsAsBytes(int bitCount)
    {
        if (bitCount < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(bitCount));
        }

        var buffer = new byte[(bitCount + 7) / 8];
        var whole = bitCount / 8;
        for (var i = 0; i < whole; i++)
        {
            buffer[i] = ReadByte();
        }

        var remainder = bitCount & 7;
        if (remainder > 0)
        {
            buffer[whole] = (byte)ReadUBitLong(remainder);
        }

        return buffer;
    }

    // Coordinate encoding constants from Valve's coordsize.h.
    private const int CoordIntegerBits = 14;
    private const int CoordFractionalBits = 5;
    private const float CoordResolution = 1.0f / (1 << CoordFractionalBits);
    private const int NormalFractionalBits = 11;
    private const float NormalResolution = 1.0f / ((1 << NormalFractionalBits) - 1);

    /// <summary>
    /// Variable-width world coordinate: presence flags for integer and fractional parts, an
    /// optional sign, then the parts that are present. Costs 2, 7, 17 or 21 bits.
    /// </summary>
    public float ReadBitCoord()
    {
        var hasInteger = ReadOneBit();
        var hasFraction = ReadOneBit();

        if (hasInteger == 0 && hasFraction == 0)
        {
            return 0f;
        }

        var negative = ReadOneBit() != 0;

        var integer = 0u;
        if (hasInteger != 0)
        {
            // Encoded as value-1 so the range [1,N] fits in N-1 bits worth of codes.
            integer = ReadUBitLong(CoordIntegerBits) + 1;
        }

        var fraction = 0u;
        if (hasFraction != 0)
        {
            fraction = ReadUBitLong(CoordFractionalBits);
        }

        var value = integer + (fraction * CoordResolution);
        return negative ? -value : value;
    }

    /// <summary>Three presence flags, then the present components in order. Absent axes are zero.</summary>
    public Vec3 ReadBitVec3Coord()
    {
        var hasX = ReadOneBit() != 0;
        var hasY = ReadOneBit() != 0;
        var hasZ = ReadOneBit() != 0;

        return new Vec3(
            hasX ? ReadBitCoord() : 0f,
            hasY ? ReadBitCoord() : 0f,
            hasZ ? ReadBitCoord() : 0f);
    }

    public float ReadBitNormal()
    {
        var negative = ReadOneBit() != 0;
        var fraction = ReadUBitLong(NormalFractionalBits);
        var value = fraction * NormalResolution;
        return negative ? -value : value;
    }

    /// <summary>
    /// Two presence-flagged components then a sign bit for z, which is reconstructed from the
    /// unit-length constraint rather than transmitted.
    /// </summary>
    public Vec3 ReadBitVec3Normal()
    {
        var hasX = ReadOneBit() != 0;
        var hasY = ReadOneBit() != 0;

        var x = hasX ? ReadBitNormal() : 0f;
        var y = hasY ? ReadBitNormal() : 0f;

        var negativeZ = ReadOneBit() != 0;
        var sumSquares = (x * x) + (y * y);
        var z = sumSquares < 1.0f ? MathF.Sqrt(1.0f - sumSquares) : 0f;

        return new Vec3(x, y, negativeZ ? -z : z);
    }

    public float ReadBitAngle(int numBits)
    {
        var shift = 1 << numBits;
        return ReadUBitLong(numBits) * (360.0f / shift);
    }

    // Multiplayer coordinate extras from coordsize.h. Integer part is 11 bits when in-bounds.
    private const int CoordIntegerBitsMp = 11;
    private const int CoordFractionalBitsMpLowPrecision = 3;
    private const float CoordResolutionLowPrecision = 1.0f / (1 << CoordFractionalBitsMpLowPrecision);

    /// <summary>
    /// Multiplayer world coordinate. Presence of the integer part, a sign, then either 11 or 14
    /// integer bits depending on the in-bounds flag. The fractional width is 5, or 3 when low
    /// precision is requested. Integral mode omits the fraction entirely.
    /// </summary>
    public float ReadBitCoordMp(CoordMpMode mode)
    {
        var integral = mode == CoordMpMode.Integral;
        var lowPrecision = mode == CoordMpMode.LowPrecision;
        var inBounds = ReadOneBit() != 0;

        var integer = 0u;
        var fraction = 0u;
        var negative = false;

        if (integral)
        {
            if (ReadOneBit() != 0)
            {
                negative = ReadOneBit() != 0;
                integer = ReadUBitLong(inBounds ? CoordIntegerBitsMp : CoordIntegerBits) + 1;
            }
        }
        else
        {
            var hasInteger = ReadOneBit() != 0;
            negative = ReadOneBit() != 0;
            if (hasInteger)
            {
                integer = ReadUBitLong(inBounds ? CoordIntegerBitsMp : CoordIntegerBits) + 1;
            }

            fraction = ReadUBitLong(lowPrecision ? CoordFractionalBitsMpLowPrecision : CoordFractionalBits);
        }

        var resolution = lowPrecision ? CoordResolutionLowPrecision : CoordResolution;
        var value = integer + (fraction * resolution);
        return negative ? -value : value;
    }

    /// <summary>Protobuf-style LEB128 up to ten bytes, matching <c>bf_read::ReadVarInt64</c>.</summary>
    public ulong ReadVarInt64()
    {
        ulong result = 0;
        for (var count = 0; count < 10; count++)
        {
            var b = ReadUBitLong(8);
            result |= (b & 0x7F) << (7 * count);
            if ((b & 0x80) == 0)
            {
                break;
            }
        }

        return result;
    }

    public long ReadSignedVarInt64()
    {
        var value = ReadVarInt64();
        return (long)(value >> 1) ^ -(long)(value & 1);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public readonly bool HasBits(int count) => BitsLeft >= count;
}
