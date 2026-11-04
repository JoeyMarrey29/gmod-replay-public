using System.Buffers.Binary;

namespace GmodDemo.Parser;

public sealed record StringTableEntry(int Index, string? Text, byte[]? UserData);

public sealed class StringTable
{
    public required string Name { get; init; }
    public required int MaxEntries { get; init; }
    public required int DeclaredEntries { get; init; }
    public required bool LengthIsVarInt { get; init; }
    public required bool UserDataFixedSize { get; init; }
    public required int UserDataSize { get; init; }
    public required int UserDataSizeBits { get; init; }
    public required bool Compressed { get; init; }

    public Dictionary<int, StringTableEntry> Entries { get; } = [];

    public void Apply(IEnumerable<StringTableEntry> updates)
    {
        foreach (var entry in updates)
        {
            Entries[entry.Index] = entry;
        }
    }
}

/// <summary>
/// Parses <c>svc_CreateStringTable</c> / <c>svc_UpdateStringTable</c> payloads.
/// </summary>
/// <remarks>
/// Entry encoding is from Source's <c>CNetworkStringTable::ParseUpdate</c>, matching
/// demostf/parser's protocol-24 implementation. Recordings use either a 16-bit capacity
/// or a five-bit capacity exponent, and fixed-width or varint payload lengths.
/// Candidate layouts must decode the complete entry payload, not just plausible headers.
/// </remarks>
public static class StringTableParser
{
    public const int HistorySize = 32;
    public const int UserDataLengthBits = 14;

    public static StringTable? ReadCreate(ref BitReader r, int? forcedLengthBits = null)
    {
        var filenames = r.PeekUBitLong(8) == ':';
        if (filenames)
        {
            r.ReadByte();
        }

        var name = r.ReadString();
        if (r.IsOverflowed || name.Length == 0)
        {
            return null;
        }

        var afterName = r.BitsRead;

        StringTable? best = null;
        var bestEnd = afterName;
        var bestScore = -1;

        foreach (var capacityBits in new[] { 16, 5 })
        {
            r.Seek(afterName);
            r.ClearOverflow();
            var capacity = (int)r.ReadUBitLong(capacityBits);
            if (capacityBits == 5 && capacity > 16) continue;
            var maxEntries = capacityBits == 5 ? 1 << capacity : capacity;
            if (maxEntries <= 0 || (maxEntries & (maxEntries - 1)) != 0) continue;
            var numEntries = (int)r.ReadUBitLong(BitMath.Log2(maxEntries) + 1);
            if (r.IsOverflowed || numEntries > maxEntries) continue;
            var afterCounts = r.BitsRead;

            foreach (var lengthWidth in forcedLengthBits is { } forced ? new[] { forced } : new[] { 0, 20, 32 })
            {
                r.Seek(afterCounts);
                r.ClearOverflow();
                var lengthBits = lengthWidth == 0
                    ? (int)r.ReadVarInt32()
                    : (int)r.ReadUBitLong(lengthWidth);

                var userDataFixedSize = r.ReadOneBit() != 0;
                var userDataSize = 0;
                var userDataSizeBits = 0;
                if (userDataFixedSize)
                {
                    userDataSize = (int)r.ReadUBitLong(12);
                    userDataSizeBits = (int)r.ReadUBitLong(4);
                }

                var compressed = r.ReadOneBit() != 0;
                if (r.IsOverflowed || lengthBits <= 0 || lengthBits > r.BitsLeft) continue;

                var payload = r.ReadBitsAsBytes(lengthBits);
                var payloadEnd = r.BitsRead;
                if (r.IsOverflowed) continue;

                var table = new StringTable
                {
                    Name = name, MaxEntries = maxEntries, DeclaredEntries = numEntries,
                    LengthIsVarInt = lengthWidth == 0, UserDataFixedSize = userDataFixedSize,
                    UserDataSize = userDataSize, UserDataSizeBits = userDataSizeBits, Compressed = compressed,
                };

                var score = ScoreEntries(table, payload, numEntries);
                if (score > bestScore)
                {
                    best = table;
                    bestEnd = payloadEnd;
                    bestScore = score;
                }
            }
        }

        if (best is null)
        {
            return null;
        }

        r.Seek(bestEnd);
        r.ClearOverflow();
        _ = filenames;
        return best;
    }

    private static int ScoreEntries(StringTable table, byte[] payload, int numEntries)
    {
        foreach (var treatCompressed in table.Compressed ? new[] { true, false } : new[] { false })
        {
            var data = treatCompressed ? DecompressTablePayload(payload) : payload;
            if (data is null)
            {
                continue;
            }

            var dataReader = new BitReader(data);
            var entries = ReadEntries(ref dataReader, table, numEntries);
            if (entries is null || dataReader.IsOverflowed || dataReader.BitsLeft > 7)
            {
                continue;
            }

            table.Entries.Clear();
            table.Apply(entries);
            return entries.Count + entries.Count(entry => !string.IsNullOrEmpty(entry.Text));
        }

        return -1;
    }

    /// <summary>
    /// Reads an update. The caller must already know the table (id is not in this payload).
    /// Returns null for an incomplete or invalid entry payload.
    /// </summary>
    public static List<StringTableEntry>? ReadUpdate(ref BitReader r, StringTable table)
    {
        r.ReadUBitLong(5);
        var changed = r.ReadOneBit() != 0 ? (int)r.ReadUBitLong(16) : 1;
        var lengthBits = (int)r.ReadUBitLong(20);
        var payload = r.ReadBitsAsBytes(lengthBits);
        if (r.IsOverflowed)
        {
            return null;
        }

        var dataReader = new BitReader(payload);
        var entries = ReadEntries(ref dataReader, table, changed);
        return entries is not null && !dataReader.IsOverflowed && dataReader.BitsLeft <= 7 ? entries : null;
    }

    /// <summary>
    /// After the type tag has been consumed, skip a CreateStringTable without interpreting entries.
    /// Shared with <see cref="NetMessageWalker"/> so skip and parse cannot drift.
    /// </summary>
    public static bool SkipCreate(ref BitReader r, int? lengthBits = null)
    {
        return ReadCreate(ref r, lengthBits) is not null && !r.IsOverflowed;
    }

    public static bool SkipUpdate(ref BitReader r)
    {
        r.ReadUBitLong(5);
        if (r.ReadOneBit() != 0)
        {
            r.ReadUBitLong(16);
        }

        var lengthBits = (int)r.ReadUBitLong(20);
        return r.SeekRelative(lengthBits);
    }

    public static byte[]? DecompressTablePayload(byte[] payload)
    {
        if (payload.Length < 8)
        {
            return null;
        }

        var uncompressedSize = BinaryPrimitives.ReadUInt32LittleEndian(payload);
        var compressedSize = BinaryPrimitives.ReadInt32LittleEndian(payload.AsSpan(4));
        if (compressedSize < 4 || 8 + compressedSize > payload.Length)
        {
            // Some builds omit the outer size pair and store a raw LZSS blob.
            return Lzss.Decompress(payload);
        }

        var blob = payload.AsSpan(8, compressedSize);
        var decompressed = Lzss.Decompress(blob);
        if (decompressed is not null && uncompressedSize != 0 && decompressed.Length != uncompressedSize)
        {
            return null;
        }

        return decompressed;
    }

    public static List<StringTableEntry>? ReadEntries(ref BitReader r, StringTable table, int count)
    {
        var entries = new List<StringTableEntry>(Math.Max(0, count));
        var history = new List<string>(HistorySize);
        var lastIndex = -1;
        var indexBits = BitMath.Log2(table.MaxEntries);

        for (var i = 0; i < count; i++)
        {
            var index = r.ReadOneBit() != 0
                ? lastIndex + 1
                : (int)r.ReadUBitLong(indexBits);

            lastIndex = index;
            if (r.IsOverflowed || index < 0 || index >= table.MaxEntries)
            {
                return null;
            }

            string? text = null;
            if (r.ReadOneBit() != 0)
            {
                if (r.ReadOneBit() != 0)
                {
                    var historyIndex = (int)r.ReadUBitLong(5);
                    var bytesToCopy = (int)r.ReadUBitLong(5);
                    var rest = r.ReadString();
                    if (historyIndex >= 0 && historyIndex < history.Count)
                    {
                        var prefix = history[historyIndex];
                        if (bytesToCopy > prefix.Length)
                        {
                            bytesToCopy = prefix.Length;
                        }

                        text = string.Concat(prefix.AsSpan(0, bytesToCopy), rest);
                    }
                    else
                    {
                        text = rest;
                    }
                }
                else
                {
                    text = r.ReadString();
                }
            }

            byte[]? userData = null;
            if (r.ReadOneBit() != 0)
            {
                if (table.UserDataFixedSize)
                {
                    userData = r.ReadBitsAsBytes(table.UserDataSizeBits);
                }
                else
                {
                    var bytes = (int)r.ReadUBitLong(UserDataLengthBits);
                    userData = r.ReadBytes(bytes);
                }
            }

            if (r.IsOverflowed)
            {
                return null;
            }

            entries.Add(new StringTableEntry(index, text, userData));

            if (history.Count == HistorySize)
            {
                history.RemoveAt(0);
            }

            history.Add(text ?? string.Empty);
        }

        return entries;
    }
}
