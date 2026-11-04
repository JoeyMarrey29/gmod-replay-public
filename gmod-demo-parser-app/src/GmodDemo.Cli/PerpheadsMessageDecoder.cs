using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace GmodDemo.Parser;

/// <summary>
/// Decodes the Perpheads gamemode's own Lua net messages. The formats come from the server's
/// client Lua: <c>sh_stringtable.lua</c> (the shared string table), <c>sh_nwstring.lua</c>
/// (networked entity variables) and the clothing module (<c>cl_clothing_data_store.lua</c>,
/// <c>sh_clothingdata.lua</c>, <c>sh_clothes.lua</c>).
/// </summary>
public sealed partial class PerpheadsMessageDecoder
{
    public sealed record BloodImpact(int Tick, float[] Position, float[]? Normal = null, float Scale = 1);
    private readonly List<BloodImpact> _bloodImpacts = [];
    public IReadOnlyList<BloodImpact> BloodImpacts => _bloodImpacts;
    public sealed record SurfaceImpact(int Tick, string Name, float[] Position, float[] Normal,
        float Scale, float Material);
    private readonly List<SurfaceImpact> _surfaceImpacts = [];
    public IReadOnlyList<SurfaceImpact> SurfaceImpacts => _surfaceImpacts;

    public static bool IsFleshImpact(string effect, float material) =>
        (effect is "gdcw_universal_impact" or "gdcw_universal_penetrate")
        && Math.Ceiling(material) is 45 or 66 or 70;

    private static readonly string[] VarTypes =
        ["String", "Int", "Bool", "Angle", "Vector", "Float", "Entity", "IntList", "KVTable", "Color"];

    private readonly List<string> _strings = [];
    private readonly List<NwEvent> _nwEvents = [];
    private readonly List<ClothingEvent> _clothingEvents = [];
    private readonly List<(int Tick, string Name, byte[] Body)> _clothingMessages = [];
    private int _clothingDataBits = 14;
    private bool _replayingClothing;
    private bool _readLegacyClothing;
    private int _clothingDecodeFailures;

    private sealed record NwEvent(int Tick, int Entity, string Op, string? Key, string? Type, object? Value);

    private sealed record ClothingItem(
        int Type, int DataNumber, uint? ClothingId, string? Name, int Skin, byte[] Color,
        int Decal, byte[]? DecalColor, int LeftMonogram, byte[]? LeftMonogramColor,
        int RightMonogram, byte[]? RightMonogramColor, string? ModelPath = null, int ModelSkin = 0);

    private sealed record ClothingRecord(
        int Sex, string? ModelOverride, int Model, int Skin, int Face, int LeftIris, int RightIris,
        List<ClothingItem> Items, bool ExplicitParts = false);

    private sealed record ClothingEvent(
        int Tick, string Kind, long Index, ClothingRecord? Data, List<int> Entities, int Entity);

    /// <summary>Radio and NPC messages: a flat record per event (see WriteUiEvents).</summary>
    private sealed record UiEvent(
        int Tick, string Kind, int Id, int Entity, string? Text, string? Text2, int[] List, int[] List2);

    private readonly List<UiEvent> _uiEvents = [];
    private readonly List<PerpheadsSoundPlay> _soundPlays = [];

    public int NwVarCount => _nwEvents.Count;

    internal IReadOnlyDictionary<string,object?> NpcPlayerStateAt(int entity,int tick)
    {
        var state=new Dictionary<string,object?>(StringComparer.OrdinalIgnoreCase);
        foreach (var entry in _nwEvents) {
            if (entry.Tick>tick || entry.Entity!=entity) continue;
            if (entry.Key is null) { state.Clear(); continue; }
            state[entry.Key.ToLowerInvariant()]=entry.Value;
        }
        return state;
    }

    public int ClothingEventCount => _clothingEvents.Count;

    public IReadOnlyList<PerpheadsSoundPlay> SoundPlays => _soundPlays;

    /// <summary>Feeds one Lua net message (name from the network string table).</summary>
    public void Process(int tick, string? name, byte[] body, int? bodyBits = null)
    {
        if (name is null)
        {
            return;
        }

        var clothingMessage = name.StartsWith("ph_clothing_data_", StringComparison.Ordinal)
            || name.StartsWith("perpheads_bonemerge", StringComparison.Ordinal);
        if (!_replayingClothing && (clothingMessage
            || name is "perpheads_stringtable_sync" or "perpheads_stringtable_add"))
        {
            _clothingMessages.Add((tick, name, body));
        }
        var clothingStart = _clothingEvents.Count;
        try
        {
            if (name.StartsWith("perpheads_bonemerge", StringComparison.Ordinal))
            {
                if (_replayingClothing && _readLegacyClothing) ReadLegacyClothing(tick, name, body);
                return;
            }
            switch (name)
            {
                case "ph_bloodsplat":
                {
                    var position = new Bits(body).Vector();
                    _bloodImpacts.Add(new BloodImpact(tick, position));
                    break;
                }
                case "perp_effects":
                {
                    var bits = new Bits(body);
                    var effect = ReadStringTable(bits);
                    var position = bits.Vector();
                    var normal = bits.Vector();
                    var scale = bits.Float();
                    var material = bits.Float();
                    if (!float.IsFinite(scale) || !float.IsFinite(material))
                        throw new InvalidDataException($"Non-finite perp_effects parameters at tick {tick}.");
                    // sh_bullets.lua carries the hit material in EffectData's radius.
                    if (IsFleshImpact(effect, material))
                        _bloodImpacts.Add(new BloodImpact(tick, position, normal, scale));
                    else if (effect is "gdcw_universal_impact" or "gdcw_universal_penetrate")
                        _surfaceImpacts.Add(new SurfaceImpact(tick, effect, position, normal, scale, material));
                    break;
                }
                case "perpheads_stringtable_sync":
                    ReadStringTableSync(body);
                    break;
                case "perpheads_stringtable_add":
                    ReadStringTableAdd(body);
                    break;
                case "ph_soundplay":
                    ReadSoundPlay(tick, body, bodyBits);
                    break;
                case "perpheads_NWVars_load_values":
                    ReadLoadValues(tick, body);
                    break;
                case "perpheads_NWVars_unset":
                {
                    var bits = new Bits(body);
                    var entity = (int)bits.UInt(16);
                    var key = ReadStringTable(bits);
                    _nwEvents.Add(new NwEvent(tick, entity, "unset", key, null, null));
                    break;
                }
                case "perpheads_NWVars_remove":
                {
                    var bits = new Bits(body);
                    _nwEvents.Add(new NwEvent(tick, (int)bits.UInt(16), "remove", null, null, null));
                    break;
                }
                case "ph_clothing_data_sync":
                {
                    var bits = new Bits(body);
                    var count = (int)bits.UInt(8);
                    for (var i = 0; i < count; i++)
                    {
                        ReadStoreEntry(tick, "store", bits);
                    }

                    break;
                }
                case "ph_clothing_data_store_new":
                    ReadStoreEntry(tick, "store", new Bits(body));
                    break;
                case "ph_clothing_data_store_reference":
                {
                    var bits = new Bits(body);
                    var entity = (int)bits.UInt(16);
                    var index = bits.UInt(32);
                    _clothingEvents.Add(new ClothingEvent(tick, "reference", index, null, [], entity));
                    break;
                }
                case "ph_clothing_data_store_delete":
                {
                    var bits = new Bits(body);
                    _clothingEvents.Add(new ClothingEvent(tick, "delete", bits.UInt(32), null, [], -1));
                    break;
                }
                case "perpheads_government_radio_sync":
                {
                    // The recording player's radio: channels joined, then channels muted.
                    var bits = new Bits(body);
                    var channels = new List<int>();
                    for (var i = bits.UInt(32); i > 0; i--)
                    {
                        channels.Add((int)bits.UInt(16));
                    }

                    var muted = new List<int>();
                    for (var i = bits.UInt(32); i > 0; i--)
                    {
                        muted.Add((int)bits.UInt(16));
                    }

                    _uiEvents.Add(new UiEvent(tick, "radioSync", 0, 0, null, null, [.. channels], [.. muted]));
                    break;
                }
                case "perpheads_government_radio_channel_sync":
                {
                    var bits = new Bits(body);
                    var id = (int)bits.UInt(16);
                    var add = bits.Bool();
                    _uiEvents.Add(new UiEvent(
                        tick, add ? "radioChannel" : "radioChannelRemoved", id, 0,
                        add ? bits.String() : null, null, [], []));
                    break;
                }
                case "perpheads_npc_used":
                {
                    var bits = new Bits(body);
                    var id = (int)bits.UInt(16);
                    _uiEvents.Add(new UiEvent(tick, "npcUsed", id, (int)bits.UInt(16), null, null, [], []));
                    break;
                }
                case "perpheads_npc_notify":
                {
                    var bits = new Bits(body);
                    _uiEvents.Add(new UiEvent(tick, "npcNotify", 0, 0, bits.String(), bits.String(), [], []));
                    break;
                }
                // cl_chat.lua: message id (32), speaker entity (16), text, category (8).
                case "perp_chat":
                case "perp_ochat":
                case "perp_fchat":
                {
                    var bits = new Bits(body);
                    bits.UInt(32);
                    var speaker = (int)bits.UInt(16);
                    var text = bits.String();
                    var category = (int)bits.UInt(8);
                    var kind = name == "perp_chat" ? "chat" : name == "perp_ochat" ? "chatOoc" : "chatNarration";
                    _uiEvents.Add(new UiEvent(tick, kind, category, speaker, text, null, [], []));
                    break;
                }
                // cl_gun_sounds.lua: GunSounds registration index (8), position (vector). Sent by
                // the server for each shot fired and bullet impact.
                case "ph_bullet_sound":
                case "ph_bullet_impact_sound":
                {
                    var bits = new Bits(body);
                    var index = (int)bits.UInt(8);
                    var pos = bits.Vector();
                    _uiEvents.Add(new UiEvent(
                        tick, name == "ph_bullet_sound" ? "bulletSound" : "impactSound", index, 0,
                        string.Join(' ', pos.Select(v => v.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture))),
                        null, [], []));
                    break;
                }
                // message id (32), sender name, sender team (8), category (8), text.
                case "perpheads_npcchat":
                {
                    var bits = new Bits(body);
                    bits.UInt(32);
                    var from = bits.String();
                    bits.UInt(8);
                    var category = (int)bits.UInt(8);
                    _uiEvents.Add(new UiEvent(tick, "chatNpc", category, 0, from, bits.String(), [], []));
                    break;
                }
                // sh_animations.lua: player entity (16), sequence number (16), gesture slot (32),
                // play once (1 bit).
                case "ph_playanim":
                {
                    var bits = new Bits(body);
                    var player = (int)bits.UInt(16);
                    var sequence = (int)bits.UInt(16);
                    var slot = bits.UInt(32);
                    var once = bits.Bool();
                    _uiEvents.Add(new UiEvent(
                        tick, "playAnim", sequence, player, slot.ToString(), once ? "1" : "0", [], []));
                    break;
                }
                case "ph_stopanim":
                    _uiEvents.Add(new UiEvent(tick, "stopAnim", 0, (int)new Bits(body).UInt(16), null, null, [], []));
                    break;
                case "perpheads_announcechat":
                    _uiEvents.Add(new UiEvent(tick, "chatAnnouncement", 0, 0, new Bits(body).String(), null, [], []));
                    break;
                // cl_notices.lua: text, notice type (4), seconds (8), ...
                case "perp_notify":
                {
                    var bits = new Bits(body);
                    var text = bits.String();
                    _uiEvents.Add(new UiEvent(tick, "notify", (int)bits.UInt(4), 0, text, null, [], []));
                    break;
                }
                default:
                    if (name.StartsWith("perpheads_NWVars_set", StringComparison.Ordinal))
                    {
                        ReadSet(tick, name["perpheads_NWVars_set".Length..], body);
                    }

                    break;
            }
        }
        catch (EndOfStreamException)
        {
            if (name is "ph_bloodsplat" or "perp_effects")
                Console.Error.WriteLine($"BLOOD_IMPACT_DECODE_FAILED: truncated {name} at tick {tick}.");
            if (clothingMessage)
            {
                _clothingEvents.RemoveRange(clothingStart, _clothingEvents.Count - clothingStart);
                _clothingDecodeFailures++;
            }
            // A message that does not match its documented layout is skipped, never guessed.
        }
    }

    /// <summary>Replays clothing after the demo's own server Lua is available.</summary>
    public void ResolveClothingLayout(string? clothingLua, string? legacyClothingLua = null)
    {
        var match = Regex.Match(clothingLua ?? "",
            @"\bnew\.DataNumber\s*=\s*net\.ReadUInt\s*\(\s*(\d+)\s*\)");
        if (match.Success)
        {
            var width = int.Parse(match.Groups[1].Value, System.Globalization.CultureInfo.InvariantCulture);
            if (width is < 1 or > 32)
            {
                throw new InvalidDataException($"Unsupported clothing DataNumber width: {width}.");
            }
            _clothingDataBits = width;
        }
        else if (_clothingMessages.Any(m => m.Name.StartsWith("ph_clothing_data_", StringComparison.Ordinal)))
        {
            Console.Error.WriteLine("CLOTHING_LAYOUT_UNAVAILABLE: server sh_clothes.lua is missing or unsupported; using the current 14-bit layout.");
        }

        var strings = _strings.ToArray();
        _strings.Clear();
        _clothingEvents.Clear();
        _legacyClothing.Clear();
        _clothingDecodeFailures = 0;
        _readLegacyClothing = !_clothingMessages.Any(m =>
            m.Name.StartsWith("ph_clothing_data_", StringComparison.Ordinal));
        if (_readLegacyClothing && _clothingMessages.Any(m => m.Name == "perpheads_bonemerge"))
            ResolveLegacyClothingLayout(legacyClothingLua);
        _replayingClothing = true;
        try
        {
            foreach (var (tick, name, body) in _clothingMessages)
            {
                Process(tick, name, body);
            }
        }
        finally
        {
            _replayingClothing = false;
            _strings.Clear();
            _strings.AddRange(strings);
        }
        Console.WriteLine($"CLOTHING_LAYOUT data_bits={_clothingDataBits} source={(match.Success ? "server-lua" : "current-default")} events={_clothingEvents.Count} failed_messages={_clothingDecodeFailures}");
    }

    private void ReadStringTableSync(byte[] body)
    {
        var bits = new Bits(body);
        var length = (int)bits.UInt(16);
        var compressed = bits.Bytes(length);
        _strings.Clear();
        if (compressed.Length < 13)
        {
            return;
        }

        var data = GmodLzma.DecompressLzmaFile(compressed);
        if (data is not { Length: > 0 })
        {
            return;
        }

        foreach (var text in Encoding.Latin1.GetString(data).Split('\0'))
        {
            _strings.Add(text);
        }
    }

    private void ReadStringTableAdd(byte[] body)
    {
        var bits = new Bits(body);
        var count = (int)bits.UInt(16);
        for (var i = 0; i < count; i++)
        {
            _strings.Add(bits.String());
        }
    }

    private string ReadStringTable(Bits bits)
    {
        if (bits.Bool())
        {
            var index = (int)bits.UInt(16);
            return index >= 1 && index <= _strings.Count ? _strings[index - 1] : $"#{index}";
        }

        return bits.String();
    }

    private void ReadSoundPlay(int tick, byte[] body, int? bodyBits)
    {
        if (bodyBits is { } declaredLength
            && (declaredLength < 0 || declaredLength > body.Length * 8))
        {
            return;
        }

        var bits = new Bits(body, bodyBits);
        var name = ReadStringTable(bits);
        var position = bits.Vector();
        var level = (int)bits.UInt(8);
        var pitch = (int)bits.UInt(8);
        var volume = bits.Float();
        var hasExactLength = bodyBits.HasValue;

        if ((hasExactLength && bits.Remaining != 0)
            || (!hasExactLength && bits.Remaining >= 8)
            || string.IsNullOrWhiteSpace(name)
            || name[0] == '#'
            || position.Any(value => !float.IsFinite(value))
            || !float.IsFinite(volume))
        {
            return;
        }

        _soundPlays.Add(new PerpheadsSoundPlay(
            tick, name, position[0], position[1], position[2], level, pitch, volume));
    }

    private static object? ReadValue(Bits bits, string type) => type switch
    {
        "String" => bits.String(),
        "Int" => bits.Int(32),
        "Bool" => bits.Bool(),
        "Float" => bits.Float(),
        "Entity" => (int)bits.UInt(16),
        "Vector" => new[] { bits.Float(), bits.Float(), bits.Float() },
        "Color" => new[] { (int)bits.UInt(8), (int)bits.UInt(8), (int)bits.UInt(8), (int)bits.UInt(8) },
        "IntList" => ReadIntList(bits),
        "KVTable" => ReadKeyValueTable(bits),
        _ => throw new EndOfStreamException($"Unsupported NWVar type {type}."),
    };

    private static int[] ReadIntList(Bits bits)
    {
        var amount = (int)bits.UInt(16);
        var width = (int)bits.UInt(6);
        var list = new int[amount];
        for (var i = 0; i < amount; i++)
        {
            list[i] = (int)bits.UInt(width);
        }

        return list;
    }

    private static Dictionary<string, int> ReadKeyValueTable(Bits bits)
    {
        var count = (int)bits.UInt(6);
        var table = new Dictionary<string, int>();
        for (var i = 0; i < count; i++)
        {
            var key = (int)bits.UInt(6);
            table[key.ToString()] = (int)bits.UInt(6);
        }

        return table;
    }

    private void ReadSet(int tick, string type, byte[] body)
    {
        if (Array.IndexOf(VarTypes, type) < 0)
        {
            return;
        }

        var bits = new Bits(body);
        var entity = (int)bits.UInt(16);
        var key = ReadStringTable(bits);
        _nwEvents.Add(new NwEvent(tick, entity, "set", key, type, ReadValue(bits, type)));
    }

    private void ReadLoadValues(int tick, byte[] body)
    {
        var bits = new Bits(body);
        var entity = (int)bits.UInt(16);
        var count = (int)bits.UInt(16);
        for (var i = 0; i < count; i++)
        {
            var key = ReadStringTable(bits);
            var typeId = (int)bits.UInt(8);
            if (typeId < 1 || typeId > VarTypes.Length)
            {
                return;
            }

            var type = VarTypes[typeId - 1];
            _nwEvents.Add(new NwEvent(tick, entity, "set", key, type, ReadValue(bits, type)));
        }
    }

    private void ReadStoreEntry(int tick, string kind, Bits bits)
    {
        var index = bits.UInt(32);
        var data = ReadClothingData(bits);
        var referenceCount = (int)bits.UInt(8);
        var entities = new List<int>(referenceCount);
        for (var i = 0; i < referenceCount; i++)
        {
            entities.Add((int)bits.UInt(16));
        }

        _clothingEvents.Add(new ClothingEvent(tick, kind, index, data, entities, -1));
    }

    private ClothingRecord ReadClothingData(Bits bits)
    {
        var sex = (int)bits.UInt(1);
        string? modelOverride = bits.Bool() ? ReadStringTable(bits) : null;
        var model = (int)bits.UInt(8);
        var skin = (int)bits.UInt(8);
        var face = (int)bits.UInt(8);
        var leftIris = (int)bits.UInt(8);
        var rightIris = (int)bits.UInt(8);
        var count = (int)bits.UInt(8);
        var items = new List<ClothingItem>(count);
        for (var i = 0; i < count; i++)
        {
            items.Add(ReadClothing(bits));
        }

        return new ClothingRecord(sex, modelOverride, model, skin, face, leftIris, rightIris, items);
    }

    private static byte[] ReadColor(Bits bits) =>
        [(byte)bits.UInt(8), (byte)bits.UInt(8), (byte)bits.UInt(8), (byte)bits.UInt(8)];

    private ClothingItem ReadClothing(Bits bits)
    {
        var type = (int)bits.UInt(16);
        var dataNumber = (int)bits.UInt(_clothingDataBits);
        uint? clothingId = null;
        string? customName = null;
        if (bits.Bool())
        {
            clothingId = bits.UInt(32);
            if (bits.Bool())
            {
                customName = bits.String();
            }
        }

        var skin = (int)bits.UInt(8);
        var color = ReadColor(bits);
        var decal = 0;
        byte[]? decalColor = null;
        if (bits.Bool())
        {
            decal = (int)bits.UInt(8);
            decalColor = ReadColor(bits);
        }

        var left = 0;
        byte[]? leftColor = null;
        if (bits.Bool())
        {
            left = (int)bits.UInt(8);
            leftColor = ReadColor(bits);
        }

        var right = 0;
        byte[]? rightColor = null;
        if (bits.Bool())
        {
            right = (int)bits.UInt(8);
            rightColor = ReadColor(bits);
        }

        return new ClothingItem(
            type, dataNumber, clothingId, customName, skin, color, decal, decalColor,
            left, leftColor, right, rightColor);
    }

    // ---- JSON ---------------------------------------------------------------------------

    public void WriteNwVars(Utf8JsonWriter writer, double tickRate)
    {
        writer.WriteStartArray("nwvars");
        foreach (var item in _nwEvents)
        {
            writer.WriteStartObject();
            writer.WriteNumber("t", item.Tick);
            writer.WriteNumber("time", tickRate > 0 ? item.Tick / tickRate : 0);
            writer.WriteNumber("e", item.Entity);
            writer.WriteString("op", item.Op);
            if (item.Key is not null)
            {
                writer.WriteString("key", item.Key);
            }

            if (item.Type is not null)
            {
                writer.WriteString("type", item.Type);
                writer.WritePropertyName("value");
                WriteValue(writer, item.Value);
            }

            writer.WriteEndObject();
        }

        writer.WriteEndArray();
    }

    private static void WriteValue(Utf8JsonWriter writer, object? value)
    {
        switch (value)
        {
            case null:
                writer.WriteNullValue();
                break;
            case string text:
                writer.WriteStringValue(text);
                break;
            case bool flag:
                writer.WriteBooleanValue(flag);
                break;
            case int number:
                writer.WriteNumberValue(number);
                break;
            case float number when float.IsFinite(number):
                writer.WriteNumberValue(number);
                break;
            case float:
                writer.WriteNullValue();
                break;
            case float[] vector:
                writer.WriteStartArray();
                foreach (var part in vector)
                {
                    if (float.IsFinite(part))
                    {
                        writer.WriteNumberValue(part);
                    }
                    else
                    {
                        writer.WriteNullValue();
                    }
                }

                writer.WriteEndArray();
                break;
            case int[] list:
                writer.WriteStartArray();
                foreach (var part in list)
                {
                    writer.WriteNumberValue(part);
                }

                writer.WriteEndArray();
                break;
            case Dictionary<string, int> table:
                writer.WriteStartObject();
                foreach (var (key, number) in table)
                {
                    writer.WriteNumber(key, number);
                }

                writer.WriteEndObject();
                break;
            default:
                writer.WriteStringValue(value.ToString());
                break;
        }
    }

    public void WriteUiEvents(Utf8JsonWriter writer, double tickRate)
    {
        writer.WriteStartArray("uiEvents");
        foreach (var item in _uiEvents)
        {
            writer.WriteStartObject();
            writer.WriteNumber("t", item.Tick);
            writer.WriteNumber("time", tickRate > 0 ? item.Tick / tickRate : 0);
            writer.WriteString("kind", item.Kind);
            if (item.Id != 0)
            {
                writer.WriteNumber("id", item.Id);
            }

            if (item.Entity != 0)
            {
                writer.WriteNumber("e", item.Entity);
            }

            if (item.Text is not null)
            {
                writer.WriteString("text", item.Text);
            }

            if (item.Text2 is not null)
            {
                writer.WriteString("text2", item.Text2);
            }

            if (item.Kind == "radioSync")
            {
                writer.WriteStartArray("channels");
                foreach (var channel in item.List)
                {
                    writer.WriteNumberValue(channel);
                }

                writer.WriteEndArray();
                writer.WriteStartArray("muted");
                foreach (var channel in item.List2)
                {
                    writer.WriteNumberValue(channel);
                }

                writer.WriteEndArray();
            }

            writer.WriteEndObject();
        }

        writer.WriteEndArray();
    }

    public void WriteClothingData(Utf8JsonWriter writer, double tickRate)
    {
        writer.WriteStartArray("clothingData");
        foreach (var item in _clothingEvents)
        {
            writer.WriteStartObject();
            writer.WriteNumber("t", item.Tick);
            writer.WriteNumber("time", tickRate > 0 ? item.Tick / tickRate : 0);
            writer.WriteString("kind", item.Kind);
            writer.WriteNumber("index", item.Index);
            if (item.Entity >= 0)
            {
                writer.WriteNumber("e", item.Entity);
            }

            if (item.Data is { } data)
            {
                writer.WriteStartArray("entities");
                foreach (var entity in item.Entities)
                {
                    writer.WriteNumberValue(entity);
                }

                writer.WriteEndArray();
                writer.WriteNumber("sex", data.Sex);
                if (data.ExplicitParts) writer.WriteBoolean("explicitParts", true);
                if (data.ModelOverride is not null)
                {
                    writer.WriteString("modelOverride", data.ModelOverride);
                }

                writer.WriteNumber("model", data.Model);
                writer.WriteNumber("skin", data.Skin);
                writer.WriteNumber("face", data.Face);
                writer.WriteNumber("leftIris", data.LeftIris);
                writer.WriteNumber("rightIris", data.RightIris);
                writer.WriteStartArray("clothing");
                foreach (var clothing in data.Items)
                {
                    writer.WriteStartObject();
                    writer.WriteNumber("type", clothing.Type);
                    writer.WriteNumber("skin", clothing.Skin);
                    if (clothing.ModelPath is not null)
                    {
                        writer.WriteString("modelPath", clothing.ModelPath);
                        writer.WriteNumber("modelSkin", clothing.ModelSkin);
                    }
                    WriteColor(writer, "color", clothing.Color);
                    if (clothing.DataNumber != 0)
                    {
                        writer.WriteNumber("dataNumber", clothing.DataNumber);
                    }

                    if (clothing.ClothingId is { } id)
                    {
                        writer.WriteNumber("clothingId", id);
                    }

                    if (clothing.Name is not null)
                    {
                        writer.WriteString("name", clothing.Name);
                    }

                    if (clothing.Decal != 0)
                    {
                        writer.WriteNumber("decal", clothing.Decal);
                        WriteColor(writer, "decalColor", clothing.DecalColor);
                    }

                    if (clothing.LeftMonogram != 0)
                    {
                        writer.WriteNumber("leftMonogram", clothing.LeftMonogram);
                        WriteColor(writer, "leftMonogramColor", clothing.LeftMonogramColor);
                    }

                    if (clothing.RightMonogram != 0)
                    {
                        writer.WriteNumber("rightMonogram", clothing.RightMonogram);
                        WriteColor(writer, "rightMonogramColor", clothing.RightMonogramColor);
                    }

                    writer.WriteEndObject();
                }

                writer.WriteEndArray();
            }

            writer.WriteEndObject();
        }

        writer.WriteEndArray();
    }

    private static void WriteColor(Utf8JsonWriter writer, string name, byte[]? color)
    {
        writer.WriteStartArray(name);
        foreach (var part in color ?? [255, 255, 255, 255])
        {
            writer.WriteNumberValue(part);
        }

        writer.WriteEndArray();
    }

    // ---- bit reader ---------------------------------------------------------------------

    /// <summary>Little-endian bit reader matching Garry's Mod's net library.</summary>
    private sealed class Bits(byte[] data, int? bitLength = null)
    {
        private int _position;

        private int Total => bitLength ?? data.Length * 8;

        public int Remaining => Total - _position;

        public bool Bool() => UInt(1) != 0;

        public uint UInt(int width)
        {
            if (width < 0 || width > 32 || _position + width > Total)
            {
                throw new EndOfStreamException();
            }

            ulong value = 0;
            for (var i = 0; i < width; i++)
            {
                var bit = (data[_position >> 3] >> (_position & 7)) & 1;
                value |= (ulong)bit << i;
                _position++;
            }

            return (uint)value;
        }

        public int Int(int width)
        {
            var raw = UInt(width);
            return width == 32 ? unchecked((int)raw) : (int)raw;
        }

        public float Float() => BitConverter.UInt32BitsToSingle(UInt(32));

        // net.WriteVector: the engine's bf_write::WriteBitVec3Coord (a presence bit per axis,
        // then each present axis as an integer and 1/32 fraction with a sign).
        public float[] Vector()
        {
            var present = new[] { Bool(), Bool(), Bool() };
            var result = new float[3];
            for (var axis = 0; axis < 3; axis++)
            {
                if (!present[axis])
                {
                    continue;
                }

                var hasInt = Bool();
                var hasFraction = Bool();
                if (!hasInt && !hasFraction)
                {
                    continue;
                }

                var negative = Bool();
                var whole = hasInt ? UInt(14) + 1 : 0;
                var fraction = hasFraction ? UInt(5) : 0;
                var value = whole + fraction / 32.0f;
                result[axis] = negative ? -value : value;
            }

            return result;
        }

        public string String()
        {
            var bytes = new List<byte>();
            while (true)
            {
                var value = (byte)UInt(8);
                if (value == 0)
                {
                    break;
                }

                bytes.Add(value);
            }

            return Encoding.UTF8.GetString(bytes.ToArray());
        }

        public byte[] Bytes(int count)
        {
            var result = new byte[count];
            for (var i = 0; i < count; i++)
            {
                result[i] = (byte)UInt(8);
            }

            return result;
        }
    }
}
