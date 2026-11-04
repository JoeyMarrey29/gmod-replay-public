using System.Text.Json;

namespace GmodDemo.Parser;

/// <summary>
/// Gameplay net traffic from packets: Lua net, usermessages, prints, game events,
/// entity/voice/sound summaries, and console. PacketEntities / voice / sound bodies stay
/// out of the log — those are already in players/objects/audio.
/// </summary>
public sealed class NetMessageLog
{
    /// <summary>Largest message body written as hex in the net log (raise with --full-bodies).</summary>
    public static int MaxBodyBytes { get; set; } = 2048;

    private readonly List<NetLogRow> _rows = [];

    public IReadOnlyList<NetLogRow> Rows => _rows;

    public void Add(NetLogRow row) => _rows.Add(row);

    public void AddConsole(int tick, string command) =>
        Add(new NetLogRow { T = tick, Type = "console", Text = command });

    public void WriteJsonl(string path)
    {
        using var stream = File.Create(path);
        foreach (var row in _rows)
        {
            using var buffer = new MemoryStream();
            using (var writer = new Utf8JsonWriter(buffer))
            {
                WriteRow(writer, row);
            }

            stream.Write(buffer.ToArray());
            stream.WriteByte((byte)'\n');
        }
    }

    public void WriteClothing(Utf8JsonWriter writer)
    {
        writer.WriteStartArray("clothing");
        foreach (var row in _rows.Where(row =>
                     row.Type == "lua"
                     && row.Name is not null
                     && row.Name.StartsWith("perpheads_bonemerge", StringComparison.Ordinal)
                     && row.Keys is { Count: > 0 }))
        {
            WriteRow(writer, row);
        }

        writer.WriteEndArray();
    }

    public static string AsciiPreview(ReadOnlySpan<byte> body)
    {
        var chars = new char[body.Length];
        for (var i = 0; i < body.Length; i++)
        {
            chars[i] = body[i] is >= 32 and < 127 ? (char)body[i] : '.';
        }

        return new string(chars);
    }

    public static (string? Hex, string? Ascii) BodyFields(byte[]? body)
    {
        if (body is not { Length: > 0 })
        {
            return (null, null);
        }

        var slice = body.Length > MaxBodyBytes ? body.AsSpan(0, MaxBodyBytes) : body.AsSpan();
        var hex = Convert.ToHexString(slice);
        if (body.Length > MaxBodyBytes)
        {
            hex += $"…({body.Length} bytes)";
        }

        return (hex, AsciiPreview(slice));
    }

    internal static void WriteRow(Utf8JsonWriter writer, NetLogRow row)
    {
        writer.WriteStartObject();
        writer.WriteNumber("t", row.T);
        writer.WriteString("type", row.Type);
        WriteOpt(writer, "name", row.Name);
        WriteOpt(writer, "text", row.Text);
        WriteOpt(writer, "id", row.Id);
        WriteOpt(writer, "slot", row.Slot);
        WriteOpt(writer, "subType", row.SubType);
        WriteOpt(writer, "entity", row.Entity);
        WriteOpt(writer, "bits", row.Bits);
        WriteOpt(writer, "bytes", row.Bytes);
        if (row.Delta is { } delta)
        {
            writer.WriteBoolean("delta", delta);
        }

        WriteOpt(writer, "hex", row.Hex);
        WriteOpt(writer, "ascii", row.Ascii);
        if (row.Keys is { Count: > 0 })
        {
            writer.WriteStartObject("keys");
            foreach (var (key, value) in row.Keys)
            {
                switch (value)
                {
                    case null:
                        writer.WriteNull(key);
                        break;
                    case string s:
                        writer.WriteString(key, s);
                        break;
                    case bool b:
                        writer.WriteBoolean(key, b);
                        break;
                    case int i:
                        writer.WriteNumber(key, i);
                        break;
                    case long l:
                        writer.WriteNumber(key, l);
                        break;
                    case float f:
                        writer.WriteNumber(key, f);
                        break;
                    case double d:
                        writer.WriteNumber(key, d);
                        break;
                    default:
                        writer.WriteString(key, value.ToString());
                        break;
                }
            }

            writer.WriteEndObject();
        }

        writer.WriteEndObject();
        writer.Flush();
    }

    private static void WriteOpt(Utf8JsonWriter writer, string name, string? value)
    {
        if (value is { Length: > 0 })
        {
            writer.WriteString(name, value);
        }
    }

    private static void WriteOpt(Utf8JsonWriter writer, string name, int? value)
    {
        if (value is { } n)
        {
            writer.WriteNumber(name, n);
        }
    }
}

public sealed class NetLogRow
{
    public int T { get; init; }

    public required string Type { get; init; }

    public string? Name { get; init; }

    public string? Text { get; init; }

    public int? Id { get; init; }

    public int? Slot { get; init; }

    public int? SubType { get; init; }

    public int? Entity { get; init; }

    public int? Bits { get; init; }

    public int? Bytes { get; init; }

    public bool? Delta { get; init; }

    public string? Hex { get; init; }

    public string? Ascii { get; init; }

    public IReadOnlyDictionary<string, object?>? Keys { get; init; }
}
