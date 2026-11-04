using System.Globalization;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using GmodDemo.Parser;
using MoonSharp.Interpreter;

namespace GmodDemo.Cli;

/// <summary>
/// Builds the Perpheads data files the viewer reads (jobs, weapons, clothing) from the server
/// Lua carried by a demo and its gluapack. This is the in-process port of
/// tools/export/export_jobs.py, export_weapons.py and export_clothing.py.
/// </summary>
internal static partial class PerpData
{
    public const string ClothingFolder = "gamemodes/darkrp/gamemode/modules/clothing/";

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    public sealed record LuaTree(
        IReadOnlyList<(string Path, string Text)> Files,
        Gluapack Pack,
        string? PackPath,
        int Stubs);

    /// <summary>Walks the demo and returns every client Lua file, unpacking the gluapack.</summary>
    public static LuaTree LoadLua(string demoPath, string? forcedPack)
    {
        using var reader = DemoReader.Open(demoPath);
        var events = new EventCollector();
        events.SetRecorderName(reader.Header.ClientName);
        foreach (var command in reader.ReadCommands())
        {
            switch (command)
            {
                case PacketCommand packet:
                    events.AddPacket(packet);
                    break;
                case DataTablesCommand dataTables:
                    events.AddDataTables(dataTables);
                    break;
                case StringTablesCommand stringTables:
                    events.Precache.AddPayload(stringTables.Payload);
                    break;
            }
        }

        events.Gluapack.CollectFromDemo(events.Precache);
        return BuildTree(events, demoPath, forcedPack);
    }

    /// <summary>Unpacks the gluapack (when needed) for an already walked demo and lists its Lua.</summary>
    public static LuaTree BuildTree(EventCollector events, string demoPath, string? forcedPack)
    {
        var precache = events.Precache;
        var pack = events.Gluapack;
        string? packPath = null;
        if (pack.Detected && !pack.HasPack)
        {
            packPath = forcedPack ?? pack.SuggestPackPath(Path.GetDirectoryName(Path.GetFullPath(demoPath)));
            if (packPath is not null && pack.TryLoadFile(packPath))
            {
                pack.BindAfterLoad(precache);
            }
        }

        var files = new List<(string, string)>();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        void Add(string path, string text)
        {
            var key = NormalizePath(path);
            if (seen.Add(key))
            {
                files.Add((key, text.Replace("\r\n", "\n").Replace('\r', '\n')));
            }
        }

        foreach (var record in precache.LuaFileRecords)
        {
            var text = pack.FileForPath(record.Path) ?? LuaExporter.FromUserData(record.UserData);
            if (text is not null)
            {
                Add(record.Path, text);
            }
        }

        if (pack.HasPack)
        {
            foreach (var path in precache.LuaFiles)
            {
                if (pack.FileForPath(path) is { } text)
                {
                    Add(path, text);
                }
            }

            // dem_stringtables can truncate before these module paths. Resolve only
            // hash-verified bodies from this recording's pack, never another server version.
            foreach (var name in AttachmentFiles)
            {
                var path = AttachmentFolder + name;
                if (pack.FileForPath(path) is { } text) Add(path, text);
            }

            foreach (var (hash, text) in pack.UnmappedFiles())
            {
                Add("_unmapped/" + hash + ".lua", text);
            }
        }

        var stubs = files.Count(f => Gluapack.IsStub(f.Item2));
        return new LuaTree(files, pack, packPath, stubs);
    }

    private static string NormalizePath(string path)
    {
        var n = path.Replace('\\', '/').Trim().TrimStart('/');
        if (!n.EndsWith(".lua", StringComparison.OrdinalIgnoreCase)
            && !n.EndsWith(".txt", StringComparison.OrdinalIgnoreCase))
        {
            n += ".lua";
        }

        return n;
    }

    private static bool IsLua(string path) => path.EndsWith(".lua", StringComparison.OrdinalIgnoreCase);

    // ---- jobs (export_jobs.py) ----------------------------------------------------------

    public static JsonObject Jobs(IReadOnlyList<(string Path, string Text)> files, out int teamCount)
    {
        var lua = files.Where(f => IsLua(f.Path)).ToList();

        // Team ids come from sh_teams.lua; names and colours from team.SetUp in sh_jobs.lua.
        var teamIds = new Dictionary<string, int>();
        foreach (var (path, text) in lua)
        {
            if (path.EndsWith("core/sh_teams.lua", StringComparison.OrdinalIgnoreCase))
            {
                teamIds = Regex.Matches(text, @"(TEAM_\w+)\s*=\s*(\d+)")
                    .GroupBy(m => m.Groups[1].Value)
                    .ToDictionary(g => g.Key, g => int.Parse(g.Last().Groups[2].Value, CultureInfo.InvariantCulture));
            }
        }

        var teams = new SortedDictionary<int, JsonObject>();
        foreach (var (path, text) in lua)
        {
            if (!path.EndsWith("jobs/sh_jobs.lua", StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            foreach (Match m in Regex.Matches(
                         text, @"team\.SetUp\((TEAM_\w+),\s*""([^""]+)"",\s*Color\((\d+),\s*(\d+),\s*(\d+)\)\)"))
            {
                if (!teamIds.TryGetValue(m.Groups[1].Value, out var id))
                {
                    continue;
                }

                teams[id] = new JsonObject
                {
                    ["id"] = id,
                    ["key"] = m.Groups[1].Value,
                    ["name"] = m.Groups[2].Value,
                    ["color"] = new JsonArray(Int(m.Groups[3]), Int(m.Groups[4]), Int(m.Groups[5])),
                };
            }
        }

        // Rank tables: <Table>.RANK_X = n, then <Table>.RankNames / RankShortNames blocks.
        var ranks = new Dictionary<string, (JsonObject Names, JsonObject Short)>();
        foreach (var (_, text) in lua)
        {
            foreach (var table in new[] { "PoliceWhitelist", "MedicJob", "FirefighterJob" })
            {
                var constants = new Dictionary<string, int>();
                foreach (Match m in Regex.Matches(text, table + @"\.(RANK_\w+)\s*=\s*(\d+)"))
                {
                    constants[m.Groups[1].Value] = Int(m.Groups[2]);
                }

                if (constants.Count == 0)
                {
                    continue;
                }

                if (!ranks.TryGetValue(table, out var entry))
                {
                    entry = (new JsonObject(), new JsonObject());
                    ranks[table] = entry;
                }

                foreach (var (target, field) in new[] { (entry.Names, "RankNames"), (entry.Short, "RankShortNames") })
                {
                    var block = Regex.Match(text, table + @"\." + field + @"\s*=\s*\{(.*?)\n\}", RegexOptions.Singleline);
                    if (!block.Success)
                    {
                        continue;
                    }

                    foreach (Match m in Regex.Matches(block.Groups[1].Value, @"\[" + table + @"\.(RANK_\w+)\]\s*=\s*""([^""]*)"""))
                    {
                        if (constants.TryGetValue(m.Groups[1].Value, out var rank))
                        {
                            target[rank.ToString(CultureInfo.InvariantCulture)] = m.Groups[2].Value;
                        }
                    }
                }
            }
        }

        var radio = new JsonObject();
        foreach (var (_, text) in lua)
        {
            var block = Regex.Match(text, @"GovernmentRadioSystem\.ChannelSettings\s*=\s*\{(.*?)\n\}", RegexOptions.Singleline);
            if (!block.Success)
            {
                continue;
            }

            foreach (Match m in Regex.Matches(block.Groups[1].Value, @"\[(\d+)\]\s*=\s*\{[^{}]*?name\s*=\s*""([^""]*)""", RegexOptions.Singleline))
            {
                radio[m.Groups[1].Value] = m.Groups[2].Value;
            }
        }

        var npcs = new JsonObject();
        foreach (var (_, text) in lua)
        {
            var name = Regex.Match(text, @"^\s*NPC\.Name\s*=\s*""([^""]*)""", RegexOptions.Multiline);
            var npcId = Regex.Match(text, @"^\s*NPC\.ID\s*=\s*(\d+)", RegexOptions.Multiline);
            if (name.Success && npcId.Success)
            {
                npcs[npcId.Groups[1].Value] = name.Groups[1].Value;
            }
        }

        int Team(string key) => teamIds.TryGetValue(key, out var id)
            ? id
            : throw new InvalidDataException($"{key} is not defined in core/sh_teams.lua.");

        JsonObject RankEntry(string table) => ranks.TryGetValue(table, out var e)
            ? new JsonObject { ["names"] = e.Names, ["short"] = e.Short }
            : new JsonObject();

        var teamsJson = new JsonObject();
        foreach (var (id, team) in teams)
        {
            teamsJson[id.ToString(CultureInfo.InvariantCulture)] = team;
        }

        teamCount = teams.Count;
        return new JsonObject
        {
            ["teams"] = teamsJson,
            ["npcs"] = npcs,
            ["radioChannels"] = radio,
            ["ranks"] = new JsonObject
            {
                ["police"] = RankEntry("PoliceWhitelist"),
                ["medic"] = RankEntry("MedicJob"),
                ["firefighter"] = RankEntry("FirefighterJob"),
            },
            ["rankVariables"] = new JsonObject
            {
                [Team("TEAM_POLICE").ToString(CultureInfo.InvariantCulture)] = "policerank",
                [Team("TEAM_MEDIC").ToString(CultureInfo.InvariantCulture)] = "medicrank",
                [Team("TEAM_FIREFIGHTER").ToString(CultureInfo.InvariantCulture)] = "firefighterrank",
            },
            ["dispatcherTeam"] = Team("TEAM_DISPATCHER"),
            ["citizenTeam"] = Team("TEAM_CITIZEN"),
        };
    }

    private static int Int(Group group) => int.Parse(group.Value, CultureInfo.InvariantCulture);

    // ---- weapons (export_weapons.py) ----------------------------------------------------

    private const string SwepString = @"SWEP\.{0}\s*=\s*(?:Model\(|Sound\()?\s*""([^""]+)""";
    private const string SwepVector = @"SWEP\.{0}\s*=\s*Vector\(\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*\)";
    private const string SwepNumber = @"SWEP\.{0}\s*=\s*([-\d.]+)";

    // Bases derived from the firearm base (perpheads_gun_base: IronSights float slot 4, Passive
    // bool slot 0, Reloading bool slot 1 in its SetupDataTables).
    private static readonly HashSet<string> GunBases =
    [
        "perpheads_gun_base", "perpheads_pistol_base", "perpheads_rifle_base",
        "perpheads_revolver_base", "perpheads_shotgun_base", "perpheads_sniper_base",
        "perpheads_nomag_base",
    ];

    public sealed record Weapons(JsonObject Models, JsonObject Sounds, JsonObject Info);

    public static Weapons Sweps(IReadOnlyList<(string Path, string Text)> files)
    {
        static Match Find(string format, string field, string text) =>
            Regex.Match(text, string.Format(CultureInfo.InvariantCulture, format, field));

        static string NormModel(string path)
        {
            var p = path.Replace('\\', '/').ToLowerInvariant();
            return p.EndsWith(".mdl", StringComparison.Ordinal) ? p[..^4] : p;
        }

        var models = new SortedDictionary<string, string>(StringComparer.Ordinal);
        var sounds = new SortedDictionary<string, string>(StringComparer.Ordinal);
        var info = new SortedDictionary<string, Dictionary<string, JsonNode>>(StringComparer.Ordinal);
        var dollSounds = DollSounds(files);
        var meleeSwing = files.Any(f => f.Text.Contains("function SWEP:SwingSound()", StringComparison.Ordinal)
            && f.Text.Contains("npc/vort/claw_swing", StringComparison.Ordinal));
        foreach (var (path, text) in files.Where(f => IsLua(f.Path)).OrderBy(f => f.Path.ToLowerInvariant(), StringComparer.Ordinal))
        {
            var view = Find(SwepString, "ViewModel", text);
            if (!view.Success)
            {
                continue;
            }

            var key = NormModel(view.Groups[1].Value);
            var world = Find(SwepString, "WorldModel", text);
            if (world.Success)
            {
                models.TryAdd(key, NormModel(world.Groups[1].Value));
            }

            var shot = Find(SwepString, @"Primary\.Sound", text);
            if (shot.Success)
            {
                sounds.TryAdd(key, shot.Groups[1].Value.ToLowerInvariant());
            }

            var entry = new Dictionary<string, JsonNode>();
            foreach (var (field, output) in new[] { ("ElfShake", "dollShake"), ("ElfFidget", "dollFidget") })
            {
                var alias = Find(SwepString, field, text);
                if (alias.Success && dollSounds.TryGetPropertyValue(alias.Groups[1].Value, out var definition)
                    && definition is not null)
                {
                    entry[output] = definition.DeepClone();
                }
            }
            var swepBase = Find(SwepString, "Base", text);
            if (swepBase.Success)
            {
                entry["base"] = swepBase.Groups[1].Value;
                entry["gun"] = GunBases.Contains(swepBase.Groups[1].Value);
                if (swepBase.Groups[1].Value == "perpheads_melee_base") {
                    entry["melee"] = true;
                    if(meleeSwing && !text.Contains("function SWEP:SwingSound()", StringComparison.Ordinal))
                        entry["meleeSwing"] = new JsonObject {
                            ["files"] = new JsonArray("npc/vort/claw_swing1.wav", "npc/vort/claw_swing2.wav"),
                            ["level"] = 75, ["volume"] = 1,
                        };
                    foreach(var (field,output) in new[]{("Primary.Delay","meleePrimaryDelay"),("Secondary.Delay","meleeSecondaryDelay")}) {
                        var delay=Find(SwepNumber,Regex.Escape(field),text);
                        if(delay.Success && TryNumber(delay.Groups[1].Value,out var value)) entry[output]=Math.Clamp(value,0,5);
                    }
                }
            }

            var name = Find(SwepString, "PrintName", text);
            if (name.Success)
            {
                entry["name"] = name.Groups[1].Value;
            }

            foreach (var (field, outKey) in new[] { ("IronSightsPos", "ironPos"), ("IronSightsAng", "ironAng"),
                         ("RailSightsPos","railPos"), ("RailSightsAng","railAng") })
            {
                var vector = Find(SwepVector, field, text);
                if (vector.Success
                    && TryNumber(vector.Groups[1].Value, out var x)
                    && TryNumber(vector.Groups[2].Value, out var y)
                    && TryNumber(vector.Groups[3].Value, out var z))
                {
                    entry[outKey] = new JsonArray(x, y, z);
                }
            }

            var fov = Find(SwepNumber, "ViewModelFOV", text);
            if (fov.Success && TryNumber(fov.Groups[1].Value, out var fovValue))
            {
                entry["fov"] = fovValue;
            }

            if (Regex.IsMatch(text, @"SWEP\.ViewModelFlip\s*=\s*true"))
            {
                entry["flip"] = true;
            }

            if (entry.Count == 0)
            {
                continue;
            }

            if (!info.TryGetValue(key, out var known))
            {
                info[key] = entry;
                continue;
            }

            // Several SWEPs share this first-person model: keep only what they agree on,
            // so the viewer never names one weapon after another.
            foreach (var field in known.Keys.ToList())
            {
                if (!entry.TryGetValue(field, out var other) || !JsonNode.DeepEquals(known[field], other))
                {
                    known.Remove(field);
                }
            }

            if (!known.ContainsKey("base"))
            {
                foreach (var field in new[] { "gun", "ironPos", "ironAng", "fov" })
                {
                    known.Remove(field);
                }
            }
        }

        var modelsJson = new JsonObject();
        foreach (var (k, v) in models)
        {
            modelsJson[k] = v;
        }

        var soundsJson = new JsonObject();
        foreach (var (k, v) in sounds)
        {
            soundsJson[k] = v;
        }

        var infoJson = new JsonObject();
        foreach (var (k, fields) in info)
        {
            var o = new JsonObject();
            foreach (var field in fields.Keys.Order(StringComparer.Ordinal))
            {
                o[field] = fields[field];
            }

            infoJson[k] = o;
        }

        return new Weapons(modelsJson, soundsJson, infoJson);
    }

    private static JsonObject DollSounds(IReadOnlyList<(string Path, string Text)> files)
    {
        var source = files.FirstOrDefault(f => f.Path.EndsWith("/core/sh_sounds.lua", StringComparison.Ordinal));
        if (source.Text is null || Gluapack.IsStub(source.Text))
        {
            return new JsonObject();
        }

        var script = new Script(CoreModules.Preset_Complete);
        script.Globals["SERVER"] = true;
        script.Globals["CLIENT"] = false;
        script.DoString("""
            definitions = {}
            function AddCSLuaFile() end
            util = { AddNetworkString = function() end }
            sound = { Add = function(entry)
                if string.sub(entry.name or "", 1, 3) == "Elf" then
                    local paths = entry.sound
                    if type(paths) == "string" then paths = { paths } end
                    definitions[entry.name] = { files = paths, level = entry.level or 75,
                                                volume = entry.volume or 1 }
                end
            end }
            """, codeFriendlyName: "doll sound export prelude");
        script.DoString(Glua(source.Text), codeFriendlyName: source.Path);
        var definitions = script.Globals.Get("definitions");
        return definitions.Table.Pairs.Any() ? (JsonObject)Convert(definitions)! : new JsonObject();
    }

    private static bool TryNumber(string text, out double value) =>
        double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out value);

    // ---- clothing (export_clothing.py) --------------------------------------------------

    private const string ClothingPrelude = """
        SEX_MALE = 1
        SEX_FEMALE = 0
        function Color(r, g, b, a) return {r = r, g = g, b = b, a = a or 255} end
        function Either(c, a, b) if c then return a else return b end end
        function AddCSLuaFile() end
        function T(s) return s end
        local plain_insert = table.insert
        function table.insert(t, a, b)
          if b == nil then t[#t + 1] = a; return #t end
          plain_insert(t, a, b); return a
        end
        function table.Count(t) local n = 0; for _ in pairs(t) do n = n + 1 end; return n end
        function table.Add(a, b) for _, v in ipairs(b) do a[#a + 1] = v end; return a end
        function table.HasValue(t, v) for _, x in pairs(t) do if x == v then return true end end return false end
        function include(name) return assert(load(__perp_read(name), "=" .. name))() end
        Clothing = {}
        """;

    private const string ClothingExport = """
        local result = {types = {}, bodyParts = {}, requiredSlots = {}}
        local function copy_list(t) local r = {} for _, v in ipairs(t or {}) do r[#r + 1] = v end return r end
        for index, t in pairs(Clothing.ClothingTypesByID) do
          local skins = {}
          for i, skin in ipairs(t.Skins or {}) do
            local subs = {}
            for k, v in pairs(skin.SubMaterials or {}) do subs[#subs + 1] = {k, v} end
            skins[i] = {name = skin.Name, canTint = skin.CanTint, subMaterials = subs}
          end
          local variants = {}
          if t.GetModel then
            for _, feet in ipairs({false, true}) do
              for _, thighs in ipairs({false, true}) do
                local missing = {}
                missing[Clothing.BODY_SLOT_FEET] = feet or nil
                missing[Clothing.BODY_SLOT_THIGHS] = thighs or nil
                variants[#variants + 1] = {feet = feet, thighs = thighs, model = t.GetModel(missing)}
              end
            end
          end
          result.types[#result.types + 1] = {
            index = index, identifier = t.Identifier or t.Name, model = t.Model, slots = copy_list(t.Slots),
            category = t.Category, sex = t.Sex, government = t.Government or false,
            accessory = t.IsAccessory or false, bodyPart = t.IsBodyPart or false, skins = skins,
            modelVariants = variants, excludedBodyParts = copy_list(t.ExcludedBodyParts),
            optionalBodyPartSlots = copy_list(t.OptionalBodyPartSlots),
            attachBodyPartsForSlots = t.AttchBodyPartsForSlots or false,
            dataNumberBodyGroup = t.DataNumberBodyGroup, dataNumberMaterialBase = t.DataNumberMaterialBase,
            name = t.Name,
          }
        end
        for slot, _ in pairs(Clothing.RequiredSlots) do result.requiredSlots[#result.requiredSlots + 1] = slot end
        return result
        """;

    /// <summary>Rewrites the GLua-only operators so stock Lua can load the file.</summary>
    private static string Glua(string text)
    {
        text = text.Replace("!=", "~=", StringComparison.Ordinal);
        text = text.Replace("&&", " and ", StringComparison.Ordinal);
        text = text.Replace("||", " or ", StringComparison.Ordinal);
        text = Regex.Replace(text, @"(^|\s)//", "$1--", RegexOptions.Multiline);
        text = Regex.Replace(text, "!(?!=)", " not ");
        text = Regex.Replace(text, @"\bcontinue\b", "break");
        return text;
    }

    /// <summary>
    /// The gun and impact sounds (sh_gun_sounds.lua) in registration order: the index the server
    /// sends in ph_bullet_sound / ph_bullet_impact_sound. Empty for servers without that module.
    /// </summary>
    public static JsonObject GunSounds(IReadOnlyList<(string Path, string Text)> files)
    {
        var result = new JsonObject { ["sounds"] = new JsonArray() };
        var source = files.FirstOrDefault(f => f.Text.Contains("function GunSounds:RegisterSound", StringComparison.Ordinal)
            && !Gluapack.IsStub(f.Text));
        if (source.Text is null)
        {
            return result;
        }

        var script = new Script(CoreModules.Preset_Complete);
        script.Globals["CLIENT"] = false;
        script.Globals["SERVER"] = true;
        script.DoString(Glua(source.Text), codeFriendlyName: source.Path);
        var export = script.DoString("""
            local out = { sounds = {}, maxBulletDistance = GunSounds.MAX_BULLET_SOUND_DISTANCE,
                          maxImpactDistance = GunSounds.MAX_IMPACT_SOUND_DISTANCE }
            for index, entry in ipairs(GunSounds.RegisteredSoundsByIndex) do
                local files = {}
                for _, file in ipairs(entry.Sounds) do files[#files + 1] = file end
                out.sounds[#out.sounds + 1] = { index = index, name = entry.Name, files = files,
                                                level = entry.Level, volume = entry.Volume }
            end
            return out
            """, codeFriendlyName: "gun sounds export");
        return (JsonObject)Convert(export)!;
    }

    public static JsonObject Clothing(IReadOnlyList<(string Path, string Text)> files, out int typeCount)
    {
        var byPath = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var (path, text) in files)
        {
            byPath.TryAdd(path, text);
        }

        string Read(string name)
        {
            if (!byPath.TryGetValue(ClothingFolder + name, out var text))
            {
                throw new InvalidDataException($"{ClothingFolder}{name} is not in the demo's Lua.");
            }

            if (Gluapack.IsStub(text))
            {
                throw new InvalidDataException($"{ClothingFolder}{name} is still packed (gluapack not loaded).");
            }

            return Glua(text);
        }

        var script = new Script(CoreModules.Preset_Complete);
        script.Globals["__perp_read"] = (Func<string, string>)Read;
        script.DoString(ClothingPrelude, codeFriendlyName: "prelude");
        var clothingText = Read("sh_clothing.lua");
        var slotsText = clothingText.Split("IncludeSH(\"sh_bodyparts.lua\")")[0];
        script.DoString(slotsText, codeFriendlyName: "sh_clothing.lua");
        script.DoString(Read("sh_bodyparts.lua"), codeFriendlyName: "sh_bodyparts.lua");
        if (script.Globals.Get("Clothing").Table.Get("RequiredSlots").IsNil())
        {
            var legacySlots = Regex.Match(clothingText, @"local\s+slotsLeftToFill\s*=\s*(\{[^}]*\})");
            if (!legacySlots.Success)
                throw new InvalidDataException("Clothing required slots are missing from the server Lua.");
            script.DoString("Clothing.RequiredSlots = " + legacySlots.Groups[1].Value,
                codeFriendlyName: "legacy clothing required slots");
        }
        script.DoString(Read("sh_clothingtypes.lua"), codeFriendlyName: "sh_clothingtypes.lua");
        var export = script.DoString(ClothingExport, codeFriendlyName: "export");

        var data = (JsonObject)Convert(export)!;
        var types = ((data["types"] as JsonArray) ?? []).Select(t => t!.DeepClone())
            .OrderBy(t => ToDouble(t["index"])).ToList();

        // pairs() order is not data: give the material remaps and required slots a stable order.
        foreach (var type in types)
        {
            if (type!["skins"] is not JsonArray skins)
            {
                continue;
            }

            foreach (var skin in skins)
            {
                if (skin?["subMaterials"] is JsonArray subs)
                {
                    var sorted = subs.Select(s => s!.DeepClone())
                        .OrderBy(s => s is JsonArray pair ? pair[0]?.ToJsonString() : "", StringComparer.Ordinal).ToList();
                    skin["subMaterials"] = new JsonArray([.. sorted]);
                }
            }
        }

        data["types"] = new JsonArray([.. types]);
        if (data["requiredSlots"] is JsonArray required)
        {
            var slots = required.Select(ToDouble).Order().ToList();
            data["requiredSlots"] = new JsonArray([.. slots.Select(s => (JsonNode)Number(s))]);
        }

        typeCount = types.Count;
        return data;
    }

    private static JsonNode? Convert(DynValue value)
    {
        switch (value.Type)
        {
            case DataType.Table:
            {
                var table = value.Table;
                var pairs = table.Pairs.Where(p => !p.Value.IsNil()).ToList();
                var isList = pairs.Count > 0
                    && pairs.All(p => p.Key.Type == DataType.Number && p.Key.Number == Math.Floor(p.Key.Number))
                    && pairs.Select(p => p.Key.Number).Order().SequenceEqual(Enumerable.Range(1, pairs.Count).Select(i => (double)i));
                if (isList)
                {
                    return new JsonArray([.. pairs.OrderBy(p => p.Key.Number).Select(p => Convert(p.Value))]);
                }

                var obj = new JsonObject();
                foreach (var pair in pairs)
                {
                    var key = pair.Key.Type switch
                    {
                        DataType.Number => Number(pair.Key.Number).ToJsonString(),
                        DataType.Boolean => pair.Key.Boolean ? "True" : "False",
                        _ => pair.Key.CastToString() ?? "",
                    };
                    obj[key] = Convert(pair.Value);
                }

                return obj;
            }

            case DataType.Number:
                return Number(value.Number);
            case DataType.String:
                return value.String;
            case DataType.Boolean:
                return value.Boolean;
            default:
                return null;
        }
    }

    private static double ToDouble(JsonNode? node) =>
        node is null ? 0 : double.Parse(node.ToJsonString(), CultureInfo.InvariantCulture);

    private static JsonValue Number(double value) =>
        value == Math.Floor(value) && Math.Abs(value) < 1e15 ? JsonValue.Create((long)value) : JsonValue.Create(value);

    // ---- output ---------------------------------------------------------------------------

    public static string ToJson(JsonNode node) => node.ToJsonString(JsonOptions);
}

internal static partial class Program
{
    private static int RunPerpData(string[] args)
    {
        if (args.Length == 0)
        {
            return Fail("perp-data requires a demo path.");
        }

        var demo = args[0];
        string? outDir = null;
        string? pack = null;
        string? dumpLua = null;
        for (var i = 1; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "-o" or "--out":
                    if (++i >= args.Length)
                    {
                        return Fail("--out requires a directory.");
                    }

                    outDir = args[i];
                    break;
                case "--gluapack":
                    if (++i >= args.Length)
                    {
                        return Fail("--gluapack requires a .bsp path.");
                    }

                    pack = args[i];
                    break;
                case "--dump-lua":
                    if (++i >= args.Length)
                    {
                        return Fail("--dump-lua requires a directory.");
                    }

                    dumpLua = args[i];
                    break;
                default:
                    return Fail($"Unknown option '{args[i]}'.");
            }
        }

        if (outDir is null)
        {
            return Fail("perp-data requires -o <directory>.");
        }

        var tree = PerpData.LoadLua(demo, pack);
        Console.WriteLine($"lua files {tree.Files.Count}, still packed {tree.Stubs}, gluapack {tree.PackPath ?? "none"}");
        if (dumpLua is not null)
        {
            foreach (var (path, text) in tree.Files)
            {
                var full = LuaExporter.SafePath(dumpLua, path);
                Directory.CreateDirectory(Path.GetDirectoryName(full)!);
                File.WriteAllText(full, text, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
            }
        }
        if (tree.Pack.Detected && !tree.Pack.HasPack)
        {
            Console.Error.WriteLine(
                $"error: the demo's Lua is packed and its gluapack ({tree.Pack.FileName ?? "?"}.bsp) was not found. "
                + "Join the server once so GMod downloads it, or pass --gluapack.");
            return 4;
        }

        var outputs = PerpData.Build(tree, demo, out var counts);
        if (outputs is null)
        {
            Console.Error.WriteLine($"error: incomplete data ({counts}).");
            return 5;
        }

        PerpData.WriteAll(outDir, outputs);
        Console.WriteLine(counts);
        return 0;
    }
}

internal static partial class PerpData
{
    // Bumped whenever the exported data changes shape, so cached packs are rebuilt.
    public const int FormatVersion = 12;

    private static readonly string[] DataFiles =
        ["perpheads_jobs.json", "swep_models.json", "swep_sounds.json", "swep_info.json", "clothing_types.json",
         "gun_sounds.json", "weapon_attachments.json", "vehicle_plates.json"];

    /// <summary>Builds every data file in memory; null when the Lua did not give complete data.</summary>
    public static (string Name, string Text)[]? Build(LuaTree tree, string demo, out string counts)
    {
        var jobs = Jobs(tree.Files, out var teamCount);
        var weapons = Sweps(tree.Files);
        var clothing = Clothing(tree.Files, out var typeCount);
        var gunSounds = GunSounds(tree.Files);
        var attachments = Attachments(tree.Files);
        var plates = VehiclePlates(tree.Files);
        counts = $"teams {teamCount}, weapons {weapons.Info.Count}, clothing types {typeCount}, "
            + $"gun sounds {(gunSounds["sounds"] as JsonArray)?.Count ?? 0}, "
            + $"attachments {(attachments["attachments"] as JsonArray)?.Count ?? 0}, "
            + $"vehicle plate layouts {(plates["vehicles"] as JsonArray)?.Count ?? 0}";
        if (teamCount == 0 || typeCount == 0 || weapons.Info.Count == 0)
        {
            return null;
        }

        return
        [
            ("perpheads_jobs.json", ToJson(jobs)),
            ("swep_models.json", ToJson(weapons.Models)),
            ("swep_sounds.json", ToJson(weapons.Sounds)),
            ("swep_info.json", ToJson(weapons.Info)),
            ("clothing_types.json", ToJson(clothing)),
            ("gun_sounds.json", ToJson(gunSounds)),
            ("weapon_attachments.json", ToJson(attachments)),
            ("vehicle_plates.json", ToJson(plates)),
            ("source.txt", string.Join('\n',
                "demo=" + Path.GetFullPath(demo),
                "gluapack=" + (tree.PackPath ?? ""),
                "format=" + FormatVersion,
                "generated=" + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture)) + "\n"),
        ];
    }

    /// <summary>Writes the files; each replaces the old one only once it is fully written.</summary>
    public static void WriteAll(string outDir, (string Name, string Text)[] outputs)
    {
        Directory.CreateDirectory(outDir);
        var utf8 = new UTF8Encoding(encoderShouldEmitUTF8Identifier: false);
        foreach (var (name, text) in outputs)
        {
            var target = Path.Combine(outDir, name);
            var temp = target + "." + Environment.ProcessId + ".tmp";
            File.WriteAllText(temp, text, utf8);
            File.Move(temp, target, overwrite: true);
        }
    }

    private static bool IsComplete(string dir)
    {
        var source = Path.Combine(dir, "source.txt");
        return DataFiles.All(name => File.Exists(Path.Combine(dir, name)))
            && File.Exists(source)
            && File.ReadAllLines(source).Contains("format=" + FormatVersion);
    }

    /// <summary>
    /// Returns the data folder for the server Lua version (gluapack) the demo was recorded with,
    /// building it under <paramref name="cacheRoot"/> the first time that version is seen.
    /// Null when the demo's version cannot be identified or its gluapack is not on disk.
    /// </summary>
    public static string? EnsureCached(EventCollector events, string demoPath, string cacheRoot, out string note)
    {
        var pack = events.Gluapack;
        var key = pack.FileName is { Length: > 0 } name ? Path.GetFileNameWithoutExtension(name).ToLowerInvariant() : null;
        if (key is null || key.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0)
        {
            note = "no gluapack id in the demo";
            return null;
        }

        var dir = Path.Combine(cacheRoot, key);
        if (IsComplete(dir))
        {
            note = "cached " + key;
            return dir;
        }

        var tree = BuildTree(events, demoPath, forcedPack: null);
        if (tree.Pack.Detected && !tree.Pack.HasPack)
        {
            note = $"gluapack {key}.bsp is not downloaded";
            return null;
        }

        var outputs = Build(tree, demoPath, out var counts);
        if (outputs is null)
        {
            note = "incomplete data (" + counts + ")";
            return null;
        }

        // Built in a private folder and moved into place, so a second viewer loading the same
        // demo never reads half-written data.
        var temp = dir + "." + Environment.ProcessId + ".tmp";
        WriteAll(temp, outputs);
        try
        {
            Directory.Move(temp, dir);
        }
        catch (IOException) when (IsComplete(dir))
        {
            Directory.Delete(temp, recursive: true);
        }
        catch (IOException)
        {
            if (Directory.Exists(dir))
            {
                Directory.Delete(dir, recursive: true);
            }

            Directory.Move(temp, dir);
        }

        note = "built " + key + " (" + counts + ")";
        return dir;
    }
}
