namespace GmodDemo.Parser;

/// <summary>
/// Parses the engine's <c>dem_stringtables</c> blob. That dump is a 512 KiB buffer and is
/// truncated when full, but <c>modelprecache</c> is the second table and fits. Each table is a
/// name, a 16-bit count, then C-strings with a 1-bit userdata flag (and a trailing 1-bit
/// client-side extra table).
/// </summary>
public static class DemoStringTables
{
    public static Dictionary<string, List<string?>>? Parse(byte[] dump)
    {
        var full = ParseFull(dump);
        if (full is null)
        {
            return null;
        }

        return full.ToDictionary(
            kv => kv.Key,
            kv => kv.Value.Select(e => e.Text).ToList(),
            StringComparer.Ordinal);
    }

    public static Dictionary<string, List<StringTableEntry>>? ParseFull(byte[] dump)
    {
        if (dump.Length < 16 || dump[1] != (byte)'d')
        {
            return null;
        }

        var reader = new BitReader(dump);
        var nTables = reader.ReadByte();
        if (nTables is < 2 or > 64)
        {
            return null;
        }

        var tables = new Dictionary<string, List<StringTableEntry>>(StringComparer.Ordinal);
        for (var t = 0; t < nTables && !reader.IsOverflowed && reader.BitsLeft > 24; t++)
        {
            var name = reader.ReadString();
            if (name.Length == 0)
            {
                return tables.Count > 0 ? tables : null;
            }

            var nStrings = (int)reader.ReadUBitLong(16);
            if (nStrings < 0 || nStrings > 200_000)
            {
                return tables.Count > 0 ? tables : null;
            }

            var entries = ReadEntries(ref reader, nStrings);
            if (reader.ReadOneBit() != 0)
            {
                var nClient = (int)reader.ReadUBitLong(16);
                if (nClient >= 0 && nClient < 200_000)
                {
                    entries.AddRange(ReadEntries(ref reader, nClient));
                }
            }

            tables[name] = entries;
        }

        return tables.Count > 0 ? tables : null;
    }

    private static List<StringTableEntry> ReadEntries(ref BitReader reader, int count)
    {
        var entries = new List<StringTableEntry>(count);
        for (var i = 0; i < count && !reader.IsOverflowed; i++)
        {
            var text = reader.ReadString();
            byte[]? userData = null;
            if (reader.ReadOneBit() != 0)
            {
                var length = (int)reader.ReadUBitLong(16);
                if (length < 0 || length > 1_000_000)
                {
                    break;
                }

                userData = reader.ReadBitsAsBytes(Math.Min(length * 8, reader.BitsLeft));
            }

            entries.Add(new StringTableEntry(i, text.Length == 0 ? null : text, userData is { Length: > 0 } ? userData : null));
        }

        return entries;
    }
}
