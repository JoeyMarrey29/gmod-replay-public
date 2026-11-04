namespace GmodDemo.Parser;

public sealed partial class PerpheadsMessageDecoder
{
    private sealed record LegacyPart(string Model, bool Visible, int Skin, byte[] Color, int[] Vars);
    private readonly Dictionary<int, Dictionary<int, LegacyPart>> _legacyClothing = [];
    private string[] _legacyClothingVariables = [];

    private void ResolveLegacyClothingLayout(string? lua)
    {
        var initialize = System.Text.RegularExpressions.Regex.Match(lua ?? "",
            @"function\s+\w+:Initialize\s*\(\s*\)(?<body>[\s\S]*?)\bend\b");
        _legacyClothingVariables = System.Text.RegularExpressions.Regex.Matches(
            initialize.Groups["body"].Value, """self:NetworkVar\s*\(\s*["']([^"']+)["']\s*\)""")
            .Select(m => m.Groups[1].Value.ToLowerInvariant()).ToArray();
        if (_legacyClothingVariables.Length == 0
            || !_legacyClothingVariables.Contains("clothing_type")
            || _legacyClothingVariables.Distinct().Count() != _legacyClothingVariables.Length)
            throw new InvalidDataException("Legacy clothing variable layout is missing or unsupported in the demo's server sh_clothing_ent.lua.");
    }

    private int LegacyVariable(LegacyPart part, string name, int fallback = 0)
    {
        var index = Array.IndexOf(_legacyClothingVariables, name);
        return index < 0 ? fallback : part.Vars[index];
    }

    private void ReadLegacyClothing(int tick, string name, byte[] body)
    {
        if (name is not ("perpheads_bonemerge" or "perpheads_bonemerge_entry_visible"
            or "perpheads_bonemerge_entry_skin" or "perpheads_bonemerge_entry_color"
            or "perpheads_bonemerge_entry_nwvar" or "perpheads_bonemerge_entry_removed"
            or "perpheads_bonemerge_entity_removed" or "perpheads_bonemerge_remove_merged")) return;
        var bits = new Bits(body);
        var entity = (int)bits.UInt(16);
        var entry = name is "perpheads_bonemerge_entity_removed" or "perpheads_bonemerge_remove_merged"
            ? -1 : (int)bits.UInt(16);
        if (!_legacyClothing.TryGetValue(entity, out var parts))
        {
            if (name != "perpheads_bonemerge") return;
            parts = [];
        }
        if (name == "perpheads_bonemerge")
        {
            var className = ReadStringTable(bits);
            var model = ReadStringTable(bits);
            if (className != "clothing_ent") return;
            var visible = bits.Bool();
            var color = bits.Bool() ? LegacyColor(bits) : new byte[] { 255,255,255,255 };
            var skin = bits.Bool() ? (int)bits.UInt(8) : 0;
            if (bits.Bool()) bits.UInt(8);
            if (bits.Bool()) bits.Float();
            var vars = new int[_legacyClothingVariables.Length];
            for (var i = 0; i < vars.Length; i++) vars[i] = LegacyNumber(bits);
            parts[entry] = new(model, visible, skin, color, vars);
        }
        else if (entry < 0)
        {
            if (parts.Count == 0) return;
            parts.Clear();
        }
        else if (name == "perpheads_bonemerge_entry_removed")
        {
            if (!parts.Remove(entry)) return;
        }
        else
        {
            if (!parts.TryGetValue(entry, out var part)) return;
            switch (name)
            {
                case "perpheads_bonemerge_entry_visible": part = part with { Visible = bits.Bool() }; break;
                case "perpheads_bonemerge_entry_skin": part = part with { Skin = (int)bits.UInt(8) }; break;
                case "perpheads_bonemerge_entry_color": part = part with { Color = LegacyColor(bits) }; break;
                case "perpheads_bonemerge_entry_nwvar":
                    var index = (int)bits.UInt(8) - 1;
                    var value = LegacyNumber(bits);
                    if (index < 0 || index >= part.Vars.Length)
                        throw new InvalidDataException($"Invalid legacy clothing variable {index + 1} at tick {tick}.");
                    var vars = (int[])part.Vars.Clone();
                    vars[index] = value;
                    part = part with { Vars = vars };
                    break;
                default: return;
            }
            parts[entry] = part;
        }
        _legacyClothing[entity] = parts;
        var items = parts.OrderBy(p => p.Key).Where(p => p.Value.Visible).Select(p =>
        {
            var part = p.Value;
            var skin = LegacyVariable(part, "clothing_skin", 1);
            return new ClothingItem(LegacyVariable(part, "clothing_type"),
                LegacyVariable(part, "clothing_data_number"), null, null,
                skin == 0 ? 1 : skin, part.Color, 0, null, 0, null, 0, null,
                part.Model, part.Skin);
        }).ToList();
        _clothingEvents.Add(new(tick, "store", 1_000_000_000L + _clothingEvents.Count,
            new ClothingRecord(1, null, 0, 0, 0, 0, 0, items, true), [entity], -1));
    }

    private static byte[] LegacyColor(Bits bits) =>
        [(byte)bits.UInt(8), (byte)bits.UInt(8), (byte)bits.UInt(8), (byte)bits.UInt(8)];

    private static int LegacyNumber(Bits bits)
    {
        var type = bits.UInt(8);
        if (type == 0) return 0;
        if (type != 3) throw new InvalidDataException($"Unsupported legacy clothing value type {type}.");
        var raw = (ulong)bits.UInt(32) | ((ulong)bits.UInt(32) << 32);
        var value = BitConverter.Int64BitsToDouble(unchecked((long)raw));
        if (!double.IsFinite(value) || value < int.MinValue || value > int.MaxValue || value != Math.Truncate(value))
            throw new InvalidDataException("Invalid legacy clothing numeric value.");
        return (int)value;
    }
}
