using System.Text.Json;

namespace GmodDemo.Parser;

public sealed record TempEntityRow(int Tick, float Delay, string ClassName,
    IReadOnlyDictionary<string, PropValue> Properties);

/// <summary>Source temporary entities use the demo's own server classes and send properties.</summary>
public sealed class TempEntityDecoder
{
    public void WritePlayerAnimations(Utf8JsonWriter writer, double tickRate)
    {
        writer.WriteStartArray("playerAnimations");
        foreach (var row in _rows.Where(row => row.ClassName == "CTEPlayerAnimEvent"))
        {
            var handle = row.Properties.GetValueOrDefault("m_hPlayer") is PropValue.Int player
                ? (int)player.Value : -1;
            if (handle < 0 || handle == 8388607) continue;
            var animation = row.Properties.GetValueOrDefault("m_iEvent") is PropValue.Int value
                ? (int)value.Value : 0;
            writer.WriteStartObject();
            writer.WriteNumber("time", row.Tick / tickRate + row.Delay);
            writer.WriteNumber("entity", handle & 8191);
            writer.WriteNumber("event", animation);
            writer.WriteNumber("data", row.Properties.GetValueOrDefault("m_nData") is PropValue.Int data ? data.Value : 0);
            writer.WriteEndObject();
        }
        writer.WriteEndArray();
    }

    private readonly List<TempEntityRow> _rows = [];
    public IReadOnlyList<TempEntityRow> Rows => _rows;
    public int FailedMessages { get; private set; }

    public void Read(ref BitReader reader, int tick, DataTableSet tables)
    {
        var count = (int)reader.ReadUBitLong(8);
        if (count == 0) count = 1; // Source's reliable single-event form.
        var length = (int)reader.ReadVarInt32();
        if (length < 0 || length > reader.BitsLeft)
        {
            FailedMessages++;
            return;
        }
        var data = reader.ReadBitsAsBytes(length);
        var bits = new BitReader(data);
        var classBits = BitMath.Log2(Math.Max(1, tables.Classes.Count)) + 1;
        ServerClass? serverClass = null;
        var values = new Dictionary<string, PropValue>(StringComparer.Ordinal);
        var pending = new List<TempEntityRow>();
        for (var i = 0; i < count; i++)
        {
            var delay = bits.ReadOneBit() != 0 ? bits.ReadSBitLong(8) / 100f : 0f;
            if (bits.ReadOneBit() != 0)
            {
                var id = (int)bits.ReadUBitLong(classBits) - 1;
                serverClass = tables.ClassesById.GetValueOrDefault(id);
                values = new(StringComparer.Ordinal);
            }
            if (serverClass is null || !serverClass.Name.StartsWith("CTE", StringComparison.Ordinal))
            {
                FailedMessages++;
                return;
            }
            var last = -1;
            while (bits.ReadOneBit() != 0 && !bits.IsOverflowed)
            {
                var index = last + 1 + (int)bits.ReadUBitVar();
                if ((uint)index >= (uint)serverClass.Flattened.Count) { FailedMessages++; return; }
                var prop = serverClass.Flattened[index];
                var value = PropDecoder.Decode(ref bits, prop);
                if (value is null) { FailedMessages++; return; }
                values[prop.Name] = value;
                last = index;
            }
            if (bits.IsOverflowed || bits.BitsRead > length) { FailedMessages++; return; }
            pending.Add(new(tick, delay, serverClass.Name, new Dictionary<string, PropValue>(values)));
        }
        _rows.AddRange(pending);
    }

    public void Write(Utf8JsonWriter writer, PrecacheTables precache, double tickRate,
        IReadOnlyList<PerpheadsMessageDecoder.BloodImpact>? bloodImpacts = null,
        IReadOnlyList<PerpheadsMessageDecoder.SurfaceImpact>? surfaceImpacts = null)
    {
        writer.WriteStartArray("effects");
        foreach (var row in _rows)
        {
            var p = row.Properties;
            int Int(string key) => p.GetValueOrDefault(key) is PropValue.Int v ? (int)v.Value : 0;
            float Float(string key) => p.GetValueOrDefault(key) is PropValue.Float v ? v.Value : 0;
            Vec3 Vector(string key) => p.GetValueOrDefault(key) is PropValue.Vector v
                ? v.Value : new(Float(key + "[0]"), Float(key + "[1]"), Float(key + "[2]"));
            var decal = row.ClassName is "CTEDecal" or "CTEWorldDecal" or "CTEProjectedDecal";
            var dispatch = row.ClassName == "CTEEffectDispatch";
            var blood = row.ClassName is "CTEBloodSprite" or "CTEBloodStream";
            if (!decal && !dispatch && !blood) continue;
            var name = decal ? precache.Decals.GetValueOrDefault(Int("m_nIndex"))
                : dispatch ? precache.Effects.GetValueOrDefault(Int("m_iEffectName")) : "bloodimpact";
            if (name is null) continue;
            if (dispatch && PerpheadsMessageDecoder.IsFleshImpact(name, Float("m_flRadius")))
                name = "bloodimpact";
            var origin = Vector(dispatch ? "m_vOrigin" : "m_vecOrigin");
            var start = Vector(dispatch ? "m_vStart" : "m_vecStart");
            var normal = Vector(dispatch ? "m_vNormal" : "m_vecDirection");
            void WriteVector(string key, Vec3 value)
            {
                writer.WriteStartArray(key);
                writer.WriteNumberValue(value.X);
                writer.WriteNumberValue(value.Y);
                writer.WriteNumberValue(value.Z);
                writer.WriteEndArray();
            }
            writer.WriteStartObject();
            writer.WriteNumber("time", row.Tick / tickRate + row.Delay);
            writer.WriteString("kind", decal ? "decal" : "dispatch");
            writer.WriteString("name", name);
            WriteVector("origin", origin);
            WriteVector("start", start);
            WriteVector("normal", normal);
            writer.WriteNumber("entity", Int(dispatch ? "entindex" : "m_nEntity"));
            writer.WriteNumber("scale", Float("m_flScale"));
            writer.WriteNumber("material", Float("m_flRadius"));
            writer.WriteEndObject();
        }
        if (bloodImpacts is not null)
        {
            foreach (var impact in bloodImpacts)
            {
                writer.WriteStartObject();
                writer.WriteNumber("time", impact.Tick / tickRate);
                writer.WriteString("kind", "dispatch");
                writer.WriteString("name", "bloodimpact");
                writer.WriteStartArray("origin");
                foreach (var coordinate in impact.Position) writer.WriteNumberValue(coordinate);
                writer.WriteEndArray();
                if (impact.Normal is not null)
                {
                    writer.WriteStartArray("normal");
                    foreach (var coordinate in impact.Normal) writer.WriteNumberValue(coordinate);
                    writer.WriteEndArray();
                }
                writer.WriteNumber("scale", impact.Scale);
                writer.WriteEndObject();
            }
            Console.WriteLine($"BLOOD_IMPACTS recorded={bloodImpacts.Count}");
        }
        if (surfaceImpacts is not null)
        {
            foreach (var impact in surfaceImpacts)
            {
                writer.WriteStartObject();
                writer.WriteNumber("time", impact.Tick / tickRate);
                writer.WriteString("kind", "dispatch");
                writer.WriteString("name", impact.Name);
                writer.WriteStartArray("origin");
                foreach (var coordinate in impact.Position) writer.WriteNumberValue(coordinate);
                writer.WriteEndArray();
                writer.WriteStartArray("normal");
                foreach (var coordinate in impact.Normal) writer.WriteNumberValue(coordinate);
                writer.WriteEndArray();
                writer.WriteNumber("scale", impact.Scale);
                writer.WriteNumber("material", impact.Material);
                writer.WriteEndObject();
            }
            Console.WriteLine($"SURFACE_IMPACTS recorded={surfaceImpacts.Count}");
        }
        writer.WriteEndArray();
        if (FailedMessages > 0)
            Console.Error.WriteLine($"TEMP_ENTITIES_FAILED: {FailedMessages} messages discarded atomically.");
    }
}
