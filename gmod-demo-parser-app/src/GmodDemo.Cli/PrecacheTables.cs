namespace GmodDemo.Parser;

/// <summary>
/// Recovers <c>modelprecache</c> (index → <c>models/…mdl</c>) and <c>networkstring</c>
/// (Lua class / net-message names). Creation messages retain exact table metadata for
/// incremental model additions; the engine's <c>dem_stringtables</c> dump seeds the other
/// tables and supplies model paths when the full signon cannot be walked.
/// </summary>
public sealed class PrecacheTables
{
    public sealed record LightStyle(int Tick, int Style, string Pattern);
    public List<LightStyle> LightStyles { get; } = [];
    private readonly Dictionary<int, string> _lightStyles = [];
    private readonly Dictionary<int, string> _models = [];
    private readonly Dictionary<int, string> _network = [];
    private readonly Dictionary<int, string> _sounds = [];
    private readonly Dictionary<int, string> _effects = [];
    private readonly Dictionary<int, string> _decals = [];
    private readonly List<StringTable?> _tablesById = [];
    private readonly Dictionary<int, byte[]> _instanceBaselines = [];
    public IReadOnlyDictionary<int, byte[]> InstanceBaselines => _instanceBaselines;
    public int ModelUpdateFailures { get; private set; }
    public IReadOnlyDictionary<int, string> Effects => _effects;
    public IReadOnlyDictionary<int, string> Decals => _decals;

    public IReadOnlyDictionary<int, string> Models => _models;

    public IReadOnlyDictionary<int, string> Sounds => _sounds;

    public IReadOnlyDictionary<int, string> NetworkStrings => _network;

    public IReadOnlyList<string> LuaFiles => LuaFileRecords.Select(r => r.Path).ToList();

    public IReadOnlyList<LuaFileRecord> LuaFileRecords { get; private set; } = [];

    internal void SetLuaFileRecords(IReadOnlyList<LuaFileRecord> records) =>
        LuaFileRecords = records;

    public void AddPayload(byte[] payload, int tick = 0)
    {
        var dump = DemoStringTables.ParseFull(payload);
        if (dump is not null)
        {
            if (dump.TryGetValue("lightstyles", out var styles)) AddLightStyles(styles, tick);
            if (dump.TryGetValue("instancebaseline", out var baselines))
                AddInstanceBaselines(baselines);
            LoadList(dump, "modelprecache", _models, IsModelPath, preserveExisting: true);
            LoadList(dump, "networkstring", _network, s => s.Length > 0);
            LoadList(dump, "soundprecache", _sounds, s => s.Length > 0);
            LoadList(dump, "EffectDispatch", _effects, s => s.Length > 0);
            LoadList(dump, "decalprecache", _decals, s => s.Length > 0);
            if (dump.TryGetValue("client_lua_files", out var lua))
            {
                LuaFileRecords = lua
                    .Where(e => e.Text is { Length: > 0 } && e.Text != "paths")
                    .Select(e => new LuaFileRecord(e.Text!, e.UserData))
                    .ToList();
            }
            return;
        }

        if (_models.Count < 8)
        {
            TryLoadCreate(payload, "modelprecache", _models, IsModelPath);
        }

        if (_network.Count < 8)
        {
            TryLoadCreate(payload, "networkstring", _network, s => s.Length > 0);
        }
    }

    public void AddPacket(byte[] payload, IReadOnlyList<NetMessageSpan> messages, int tick = 0)
    {
        foreach (var message in messages)
        {
            var reader = new BitReader(payload);
            reader.Seek(message.StartBit + NetMessageLayout.TypeBits);

            if (message.Type == NetMessageType.SvcCreateStringTable)
            {
                var table = StringTableParser.ReadCreate(ref reader);
                _tablesById.Add(table);
                if (table is not null)
                {
                    AddEntries(table, table.Entries.Values, tick);
                }
            }
            else if (message.Type == NetMessageType.SvcUpdateStringTable)
            {
                var tableId = (int)reader.ReadUBitLong(5);
                if (reader.IsOverflowed || tableId < 0 || tableId >= _tablesById.Count
                    || _tablesById[tableId] is null)
                {
                    continue;
                }

                ApplyUpdate(payload, message.StartBit, tick);
            }
        }
    }

    private void ApplyUpdate(byte[] payload, int startBit, int tick)
    {
        var reader = new BitReader(payload);
        reader.Seek(startBit + NetMessageLayout.TypeBits);
        var tableId = (int)reader.PeekUBitLong(5);
        if (reader.IsOverflowed || tableId < 0 || tableId >= _tablesById.Count
            || _tablesById[tableId] is not { } table)
        {
            return;
        }

        var entries = StringTableParser.ReadUpdate(ref reader, table);
        if (entries is null)
        {
            if (table.Name == "modelprecache") ModelUpdateFailures++;
            return;
        }

        if (table.Name == "instancebaseline")
            entries = entries.Select(entry => entry with
            {
                Text = entry.Text ?? table.Entries.GetValueOrDefault(entry.Index)?.Text,
            }).ToList();
        table.Apply(entries);
        AddEntries(table, entries, tick);
    }

    public string? ModelPath(int? index) =>
        index is { } i && i > 0 && _models.TryGetValue(i, out var path) ? path : null;

    public string? NetworkName(int? index) =>
        index is { } i && i > 0 && _network.TryGetValue(i, out var name) ? name : null;

    private static bool IsModelPath(string text) =>
        text.Contains(".mdl", StringComparison.OrdinalIgnoreCase)
        || text.StartsWith("models/", StringComparison.OrdinalIgnoreCase)
        || text.StartsWith("*", StringComparison.Ordinal);

    private void AddEntries(StringTable table, IEnumerable<StringTableEntry> entries, int tick)
    {
        if (table.Name == "lightstyles") { AddLightStyles(entries, tick); return; }
        if (table.Name == "instancebaseline")
        {
            AddInstanceBaselines(entries);
            return;
        }
        var target = table.Name switch
        {
            "modelprecache" => _models,
            "soundprecache" => _sounds,
            _ => null,
        };
        if (target is null)
        {
            return;
        }

        foreach (var entry in entries)
        {
            if (entry.Text is { Length: > 0 } text
                && (table.Name != "modelprecache" || IsModelPath(text)))
            {
                target[entry.Index] = text;
            }
        }
    }

    private void AddLightStyles(IEnumerable<StringTableEntry> entries, int tick)
    {
        foreach (var entry in entries)
        {
            // The table index is the style number; the NUL-terminated user data is
            // the brightness pattern. Updates commonly omit the entry's string.
            if (entry.Index is < 0 or > 255 || entry.UserData is not { Length: > 0 } data) continue;
            var end = Array.IndexOf(data, (byte)0);
            var pattern = System.Text.Encoding.ASCII.GetString(data, 0, end < 0 ? data.Length : end);
            if (pattern.Length == 0 || pattern.Any(c => c is < 'a' or > 'z')) continue;
            if (_lightStyles.GetValueOrDefault(entry.Index) == pattern) continue;
            _lightStyles[entry.Index] = pattern;
            LightStyles.Add(new LightStyle(tick, entry.Index, pattern));
        }
    }

    private void AddInstanceBaselines(IEnumerable<StringTableEntry> entries)
    {
        foreach (var entry in entries)
            if (int.TryParse(entry.Text, out var classId) && entry.UserData is { Length: > 0 } data)
                _instanceBaselines[classId] = data;
    }

    private static void LoadList(
        Dictionary<string, List<StringTableEntry>> dump,
        string tableName,
        Dictionary<int, string> target,
        Func<string, bool> keep,
        bool preserveExisting = false)
    {
        if (!dump.TryGetValue(tableName, out var entries))
        {
            return;
        }

        for (var i = 0; i < entries.Count; i++)
        {
            if (entries[i].Text is { Length: > 0 } text && keep(text))
            {
                if (preserveExisting)
                {
                    target.TryAdd(i, text);
                }
                else
                {
                    target[i] = text;
                }
            }
        }
    }

    private static void TryLoadCreate(byte[] payload, string tableName, Dictionary<int, string> target, Func<string, bool> keep)
    {
        var needle = System.Text.Encoding.ASCII.GetBytes(tableName);
        foreach (var nameBit in FindAllBitAligned(payload, needle))
        {
            if (nameBit < NetMessageLayout.TypeBits)
            {
                continue;
            }

            var start = nameBit - NetMessageLayout.TypeBits;
            var reader = new BitReader(payload);
            reader.Seek(start);
            if (reader.ReadUBitLong(NetMessageLayout.TypeBits) != (uint)NetMessageType.SvcCreateStringTable)
            {
                continue;
            }

            var table = StringTableParser.ReadCreate(ref reader);
            if (table is null || !table.Name.Equals(tableName, StringComparison.Ordinal))
            {
                continue;
            }

            if ((table.MaxEntries & (table.MaxEntries - 1)) != 0)
            {
                continue;
            }

            var loaded = 0;
            foreach (var entry in table.Entries.Values)
            {
                if (entry.Text is { Length: > 0 } text && keep(text))
                {
                    target[entry.Index] = text;
                    loaded++;
                }
            }

            if (loaded > 0)
            {
                return;
            }
        }
    }

    private static IEnumerable<int> FindAllBitAligned(byte[] payload, byte[] needle)
    {
        var totalBits = payload.Length * 8;
        if (totalBits < needle.Length * 8)
        {
            yield break;
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
            var from = 0;
            while (from + needle.Length <= shifted.Length)
            {
                var at = shifted.AsSpan(from).IndexOf(needle);
                if (at < 0)
                {
                    break;
                }

                yield return shift + ((from + at) * 8);
                from += at + 1;
            }
        }
    }
}

public sealed record LuaFileRecord(string Path, byte[]? UserData);
