namespace GmodDemo.Parser;

public enum GameEventKeyType : byte
{
    Local = 0,
    String = 1,
    Float = 2,
    Long = 3,
    Short = 4,
    Byte = 5,
    Bool = 6,
}

public sealed record GameEventKeyDef(string Name, GameEventKeyType Type);

public sealed record GameEventDescriptor(int Id, string Name, IReadOnlyList<GameEventKeyDef> Keys);

/// <summary>
/// Source 2013 <c>svc_GameEventList</c> / <c>svc_GameEvent</c>. The list is sent in signon after
/// string tables; CreateStringTable framing still dies on that packet, so the list is recovered
/// by locating <c>server_spawn</c> (bit-aligned) and reading the message header just before it.
/// Current GMod writes key types as 3-bit values, not Source's 8-bit <c>WriteByte</c>.
/// </summary>
public sealed class GameEventList
{
    public const int KeyTypeBits = 3;

    public IReadOnlyList<GameEventDescriptor> Events { get; }

    public IReadOnlyDictionary<int, GameEventDescriptor> ById { get; }

    public GameEventList(IReadOnlyList<GameEventDescriptor> events)
    {
        Events = events;
        ById = events.ToDictionary(e => e.Id);
    }

    public static GameEventList? TryFind(byte[] payload)
    {
        if (payload.Length < 64)
        {
            return null;
        }

        var spawnBit = FindBitAligned(payload, "server_spawn"u8.ToArray());
        if (spawnBit < NetMessageLayout.MaxEventBits)
        {
            return null;
        }

        var dataStart = spawnBit - NetMessageLayout.MaxEventBits;
        var headerBits = NetMessageLayout.TypeBits + NetMessageLayout.MaxEventBits + 20;
        if (dataStart < headerBits)
        {
            return null;
        }

        var reader = new BitReader(payload);
        reader.Seek(dataStart - headerBits);
        if (reader.ReadUBitLong(NetMessageLayout.TypeBits) != (uint)NetMessageType.SvcGameEventList)
        {
            return null;
        }

        var num = (int)reader.ReadUBitLong(NetMessageLayout.MaxEventBits);
        var lengthBits = (int)reader.ReadUBitLong(20);
        if (num is < 8 or > 512 || lengthBits < 128 || lengthBits > reader.BitsLeft || reader.BitsRead != dataStart)
        {
            return null;
        }

        var parsed = TryParseData(ref reader, num, lengthBits);
        if (parsed is null
            || parsed.Count < 8
            || parsed[0].Name != "server_spawn"
            || !parsed.Any(e => e.Name is "player_say" or "player_hurt" or "player_spawn"))
        {
            return null;
        }

        return new GameEventList(parsed);
    }

    private static int FindBitAligned(byte[] payload, byte[] needle)
    {
        var totalBits = payload.Length * 8;
        if (totalBits < needle.Length * 8)
        {
            return -1;
        }

        for (var shift = 0; shift < 8; shift++)
        {
            var available = (totalBits - shift) / 8;
            if (available < needle.Length)
            {
                continue;
            }

            var reader = new BitReader(payload);
            reader.Seek(shift);
            var shifted = reader.ReadBytes(available);
            var at = shifted.AsSpan().IndexOf(needle);
            if (at >= 0)
            {
                return shift + (at * 8);
            }
        }

        return -1;
    }

    private static List<GameEventDescriptor>? TryParseData(ref BitReader reader, int declaredCount, int lengthBits)
    {
        var end = reader.BitsRead + lengthBits;
        var events = new List<GameEventDescriptor>(declaredCount);
        for (var i = 0; i < declaredCount && reader.BitsRead < end && !reader.IsOverflowed; i++)
        {
            var id = (int)reader.ReadUBitLong(NetMessageLayout.MaxEventBits);
            var name = reader.ReadString(64);
            if (name.Length is < 3 or > 64
                || !name.All(c => char.IsAsciiLetterOrDigit(c) || c == '_')
                || !char.IsAsciiLetter(name[0]))
            {
                return null;
            }

            var keys = new List<GameEventKeyDef>();
            while (reader.BitsRead < end && !reader.IsOverflowed)
            {
                var type = (GameEventKeyType)reader.ReadUBitLong(KeyTypeBits);
                if (type == GameEventKeyType.Local)
                {
                    break;
                }

                if ((byte)type > 6)
                {
                    return null;
                }

                var key = reader.ReadString(64);
                if (key.Length is < 1 or > 64
                    || !key.All(c => char.IsAsciiLetterOrDigit(c) || c == '_'))
                {
                    return null;
                }

                keys.Add(new GameEventKeyDef(key, type));
                if (keys.Count > 32)
                {
                    return null;
                }
            }

            events.Add(new GameEventDescriptor(id, name, keys));
        }

        return reader.IsOverflowed ? null : events;
    }

    public GameEventValues? ReadEvent(ref BitReader reader, int lengthBits)
    {
        var end = reader.BitsRead + lengthBits;
        var id = (int)reader.ReadUBitLong(NetMessageLayout.MaxEventBits);
        if (!ById.TryGetValue(id, out var descriptor))
        {
            return new GameEventValues(id, null, new Dictionary<string, object?>());
        }

        var values = new Dictionary<string, object?>(StringComparer.Ordinal);
        foreach (var key in descriptor.Keys)
        {
            if (reader.BitsRead >= end || reader.IsOverflowed)
            {
                break;
            }

            values[key.Name] = key.Type switch
            {
                GameEventKeyType.String => reader.ReadString(),
                GameEventKeyType.Float => reader.ReadFloat(),
                GameEventKeyType.Long => reader.ReadSBitLong(32),
                GameEventKeyType.Short => (int)reader.ReadShort(),
                GameEventKeyType.Byte => (int)reader.ReadByte(),
                GameEventKeyType.Bool => reader.ReadOneBit() != 0,
                _ => null,
            };
        }

        return new GameEventValues(id, descriptor.Name, values);
    }
}

public sealed record GameEventValues(int Id, string? Name, IReadOnlyDictionary<string, object?> Keys);
