using System.Text;

namespace GmodDemo.Parser;

public abstract record PropValue
{
    public sealed record Int(long Value) : PropValue;

    public sealed record Float(float Value) : PropValue;

    public sealed record Vector(Vec3 Value) : PropValue;

    public sealed record String(string Value) : PropValue;

    public sealed record Array(IReadOnlyList<PropValue> Values) : PropValue;

    public sealed record GmodTable(bool Cleared, IReadOnlyDictionary<int, PropValue?> Values) : PropValue;
}

/// <summary>
/// Decodes one flattened send property from a packed entity bitstream.
/// Integer/float/vector encodings follow Source SDK 2013 <c>dt_encode.cpp</c>.
/// Strings are length-prefixed: 9 bits of byte count, then that many bytes
/// (CS:GO <c>DecodeString</c>). Confirmed on current GMod: NUL-terminated
/// <c>bf_read::ReadString</c> desynchronizes at the first <c>CSENT_anim</c>.
/// </summary>
public static class PropDecoder
{
    public const int MaxStringBits = 9;

    public static PropValue? Decode(
        ref BitReader reader,
        FlattenedProp prop,
        int zeroBitInt64Width = 64)
    {
        return DecodeValue(ref reader, prop.Prop, prop.ArrayElement, zeroBitInt64Width);
    }

    private static PropValue? DecodeValue(
        ref BitReader reader,
        SendPropDef prop,
        SendPropDef? arrayElement,
        int zeroBitInt64Width)
    {
        switch (prop.Type)
        {
            case SendPropType.Int:
                return new PropValue.Int(DecodeInt(ref reader, prop));
            case SendPropType.Float:
                return new PropValue.Float(DecodeFloat(ref reader, prop));
            case SendPropType.Vector:
                return new PropValue.Vector(DecodeVector(ref reader, prop));
            case SendPropType.VectorXY:
                return new PropValue.Vector(new Vec3(DecodeFloat(ref reader, prop), DecodeFloat(ref reader, prop), 0f));
            case SendPropType.String:
                return new PropValue.String(DecodeString(ref reader));
            case SendPropType.Array:
                return DecodeArray(ref reader, prop, arrayElement, zeroBitInt64Width);
            case SendPropType.Int64:
                return new PropValue.Int(DecodeInt64(ref reader, prop, zeroBitInt64Width));
            case SendPropType.GmodTable:
                return DecodeGmodTable(ref reader);
            default:
                return null;
        }
    }

    private static PropValue.GmodTable DecodeGmodTable(ref BitReader reader)
    {
        var count = (int)reader.ReadUBitLong(12);
        var cleared = reader.ReadBool();
        var values = new Dictionary<int, PropValue?>();
        for (var i = 0; i < count && !reader.IsOverflowed; i++)
        {
            var key = (int)reader.ReadUBitLong(12);
            var type = reader.ReadUBitLong(3);
            values[key] = type switch
            {
                0 => null,
                1 => new PropValue.Float(reader.ReadFloat()),
                2 => new PropValue.Int(reader.ReadLong()),
                3 => new PropValue.Int(reader.ReadOneBit()),
                4 or 5 => new PropValue.Vector(new Vec3(
                    reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat())),
                6 => new PropValue.Int(reader.ReadUBitLong(23)),
                7 => new PropValue.String(DecodeString(ref reader)),
                _ => throw new InvalidOperationException("Invalid three-bit GMod variant type."),
            };
        }
        return new PropValue.GmodTable(cleared, values);
    }

    private static PropValue? DecodeArray(
        ref BitReader reader,
        SendPropDef prop,
        SendPropDef? element,
        int zeroBitInt64Width)
    {
        if (element is null)
        {
            return null;
        }

        var bits = BitsForCount(prop.NumElements);
        var count = (int)reader.ReadUBitLong(bits);
        if (count < 0 || count > prop.NumElements || reader.IsOverflowed)
        {
            reader.SetOverflow();
            return null;
        }

        var values = new PropValue[count];
        for (var i = 0; i < count; i++)
        {
            var decoded = DecodeValue(ref reader, element, null, zeroBitInt64Width);
            if (decoded is null || reader.IsOverflowed)
            {
                reader.SetOverflow();
                return null;
            }

            values[i] = decoded;
        }

        return new PropValue.Array(values);
    }

    private static string DecodeString(ref BitReader reader)
    {
        var length = (int)reader.ReadUBitLong(MaxStringBits);
        if (length < 0 || length >= 1 << MaxStringBits || reader.IsOverflowed)
        {
            reader.SetOverflow();
            return string.Empty;
        }

        if (length == 0)
        {
            return string.Empty;
        }

        var bytes = reader.ReadBytes(length);
        var n = bytes.Length;
        while (n > 0 && bytes[n - 1] == 0)
        {
            n--;
        }

        return Encoding.UTF8.GetString(bytes, 0, n);
    }

    private static long DecodeInt(ref BitReader reader, SendPropDef prop)
    {
        // SPROP_VARINT reuses SPROP_NORMAL so integer props with that flag are protobuf varints.
        if ((prop.Flags & SendPropFlags.Normal) != 0)
        {
            return (prop.Flags & SendPropFlags.Unsigned) != 0
                ? reader.ReadVarInt32()
                : reader.ReadSignedVarInt32();
        }

        if (prop.Bits <= 0)
        {
            return 0;
        }

        if ((prop.Flags & SendPropFlags.Unsigned) != 0)
        {
            return reader.ReadUBitLong(prop.Bits);
        }

        return reader.ReadSBitLong(prop.Bits);
    }

    private static long DecodeInt64(
        ref BitReader reader,
        SendPropDef prop,
        int zeroBitInt64Width)
    {
        if ((prop.Flags & SendPropFlags.Normal) != 0)
        {
            return (prop.Flags & SendPropFlags.Unsigned) != 0
                ? (long)reader.ReadVarInt64()
                : reader.ReadSignedVarInt64();
        }

        var bits = prop.Bits == 0 ? zeroBitInt64Width : prop.Bits;
        if ((prop.Flags & SendPropFlags.Unsigned) != 0)
        {
            var low = reader.ReadUBitLong(32);
            var highBits = Math.Max(bits - 32, 0);
            var high = highBits > 0 ? reader.ReadUBitLong(highBits) : 0;
            return (long)(((ulong)high << 32) | low);
        }

        var negative = reader.ReadOneBit() != 0;
        var lowSigned = reader.ReadUBitLong(32);
        var highSignedBits = Math.Max(bits - 33, 0);
        var highSigned = highSignedBits > 0 ? reader.ReadUBitLong(highSignedBits) : 0;
        var value = (long)(((ulong)highSigned << 32) | lowSigned);
        return negative ? -value : value;
    }

    private static float DecodeFloat(ref BitReader reader, SendPropDef prop)
    {
        var flags = prop.Flags;
        if ((flags & SendPropFlags.Coord) != 0)
        {
            return reader.ReadBitCoord();
        }

        if ((flags & SendPropFlags.CoordMp) != 0)
        {
            return reader.ReadBitCoordMp(CoordMpMode.None);
        }

        if ((flags & SendPropFlags.CoordMpLowPrecision) != 0)
        {
            return reader.ReadBitCoordMp(CoordMpMode.LowPrecision);
        }

        if ((flags & SendPropFlags.CoordMpIntegral) != 0)
        {
            return reader.ReadBitCoordMp(CoordMpMode.Integral);
        }

        if ((flags & SendPropFlags.NoScale) != 0)
        {
            return reader.ReadBitFloat();
        }

        if ((flags & SendPropFlags.Normal) != 0)
        {
            return reader.ReadBitNormal();
        }

        if (prop.Bits <= 0)
        {
            return 0f;
        }

        var raw = reader.ReadUBitLong(prop.Bits);
        var denom = (1 << prop.Bits) - 1;
        if (denom <= 0)
        {
            return prop.LowValue;
        }

        var t = raw / (float)denom;
        return prop.LowValue + ((prop.HighValue - prop.LowValue) * t);
    }

    private static Vec3 DecodeVector(ref BitReader reader, SendPropDef prop)
    {
        if ((prop.Flags & SendPropFlags.Normal) != 0)
        {
            // Send props encode both normal components without the optional-component bits
            // used by bf_write::WriteBitVec3Normal.
            var x = reader.ReadBitNormal();
            var y = reader.ReadBitNormal();
            var z = MathF.Sqrt(MathF.Max(0, 1 - x * x - y * y));
            return new Vec3(x, y, reader.ReadOneBit() != 0 ? -z : z);
        }

        return new Vec3(DecodeFloat(ref reader, prop), DecodeFloat(ref reader, prop), DecodeFloat(ref reader, prop));
    }

    public static int BitsForCount(int maxElements)
    {
        var bits = 0;
        while (maxElements > 0)
        {
            bits++;
            maxElements >>= 1;
        }

        return bits;
    }
}
