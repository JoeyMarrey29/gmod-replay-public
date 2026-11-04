using System.Text.Json;

namespace GmodDemo.Parser;

/// <summary>Client Lua fires have no visible model. Retain their networked lifetime and size.</summary>
public sealed class FireTimeline
{
    public sealed record Key(int Tick, int Entity, int Serial, int? ClassIndex,
        string? Name, Vec3 Origin, float Scale, bool Active);
    private readonly List<Key> _keys = [];
    private readonly Dictionary<int, Key> _live = [];
    public IReadOnlyList<Key> Keys => _keys;
    public Func<int?, string?> ResolveClass { get; set; } = _ => null;

    public void Observe(int tick, EntitySnapshot snapshot)
    {
        if ((ResolveClass(snapshot.RealClassIndex) ?? snapshot.Name) != "ent_fire") return;
        if (snapshot.Origin is not { } origin) return;
        if (!float.IsFinite(origin.X) || !float.IsFinite(origin.Y) || !float.IsFinite(origin.Z)) return;
        var scale = snapshot.ModelScale ?? 1f;
        if (!float.IsFinite(scale)) scale = 1f;
        var key = new Key(tick, snapshot.Index, snapshot.Serial, snapshot.RealClassIndex,
            snapshot.Name, origin, Math.Clamp(scale, 0f, 8f), true);
        if (_live.TryGetValue(key.Entity, out var previous) && previous == key with { Tick = previous.Tick }) return;
        if (previous is not null && previous.Serial != key.Serial) Remove(tick, key.Entity);
        _live[key.Entity] = key;
        _keys.Add(key);
    }

    public void Remove(int tick, int entity)
    {
        if (_live.Remove(entity, out var previous)) _keys.Add(previous with { Tick = tick, Active = false });
    }

    public void Write(Utf8JsonWriter writer, Func<int?, string?> resolveClass, double tickRate)
    {
        writer.WriteStartArray("fires");
        foreach (var key in _keys)
        {
            if ((resolveClass(key.ClassIndex) ?? key.Name) != "ent_fire") continue;
            writer.WriteStartObject();
            writer.WriteNumber("time", Math.Max(0, key.Tick) / tickRate);
            writer.WriteNumber("entity", key.Entity);
            writer.WriteNumber("serial", key.Serial);
            writer.WriteBoolean("active", key.Active);
            writer.WriteNumber("scale", key.Scale);
            writer.WriteStartArray("origin");
            writer.WriteNumberValue(key.Origin.X);
            writer.WriteNumberValue(key.Origin.Y);
            writer.WriteNumberValue(key.Origin.Z);
            writer.WriteEndArray();
            writer.WriteEndObject();
        }
        writer.WriteEndArray();
    }
}
