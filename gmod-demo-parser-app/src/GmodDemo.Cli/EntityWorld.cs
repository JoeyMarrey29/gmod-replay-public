namespace GmodDemo.Parser;

public readonly record struct PacketEntitiesHeader(
    int MaxEntries,
    bool IsDelta,
    int DeltaFrom,
    bool Baseline,
    int UpdatedEntries,
    int LengthBits,
    bool UpdateBaseline,
    int DataStartBit);

/// <summary>
/// How the per-entity field-index stream is packed. Source 2013 uses a continuation bit plus
/// <c>ReadUBitVar</c>. CS:GO later added a leading "new way" bit and a different end marker.
/// Which encoding Garry's Mod uses is decided empirically against a real demo, not assumed.
/// </summary>
public enum PropIndexMode
{
    Unknown = 0,
    Source2013 = 1,
    CsgoNewWay = 2,
    CsgoOldWay = 3,
}

public sealed record EntitySnapshot(
    int Index,
    int ClassId,
    int Serial,
    string ClassName,
    bool IsPlayer,
    Vec3? Origin,
    Vec3? Angles,
    Vec3? EyeAngles,
    Vec3? ViewOffset,
    int? Sequence,
    float? Cycle,
    float? PlaybackRate,
    int? Health,
    int? LifeState,
    int? Flags,
    int? Team,
    int? ModelIndex,
    int? Skin,
    int? Body,
    int? UserId,
    string? Name,
    int? OwnerEntity,
    int? ActiveWeapon,
    int? Vehicle,
    int? Driver,
    int? DoorState,
    int? Parent,
    int? CreationId,
    int? MapCreatedId,
    float? CreationTime,
    float? ModelScale,
    int? RealClassIndex,
    string? GmodString0,
    string? GmodString1,
    string? GmodString2,
    string? GmodString3);

public abstract record EntityChange(int Tick, int Entity);

public sealed record EntitySpawnChange(
    int Tick,
    int Entity,
    string ClassName,
    bool IsPlayer,
    int ClassId,
    int Serial,
    string? Name,
    int? OwnerEntity,
    bool IsWeapon,
    bool IsDoor,
    bool IsVehicle)
    : EntityChange(Tick, Entity);

public sealed record EntityRemoveChange(int Tick, int Entity, bool Deleted)
    : EntityChange(Tick, Entity);

public sealed record EntityVisibilityChange(int Tick, int Entity, bool Hidden)
    : EntityChange(Tick, Entity);

public sealed record TeamEntry(int Tick, int Entity, int Team);

public sealed record WeaponLogEntry(int Tick, string Kind, int Entity, string? Value, int Other, int Serial = -1);

/// <summary>One networked ragdoll pose: world position and angles of every physics object.</summary>
public sealed record RagdollEntry(int Tick, int Entity, int Serial, int Model, Vec3[] Pos, Vec3[] Ang);

/// <summary>Full state of a first-person view model after one of its properties changed.</summary>
public sealed record ViewModelEntry(
    int Tick, int Entity, int VmIndex, int Owner, int Model, int Sequence, float Rate,
    int NewSequenceParity, int AnimationParity, int Weapon, int Effects);

/// <summary>Networked pose parameters of a vehicle (steering, wheel spin and height), normalized 0..1.</summary>
public sealed record PoseEntry(int Tick, int Entity, int Serial, int[] Index, float[] Value);

public sealed record EntityOriginChange(int Tick, int Entity, Vec3 Origin)
    : EntityChange(Tick, Entity);

public sealed record EntityAnglesChange(int Tick, int Entity, Vec3 Angles)
    : EntityChange(Tick, Entity);

public sealed record EntityEyeChange(int Tick, int Entity, Vec3 EyeAngles)
    : EntityChange(Tick, Entity);

public sealed record EntityAnimChange(int Tick, int Entity, int? Sequence, float? Cycle, float? PlaybackRate)
    : EntityChange(Tick, Entity);

public sealed record EntityStateChange(
    int Tick,
    int Entity,
    int? Health,
    int? LifeState,
    int? Flags,
    int? Team,
    int? ModelIndex,
    int? Skin,
    int? Body)
    : EntityChange(Tick, Entity);

public sealed record EntityOwnerChange(int Tick, int Entity, int? Owner, string? Name)
    : EntityChange(Tick, Entity);

public sealed record EntityVehicleChange(int Tick, int Entity, int? Vehicle, int? Parent)
    : EntityChange(Tick, Entity);

public sealed record EntityWeaponChange(int Tick, int Entity, int? Weapon, string? Name)
    : EntityChange(Tick, Entity);

public sealed record EntityDoorChange(int Tick, int Entity, Vec3? Origin, int? DoorState, bool Hidden = false)
    : EntityChange(Tick, Entity);

public sealed class EntityDecodeStats
{
    public PropIndexMode Mode { get; set; }

    public int Packets { get; set; }

    public int PacketsClean { get; set; }

    public int PacketsFailed { get; set; }

    public int EntitiesEntered { get; set; }

    public int EntitiesLeft { get; set; }

    public int OriginUpdates { get; set; }

    public int CameraMatches { get; set; }

    public bool OriginsTrusted { get; set; }
}

/// <summary>
/// Reconstructs entity state from <c>svc_PacketEntities</c> using the send tables in
/// <c>dem_datatables</c>. Enter-PVS applies the selected entity baseline, falling back
/// to the class's instance baseline, before applying explicit property deltas.
/// </summary>
public sealed class EntityWorld
{
    public const int SerialBits = 10;

    private readonly NetMessageWalker _walker = new();
    private readonly Dictionary<int, LiveEntity> _entities = [];
    private readonly HashSet<int> _dormant = [];
    private readonly Dictionary<int, int> _entityToPlayerId = [];
    private DataTableSet? _tables;
    private int _classBits;
    private PropIndexMode _mode = PropIndexMode.Unknown;
    private int _zeroBitInt64Width = 64;
    private Vec3? _lastCamera;
    private readonly Dictionary<int, int> _lastTeam = [];
    private readonly Dictionary<int, long> _lastParity = [];
    public IReadOnlyDictionary<int, byte[]> InstanceBaselines { get; set; } = new Dictionary<int, byte[]>();
    private readonly Dictionary<int, (byte[] Data, List<(FlattenedProp Prop, PropValue Value)> Props)>
        _classBaselines = [];

    /// <summary>Team number changes per player entity, from the scoreboard entity.</summary>
    public List<TeamEntry> TeamHistory { get; } = [];

    /// <summary>
    /// Weapon facts straight from the entity stream: "hold" (the SWEP's current hold type),
    /// "shot" (muzzle-flash parity changed) and "active" (a player's active weapon changed).
    /// </summary>
    public List<WeaponLogEntry> WeaponLog { get; } = [];

    public List<RagdollEntry> RagdollLog { get; } = [];

    /// <summary>Every state change of the recording player's view models (CPredictedViewModel).</summary>
    public List<ViewModelEntry> ViewModelLog { get; } = [];

    /// <summary>Vehicle pose parameter changes, the data the game animates cars from.</summary>
    public List<PoseEntry> PoseLog { get; } = [];

    private readonly Dictionary<int, float?[]> _lastPose = [];

    private sealed class ViewModelState
    {
        public int VmIndex, Owner = -1, Model, Sequence, NewSequenceParity, AnimationParity, Weapon = -1, Effects;
        public float Rate = 1f;
        public ViewModelEntry? Last;
    }

    private readonly Dictionary<int, ViewModelState> _viewModels = [];

    private sealed class RagdollState
    {
        public int Serial = -1;
        public Vec3[] Pos = new Vec3[32];
        public Vec3[] Ang = new Vec3[32];
        public int Count;
    }

    private readonly Dictionary<int, RagdollState> _ragdolls = [];

    // Entering entities are delta-encoded against these per-entity baseline snapshots,
    // not their last live state. Two slots, switched by the packet header.
    private readonly Dictionary<int, (int ClassId, List<(FlattenedProp Prop, PropValue Value)> Props)>[]
        _baselineEntities = [[], []];

    private readonly Dictionary<int, (int? Index, int Serial)> _lastActiveWeapon = [];

    /// <summary>Weapon class name per entity handle (index, serial), as last seen.</summary>
    public Dictionary<(int Index, int Serial), string> HandleNames { get; } = [];

    public DataTableSet? Tables => _tables;

    public PropIndexMode Mode => _mode;

    public EntityDecodeStats Stats { get; } = new();

    public int? RecorderEntity { get; private set; }

    private readonly Dictionary<int, int> _tickBaseUpdates = [];

    /// <summary>
    /// The entity that receives the local-player tables (m_nTickBase) is the recording
    /// player's own body, however the camera or the user-info table were matched.
    /// </summary>
    public int? LocalPlayerEntity
    {
        get
        {
            if (_tickBaseUpdates.Count == 0)
            {
                return null;
            }

            var best = _tickBaseUpdates.MaxBy(pair => pair.Value);
            return best.Value >= 100 ? best.Key : null;
        }
    }

    public void LoadTables(byte[] payload) => AttachTables(DataTableSet.Parse(payload));

    public void AttachTables(DataTableSet tables)
    {
        _classBaselines.Clear();
        _tables = tables;
        var count = Math.Max(tables.ClassDefs.Count, tables.Classes.Count);
        count = Math.Max(count, 1);
        _classBits = BitMath.Log2(count) + 1;
    }

    public void ForceMode(PropIndexMode mode)
    {
        _classBaselines.Clear();
        _mode = mode;
        Stats.Mode = mode;
    }

    public void BindPlayer(int entityIndex, int playerId) => _entityToPlayerId[entityIndex] = playerId;

    public int? PlayerIdFor(int entityIndex) =>
        _entityToPlayerId.TryGetValue(entityIndex, out var id) ? id : null;

    public EntitySnapshot? Snapshot(int entityIndex) =>
        _entities.TryGetValue(entityIndex, out var live) ? live.ToSnapshot() : null;

    public List<EntityChange> ApplyPacket(int tick, byte[] payload, Vec3? cameraOrigin)
    {
        _lastCamera = cameraOrigin;
        var changes = new List<EntityChange>();
        if (_tables is null || _tables.Overflowed || _tables.Classes.Count == 0)
        {
            return changes;
        }

        var walk = _walker.Walk(payload);
        foreach (var span in walk.Messages)
        {
            if (span.Type != NetMessageType.SvcPacketEntities)
            {
                continue;
            }

            ApplyMessage(tick, payload, span.StartBit, cameraOrigin, changes);
        }

        return changes;
    }

    public PropIndexMode Calibrate(IReadOnlyList<(int Tick, byte[] Payload, Vec3? Camera)> packets)
    {
        if (_tables is null || packets.Count == 0)
        {
            return PropIndexMode.Unknown;
        }

        var sample = packets.Count > 40 ? packets.Take(40).ToList() : packets;
        PropIndexMode best = PropIndexMode.Source2013;
        var bestInt64Width = 64;
        var bestScore = int.MinValue;
        var bestPackets = 0;

        foreach (var mode in new[] { PropIndexMode.Source2013, PropIndexMode.CsgoOldWay, PropIndexMode.CsgoNewWay })
        {
            foreach (var int64Width in new[] { 64, 60, 33 })
            {
                var candidate = new EntityWorld();
                candidate.AttachTables(_tables);
                candidate.ForceMode(mode);
                candidate._zeroBitInt64Width = int64Width;
                foreach (var (tick, payload, camera) in sample)
                {
                    candidate.ApplyPacket(tick, payload, camera);
                }

                var stats = candidate.Stats;
                var score = (stats.OriginsTrusted ? 1_000_000 : 0)
                    + (stats.PacketsClean * 20)
                    + stats.EntitiesEntered
                    + (stats.CameraMatches * 100)
                    - (stats.PacketsFailed * 5);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = mode;
                    bestInt64Width = int64Width;
                    bestPackets = stats.Packets;
                }
            }
        }

        if (bestPackets == 0)
        {
            return PropIndexMode.Unknown;
        }

        _zeroBitInt64Width = bestInt64Width;
        ForceMode(best);
        return best;
    }

    public PropIndexMode ChooseMode(byte[] payload) =>
        Calibrate([(0, payload, null)]);

    public string ProbeApplyUntilFail(byte[] payload, PropIndexMode mode)
    {
        if (_tables is null)
        {
            return "no tables";
        }

        ForceMode(mode);
        var walk = _walker.Walk(payload);
        foreach (var span in walk.Messages)
        {
            if (span.Type != NetMessageType.SvcPacketEntities)
            {
                continue;
            }

            var reader = new BitReader(payload);
            if (!TryReadHeader(ref reader, span.StartBit, out var header))
            {
                return "bad header";
            }

            var entered = 0;
            var lastClass = "";
            var lastFields = "";
            var recent = new List<string>();
            var index = -1;
            var endBit = header.DataStartBit + header.LengthBits;
            for (var i = 0; i < header.UpdatedEntries; i++)
            {
                if (reader.BitsRead >= endBit || reader.IsOverflowed)
                {
                    return $"eof at i={i} entered={entered} leftover={endBit - reader.BitsRead} lastFields={lastFields}";
                }

                index += 1 + (int)reader.ReadUBitVar();
                var leavePvs = reader.ReadOneBit() != 0;
                if (leavePvs)
                {
                    reader.ReadOneBit();
                    continue;
                }

                var enterPvs = reader.ReadOneBit() != 0;
                ServerClass? serverClass;
                if (enterPvs)
                {
                    var classId = (int)reader.ReadUBitLong(_classBits);
                    reader.ReadUBitLong(SerialBits);
                    if (!_tables.ClassesById.TryGetValue(classId, out serverClass)
                        && classId >= 0 && classId < _tables.Classes.Count)
                    {
                        serverClass = _tables.Classes[classId];
                    }

                    if (serverClass is null)
                    {
                        return $"unknown class {classId} after entered={entered} e={index}";
                    }
                }
                else if (!_entities.TryGetValue(index, out var existing))
                {
                    return $"delta missing e={index} after entered={entered} last={lastClass} fields={lastFields} recent={string.Join(",", recent)}";
                }
                else
                {
                    serverClass = existing.Class;
                }

                var start = reader.BitsRead;
                if (!TryReadProps(ref reader, serverClass, endBit, out var updates))
                {
                    return $"props fail i={i} e={index} class={serverClass.Name} flat={serverClass.Flattened.Count} entered={entered} last={lastClass} lastFields={lastFields} bits={reader.BitsRead - start} overflow={reader.IsOverflowed}";
                }

                lastFields = $"{updates.Count}b{reader.BitsRead - start}:{SummarizeUpdates(updates)}";
                if (enterPvs)
                {
                    _entities[index] = new LiveEntity(index, serverClass.ClassId, 0, serverClass);
                    entered++;
                    lastClass = $"{serverClass.Name}#{index}";
                    recent.Add(lastClass);
                    if (recent.Count > 8)
                    {
                        recent.RemoveAt(0);
                    }
                }
            }

            return $"all {header.UpdatedEntries} ok entered={entered} leftover={endBit - reader.BitsRead}";
        }

        return "no packet entities";
    }

    /// <summary>
    /// After the first unknown-delta, try reading class+serial as if the flags had been enter-PVS.
    /// If that class is known, the snapshot may encode some entities without the enter bit.
    /// </summary>
    public string PeekAfterFail(byte[] payload, PropIndexMode mode)
    {
        if (_tables is null)
        {
            return "no tables";
        }

        _entities.Clear();
        ForceMode(mode);
        var walk = _walker.Walk(payload);
        foreach (var span in walk.Messages)
        {
            if (span.Type != NetMessageType.SvcPacketEntities)
            {
                continue;
            }

            var reader = new BitReader(payload);
            if (!TryReadHeader(ref reader, span.StartBit, out var header))
            {
                return "bad header";
            }

            var index = -1;
            var endBit = header.DataStartBit + header.LengthBits;
            var entered = 0;
            for (var i = 0; i < header.UpdatedEntries; i++)
            {
                index += 1 + (int)reader.ReadUBitVar();
                var leavePvs = reader.ReadOneBit() != 0;
                if (leavePvs)
                {
                    reader.ReadOneBit();
                    continue;
                }

                var enterPvs = reader.ReadOneBit() != 0;
                if (!enterPvs && !_entities.ContainsKey(index))
                {
                    var classId = (int)reader.PeekUBitLong(_classBits);
                    var saved = reader.BitsRead;
                    var readClass = (int)reader.ReadUBitLong(_classBits);
                    var serial = (int)reader.ReadUBitLong(SerialBits);
                    _tables.ClassesById.TryGetValue(readClass, out var guess);
                    reader.Seek(saved);
                    var bits = new char[24];
                    for (var b = 0; b < bits.Length && !reader.IsOverflowed; b++)
                    {
                        bits[b] = reader.ReadOneBit() == 0 ? '0' : '1';
                    }

                    return $"i={i} e={index} entered={entered} classBits={_classBits} peekClass={classId}/{guess?.Name} serial={serial} bits={new string(bits)}";
                }

                ServerClass? serverClass;
                if (enterPvs)
                {
                    var classId = (int)reader.ReadUBitLong(_classBits);
                    reader.ReadUBitLong(SerialBits);
                    if (!_tables.ClassesById.TryGetValue(classId, out serverClass))
                    {
                        return $"unknown class {classId}";
                    }
                }
                else
                {
                    serverClass = _entities[index].Class;
                }

                if (!TryReadProps(ref reader, serverClass, endBit, out _))
                {
                    return $"props fail e={index} {serverClass.Name}";
                }

                if (enterPvs)
                {
                    _entities[index] = new LiveEntity(index, serverClass.ClassId, 0, serverClass);
                    entered++;
                }
            }

            return "no fail";
        }

        return "none";
    }

    public string PeekAfterClass(byte[] payload, int serialBits, int peekBits)
    {
        if (_tables is null)
        {
            return "";
        }

        var walk = _walker.Walk(payload);
        foreach (var span in walk.Messages)
        {
            if (span.Type != NetMessageType.SvcPacketEntities)
            {
                continue;
            }

            var reader = new BitReader(payload);
            if (!TryReadHeader(ref reader, span.StartBit, out var header))
            {
                continue;
            }

            var index = -1;
            for (var i = 0; i < header.UpdatedEntries && !reader.IsOverflowed; i++)
            {
                index += 1 + (int)reader.ReadUBitVar();
                if (reader.ReadOneBit() != 0)
                {
                    reader.ReadOneBit();
                    continue;
                }

                if (reader.ReadOneBit() == 0)
                {
                    continue;
                }

                var classId = (int)reader.ReadUBitLong(_classBits);
                var serial = (int)reader.ReadUBitLong(serialBits);
                var bits = new char[peekBits];
                for (var b = 0; b < peekBits; b++)
                {
                    bits[b] = reader.ReadOneBit() == 0 ? '0' : '1';
                }

                _tables.ClassesById.TryGetValue(classId, out var serverClass);
                return $"class={classId}/{serverClass?.Name} serial={serial} serialBits={serialBits} bits={new string(bits)}";
            }
        }

        return "none";
    }

    public readonly record struct FirstEnterProbe(
        bool IsDelta,
        int UpdatedEntries,
        int LengthBits,
        int EntityIndex,
        int ClassId,
        int Serial,
        bool KnownClass,
        string? ClassName,
        int ClassBits);

    /// <summary>
    /// Reads only the first enter-PVS header in a payload, stopping before property bits.
    /// Used to check class-id width and serial-number width without committing to a prop encoding.
    /// </summary>
    public readonly record struct PropStreamProbe(
        int ClassId,
        string? ClassName,
        int FlattenedCount,
        int FieldsRead,
        int LastIndex,
        string? FailedAt,
        bool Overflowed,
        int BitsConsumed,
        int LengthBits,
        string? Fields);

    public PropStreamProbe? ProbeFirstPropStream(byte[] payload, PropIndexMode mode)
    {
        if (_tables is null)
        {
            return null;
        }

        ForceMode(mode);
        var walk = _walker.Walk(payload);
        foreach (var span in walk.Messages)
        {
            if (span.Type != NetMessageType.SvcPacketEntities)
            {
                continue;
            }

            var reader = new BitReader(payload);
            if (!TryReadHeader(ref reader, span.StartBit, out var header))
            {
                continue;
            }

            var index = -1;
            for (var i = 0; i < header.UpdatedEntries && !reader.IsOverflowed; i++)
            {
                index += 1 + (int)reader.ReadUBitVar();
                var leavePvs = reader.ReadOneBit() != 0;
                if (leavePvs)
                {
                    reader.ReadOneBit();
                    continue;
                }

                if (reader.ReadOneBit() == 0)
                {
                    continue;
                }

                var classId = (int)reader.ReadUBitLong(_classBits);
                reader.ReadUBitLong(SerialBits);
                if (!_tables.ClassesById.TryGetValue(classId, out var serverClass)
                    && classId >= 0 && classId < _tables.Classes.Count)
                {
                    serverClass = _tables.Classes[classId];
                }

                if (serverClass is null)
                {
                    return new PropStreamProbe(classId, null, 0, 0, -1, "unknown class", reader.IsOverflowed, 0, header.LengthBits, null);
                }

                var start = reader.BitsRead;
                var last = -1;
                var fields = 0;
                var trail = new List<string>();
                var newWay = false;
                if (mode == PropIndexMode.CsgoNewWay)
                {
                    newWay = reader.ReadOneBit() != 0;
                }

                string Dump() => string.Join("; ", trail);

                while (true)
                {
                    if (reader.IsOverflowed)
                    {
                        return new PropStreamProbe(
                            classId, serverClass.Name, serverClass.Flattened.Count, fields, last,
                            "overflow before index", true, reader.BitsRead - start, header.LengthBits, Dump());
                    }

                    var next = ReadFieldIndex(ref reader, last, newWay);
                    if (next == -1)
                    {
                        return new PropStreamProbe(
                            classId, serverClass.Name, serverClass.Flattened.Count, fields, last,
                            reader.IsOverflowed ? "overflow at end marker" : "ok",
                            reader.IsOverflowed, reader.BitsRead - start, header.LengthBits, Dump());
                    }

                    if ((uint)next >= (uint)serverClass.Flattened.Count)
                    {
                        trail.Add($"#{next}?");
                        return new PropStreamProbe(
                            classId, serverClass.Name, serverClass.Flattened.Count, fields, next,
                            $"index {next} >= {serverClass.Flattened.Count}",
                            reader.IsOverflowed, reader.BitsRead - start, header.LengthBits, Dump());
                    }

                    last = next;
                    var prop = serverClass.Flattened[next];
                    var value = PropDecoder.Decode(ref reader, prop, _zeroBitInt64Width);
                    trail.Add($"{next}:{prop.Name}/{prop.Prop.Type}/{prop.Prop.Bits}");
                    if (value is null || reader.IsOverflowed)
                    {
                        return new PropStreamProbe(
                            classId, serverClass.Name, serverClass.Flattened.Count, fields, next,
                            $"decode {prop.Prop.Type} {prop.Table}.{prop.Name} bits={prop.Prop.Bits} flags={prop.Prop.Flags}",
                            reader.IsOverflowed, reader.BitsRead - start, header.LengthBits, Dump());
                    }

                    fields++;
                    if (fields > 4096)
                    {
                        return new PropStreamProbe(
                            classId, serverClass.Name, serverClass.Flattened.Count, fields, next,
                            "too many fields", true, reader.BitsRead - start, header.LengthBits, Dump());
                    }
                }
            }
        }

        return null;
    }

    public FirstEnterProbe? ProbeFirstEnter(byte[] payload)
    {
        if (_tables is null)
        {
            return null;
        }

        var walk = _walker.Walk(payload);
        foreach (var span in walk.Messages)
        {
            if (span.Type != NetMessageType.SvcPacketEntities)
            {
                continue;
            }

            var reader = new BitReader(payload);
            if (!TryReadHeader(ref reader, span.StartBit, out var header))
            {
                continue;
            }

            var index = -1;
            for (var i = 0; i < header.UpdatedEntries && !reader.IsOverflowed; i++)
            {
                index += 1 + (int)reader.ReadUBitVar();
                var leavePvs = reader.ReadOneBit() != 0;
                if (leavePvs)
                {
                    reader.ReadOneBit();
                    continue;
                }

                var enterPvs = reader.ReadOneBit() != 0;
                if (!enterPvs)
                {
                    continue;
                }

                var classId = (int)reader.ReadUBitLong(_classBits);
                var serial = (int)reader.ReadUBitLong(SerialBits);
                _tables.ClassesById.TryGetValue(classId, out var serverClass);
                if (serverClass is null && classId >= 0 && classId < _tables.Classes.Count)
                {
                    serverClass = _tables.Classes[classId];
                }

                return new FirstEnterProbe(
                    header.IsDelta,
                    header.UpdatedEntries,
                    header.LengthBits,
                    index,
                    classId,
                    serial,
                    serverClass is not null,
                    serverClass?.Name,
                    _classBits);
            }
        }

        return null;
    }

    private void ApplyMessage(int tick, byte[] payload, int startBit, Vec3? camera, List<EntityChange> changes)
    {
        if (_mode == PropIndexMode.Unknown)
        {
            ChooseMode(payload);
            if (_mode == PropIndexMode.Unknown)
            {
                return;
            }
        }

        var reader = new BitReader(payload);
        if (!TryReadHeader(ref reader, startBit, out var header))
        {
            Stats.PacketsFailed++;
            Stats.Packets++;
            return;
        }

        Stats.Packets++;
        var ok = TryReadEntities(ref reader, header, apply: true, tick, camera, changes);
        var consumed = reader.BitsRead - header.DataStartBit;
        if (!ok || reader.IsOverflowed || consumed > header.LengthBits)
        {
            Stats.PacketsFailed++;
            return;
        }

        if (header.LengthBits - consumed < 8)
        {
            Stats.PacketsClean++;
        }
    }

    private static bool TryReadHeader(ref BitReader reader, int startBit, out PacketEntitiesHeader header)
    {
        header = default;
        reader.Seek(startBit);
        reader.ReadUBitLong(NetMessageLayout.TypeBits);
        var maxEntries = (int)reader.ReadUBitLong(NetMessageLayout.MaxEdictBits);
        var isDelta = reader.ReadOneBit() != 0;
        var deltaFrom = 0;
        if (isDelta)
        {
            deltaFrom = reader.ReadLong();
        }

        var baseline = reader.ReadOneBit() != 0;
        var updated = (int)reader.ReadUBitLong(NetMessageLayout.MaxEdictBits);
        var lengthBits = (int)reader.ReadUBitLong(NetMessageLayout.DeltaSizeBits);
        var updateBaseline = reader.ReadOneBit() != 0;
        if (reader.IsOverflowed || lengthBits < 0 || lengthBits > reader.BitsLeft)
        {
            return false;
        }

        header = new PacketEntitiesHeader(
            maxEntries, isDelta, deltaFrom, baseline, updated, lengthBits, updateBaseline, reader.BitsRead);
        return true;
    }

    private bool TryReadEntities(
        ref BitReader reader,
        PacketEntitiesHeader header,
        bool apply,
        int tick,
        Vec3? camera,
        List<EntityChange>? changes)
    {
        var endBit = header.DataStartBit + header.LengthBits;
        var index = -1;
        // Entering entities are sent as a delta against the class baseline in use. A baseline
        // update promotes this packet's entering entities to the other slot (CopyEntityBaseline
        // first), so "weapon not sent" means "same as that baseline", which can be a real weapon.
        var baselineSlot = header.Baseline ? 1 : 0;
        if (apply && header.UpdateBaseline)
        {
            _baselineEntities[1 - baselineSlot] = new(_baselineEntities[baselineSlot]);
        }
        for (var i = 0; i < header.UpdatedEntries; i++)
        {
            if (reader.BitsRead >= endBit || reader.IsOverflowed)
            {
                return false;
            }

            index += 1 + (int)reader.ReadUBitVar();
            if ((uint)index >= 1u << NetMessageLayout.MaxEdictBits)
            {
                return false;
            }

            var leavePvs = reader.ReadOneBit() != 0;
            if (leavePvs)
            {
                var deleted = reader.ReadOneBit() != 0;
                if (apply)
                {
                    // A deleted weapon (dropped, consumed, unequipped) is gone from every
                    // inventory; one that merely left view is still owned.
                    if (deleted && _entities.TryGetValue(index, out var removed)
                        && removed.Class.Name.StartsWith("CWeapon", StringComparison.Ordinal))
                    {
                        WeaponLog.Add(new WeaponLogEntry(tick, "gone", index, null, 0, removed.Serial));
                    }

                    Fires.Remove(tick, index);
                    if (deleted)
                    {
                        RemoveEntity(index);
                    }
                    else
                    {
                        // Leaving PVS makes the client entity dormant, not deleted. A later
                        // delta can reference a frame where it was still visible.
                        _dormant.Add(index);
                    }
                    changes?.Add(new EntityRemoveChange(tick, index, deleted));
                    Stats.EntitiesLeft++;
                }

                continue;
            }

            var enterPvs = reader.ReadOneBit() != 0;
            if (enterPvs)
            {
                var classId = (int)reader.ReadUBitLong(_classBits);
                var serial = (int)reader.ReadUBitLong(SerialBits);
                if (!_tables!.ClassesById.TryGetValue(classId, out var serverClass)
                    && classId >= 0 && classId < _tables.Classes.Count)
                {
                    serverClass = _tables.Classes[classId];
                }

                if (serverClass is null)
                {
                    return false;
                }

                if (!TryReadProps(ref reader, serverClass, endBit, out var updates))
                {
                    return false;
                }

                if (apply)
                {
                    int? previousHeld = _entities.TryGetValue(index, out var previous)
                        ? previous.LastEmittedWeapon
                        : null;
                    if (previous is not null && previous.Serial != serial && previous.IsWeapon)
                    {
                        WeaponLog.Add(new WeaponLogEntry(tick, "gone", index, null, 0, previous.Serial));
                    }
                    List<(FlattenedProp Prop, PropValue Value)>? baselineProps = null;
                    if (_baselineEntities[baselineSlot].TryGetValue(index, out var baselineEntity)
                        && baselineEntity.ClassId == classId)
                    {
                        baselineProps = baselineEntity.Props;
                    }
                    else if (InstanceBaselines.TryGetValue(classId, out var baselineData))
                    {
                        if (!_classBaselines.TryGetValue(classId, out var cached)
                            || !ReferenceEquals(cached.Data, baselineData))
                        {
                            var baselineReader = new BitReader(baselineData);
                            if (!TryReadProps(ref baselineReader, serverClass, baselineData.Length * 8, out var decoded))
                                throw new InvalidDataException($"Could not decode instance baseline for {serverClass.Name} ({classId}).");
                            cached = (baselineData, decoded);
                            _classBaselines[classId] = cached;
                        }
                        baselineProps = cached.Props;
                    }
                    if (baselineProps is not null)
                    {
                        var merged = baselineProps.ToDictionary(p => p.Prop.Index);
                        foreach (var update in updates) merged[update.Prop.Index] = update;
                        updates = merged.Values.OrderBy(p => p.Prop.Index).ToList();
                    }
                    var live = new LiveEntity(index, classId, serial, serverClass);
                    if (serverClass.IsPlayer)
                    {
                        live.SetFlags(0);
                        live.SetLifeState(0);
                    }
                    live.Apply(updates);

                    if (header.UpdateBaseline)
                    {
                        _baselineEntities[1 - baselineSlot][index] = (classId, updates);
                    }

                    live.LastEmittedWeapon = previousHeld;
                    if (previous is not null && previous.Serial != serial) Fires.Remove(tick,index);
                    _entities[index] = live;
                    _dormant.Remove(index);
                    changes?.Add(new EntitySpawnChange(
                        tick,
                        index,
                        serverClass.Name,
                        serverClass.IsPlayer,
                        classId,
                        serial,
                        live.DisplayName,
                        live.EffectiveOwner,
                        live.IsWeapon,
                        live.IsDoor,
                        live.IsVehicle));
                    live.MarkSpawnLinks();
                    EmitInteresting(tick, live, updates, camera, changes, spawned: true);
                    Stats.EntitiesEntered++;
                }

                continue;
            }

            if (!apply)
            {
                // Scoring runs without entity state. A full update is all enter-PVS, so a delta
                // here means this candidate encoding is the wrong one.
                return false;
            }

            if (!_entities.TryGetValue(index, out var existing))
            {
                return false;
            }

            if (!TryReadProps(ref reader, existing.Class, endBit, out var delta))
            {
                return false;
            }

            if (apply)
            {
                existing.Apply(delta);
                var reappeared = _dormant.Remove(index);
                if (reappeared)
                {
                    changes?.Add(new EntitySpawnChange(
                        tick, index, existing.Class.Name, existing.Class.IsPlayer,
                        existing.ClassId, existing.Serial, existing.DisplayName, existing.EffectiveOwner,
                        existing.IsWeapon, existing.IsDoor, existing.IsVehicle));
                }
                EmitInteresting(tick, existing, delta, camera, changes, spawned: reappeared);
            }
        }

        // Dormant entities can be deleted without appearing in UpdatedEntries.
        // Delta packets carry a separate continuation-bit / entity-index list.
        if (header.IsDelta && reader.BitsRead < endBit)
        {
            var removed = new List<int>();
            while (true)
            {
                if (reader.BitsRead >= endBit) return false;
                if (reader.ReadOneBit() == 0) break;
                if (endBit - reader.BitsRead < NetMessageLayout.MaxEdictBits) return false;
                removed.Add((int)reader.ReadUBitLong(NetMessageLayout.MaxEdictBits));
            }

            if (apply)
            {
                foreach (var entityIndex in removed)
                {
                    if (_entities.TryGetValue(entityIndex, out var entity) && entity.IsWeapon)
                    {
                        WeaponLog.Add(new WeaponLogEntry(tick, "gone", entityIndex, null, 0, entity.Serial));
                    }
                    Fires.Remove(tick, entityIndex);
                    RemoveEntity(entityIndex);
                    changes?.Add(new EntityRemoveChange(tick, entityIndex, true));
                    Stats.EntitiesLeft++;
                }
            }
        }

        return !reader.IsOverflowed && reader.BitsRead <= endBit;
    }

    private void RemoveEntity(int index)
    {
        _entities.Remove(index);
        _entityToPlayerId.Remove(index);
        _dormant.Remove(index);
    }

    private bool TryReadProps(
        ref BitReader reader,
        ServerClass serverClass,
        int endBit,
        out List<(FlattenedProp Prop, PropValue Value)> updates)
    {
        updates = [];
        var last = -1;
        var flattened = serverClass.Flattened;
        var newWay = false;
        if (_mode == PropIndexMode.CsgoNewWay || _mode == PropIndexMode.CsgoOldWay)
        {
            newWay = _mode == PropIndexMode.CsgoNewWay && reader.ReadOneBit() != 0;
        }

        while (true)
        {
            if (reader.BitsRead >= endBit || reader.IsOverflowed)
            {
                return false;
            }

            var next = ReadFieldIndex(ref reader, last, newWay);
            if (next == -1)
            {
                return !reader.IsOverflowed;
            }

            if ((uint)next >= (uint)flattened.Count)
            {
                return false;
            }

            last = next;
            var prop = flattened[next];
            var value = PropDecoder.Decode(ref reader, prop, _zeroBitInt64Width);
            if (value is null || reader.IsOverflowed)
            {
                return false;
            }

            updates.Add((prop, value));
        }
    }

    /// <summary>
    /// Source 2013 <c>CDeltaBitsReader</c> is a continuation bit plus <c>ReadUBitVar</c>.
    /// CS:GO's later encoding (used as a fallback) reads a per-entity "new way" flag, then
    /// <c>ReadFieldIndex</c> with a 0xFFF end marker.
    /// </summary>
    private int ReadFieldIndex(ref BitReader reader, int last, bool newWay)
    {
        if (_mode == PropIndexMode.Source2013)
        {
            if (reader.ReadOneBit() == 0)
            {
                return -1;
            }

            return last + 1 + (int)reader.ReadUBitVar();
        }

        if (_mode is not (PropIndexMode.CsgoNewWay or PropIndexMode.CsgoOldWay))
        {
            return -1;
        }

        if (newWay && reader.ReadOneBit() != 0)
        {
            return last + 1;
        }

        uint extra;
        if (newWay && reader.ReadOneBit() != 0)
        {
            extra = reader.ReadUBitLong(3);
        }
        else
        {
            extra = reader.ReadUBitLong(7);
            switch (extra & (32 | 64))
            {
                case 32:
                    extra = (extra & ~96u) | (reader.ReadUBitLong(2) << 5);
                    break;
                case 64:
                    extra = (extra & ~96u) | (reader.ReadUBitLong(4) << 5);
                    break;
                case 96:
                    extra = (extra & ~96u) | (reader.ReadUBitLong(7) << 5);
                    break;
            }
        }

        if (extra == 0xFFF)
        {
            return -1;
        }

        return last + 1 + (int)extra;
    }

    private static string SummarizeUpdates(List<(FlattenedProp Prop, PropValue Value)> updates)
    {
        static string Show(PropValue value) => value switch
        {
            PropValue.Int i => i.Value.ToString(),
            PropValue.Float f => f.Value.ToString("0.###"),
            PropValue.Vector v => $"{v.Value.X:0.##},{v.Value.Y:0.##},{v.Value.Z:0.##}",
            PropValue.String s => s.Value.Length > 24 ? s.Value[..24] + "…" : s.Value,
            PropValue.Array a => $"[{a.Values.Count}]",
            _ => "?",
        };

        return string.Join(",", updates.Select(u => $"{u.Prop.Index}:{u.Prop.Name}={Show(u.Value)}"));
    }

    private string? NameOf(int? entityIndex)
    {
        if (entityIndex is not { } index || !_entities.TryGetValue(index, out var live))
        {
            return null;
        }

        return live.DisplayName;
    }

    public string? NameOfEntity(int entityIndex) => NameOf(entityIndex);

    public FireTimeline Fires { get; } = new();

    private void EmitInteresting(
        int tick,
        LiveEntity entity,
        List<(FlattenedProp Prop, PropValue Value)> updates,
        Vec3? camera,
        List<EntityChange>? changes,
        bool spawned)
    {
        if (changes is null)
        {
            return;
        }

        if ((Fires.ResolveClass(entity.RealClassIndex) ?? entity.DisplayName) == "ent_fire")
            Fires.Observe(tick, entity.ToSnapshot());

        var originChanged = false;
        var anglesChanged = false;
        var eyeChanged = false;
        var animChanged = spawned && entity.Class.Name=="CDynamicProp" && entity.Sequence is not null;
        var stateChanged = spawned && entity.Class.IsPlayer;
        var nameChanged = false;
        var ownerChanged = false;
        var vehicleChanged = entity.Class.IsPlayer
            && (entity.LastEmittedVehicle != entity.Vehicle
                || entity.LastEmittedParent != entity.Parent);
        var weaponChanged = false;
        var doorChanged = false;

        if (Environment.GetEnvironmentVariable("GMODDEM_DUMP_CLASS") is { Length: > 0 } dumpClass
            && (string.Equals(entity.Class.Name, dumpClass, StringComparison.Ordinal)
                || string.Equals(entity.DisplayName, dumpClass, StringComparison.Ordinal)))
        {
            // Research aid: prints every property update of one entity class to stderr.
            foreach (var (dumpProp, dumpValue) in updates)
            {
                Console.Error.WriteLine($"PROP {tick} {entity.Index} {dumpProp.Table}.{dumpProp.Name} = {dumpValue}");
            }
        }

        if (entity.Class.IsPlayer)
        {
            foreach (var (tickProp, _) in updates)
            {
                if (tickProp.Name == "m_nTickBase")
                {
                    _tickBaseUpdates[entity.Index] = _tickBaseUpdates.GetValueOrDefault(entity.Index) + 1;
                    break;
                }
            }
        }

        if (entity.Class.Name.StartsWith("CWeapon", StringComparison.Ordinal))
        {
            // Class names can arrive after the entity itself; remember the latest per handle.
            if (!string.IsNullOrEmpty(entity.DisplayName))
            {
                HandleNames[(entity.Index, entity.Serial)] = entity.DisplayName!;
            }

            if (spawned)
            {
                // An entity entering view only sends what differs from the class baseline, so
                // the firearm states (passive, reloading, sights up) that equal it are absent:
                // they are down. Without this the pose from before it left view would stick.
                foreach (var resetKind in new[] { "passive", "reload", "aim" })
                {
                    WeaponLog.Add(new WeaponLogEntry(tick, resetKind, entity.Index, "0", 0, entity.Serial));
                }
            }

            foreach (var (weaponProp, weaponValue) in updates)
            {
                if (weaponProp.Table == "DT_WeaponSWEP" && weaponProp.Name == "m_strHoldType"
                    && weaponValue is PropValue.String hold)
                {
                    WeaponLog.Add(new WeaponLogEntry(tick, "hold", entity.Index, hold.Value, 0, entity.Serial));
                }
                else if (weaponProp.Name == "m_nMuzzleFlashParity" && weaponValue is PropValue.Int parity)
                {
                    // The parity flips on every shot, so a change after the first value is a shot.
                    if (!spawned && _lastParity.TryGetValue(entity.Index, out var known)
                        && known != parity.Value)
                    {
                        WeaponLog.Add(new WeaponLogEntry(tick, "shot", entity.Index, null, 0, entity.Serial));
                    }

                    _lastParity[entity.Index] = parity.Value;
                }
                else if (weaponProp.Name == "m_hOwner" && weaponProp.Table == "DT_BaseCombatWeapon"
                    && weaponValue is PropValue.Int ownerHandle)
                {
                    // Who carries the weapon: owner entity index, or -1 when on nobody.
                    var ownerIndex = ownerHandle.Value == 8388607 || ownerHandle.Value < 0
                        ? -1 : (int)(ownerHandle.Value & ((1 << 13) - 1));
                    WeaponLog.Add(new WeaponLogEntry(
                        tick, "owner", entity.Index,
                        ownerIndex.ToString(System.Globalization.CultureInfo.InvariantCulture), 0, entity.Serial));
                }
                else if (weaponProp.Name == "m_GMOD_String0" && weaponValue is PropValue.String attachments)
                {
                    // The firearm base's AttachmentListString: comma separated attachment ids.
                    WeaponLog.Add(new WeaponLogEntry(tick, "attach", entity.Index, attachments.Value, 0, entity.Serial));
                }
                else if (weaponProp.Table == "m_GMOD_int" && weaponProp.Name == "001"
                    && weaponValue is PropValue.Int grenadeState)
                {
                    // Grenade-base slot 1 follows weapon_idle_base's StateIndex. Only
                    // grenade view models may interpret it as pin/spoon/throw state.
                    WeaponLog.Add(new WeaponLogEntry(tick, "grenade", entity.Index,
                        grenadeState.Value.ToString(System.Globalization.CultureInfo.InvariantCulture), 0, entity.Serial));
                }
                else if (weaponProp.Table is "m_GMOD_float" or "m_GMOD_bool")
                {
                    // Lua NetworkVar slots of the Perpheads firearm base: float 4 IronSights
                    // (CurTime the sights went up, 0 when down), bool 0 Passive, bool 1
                    // Reloading. Other weapon bases use these slots for other things, so the
                    // viewer only reads them for firearms.
                    var kind = (weaponProp.Table, weaponProp.Name) switch
                    {
                        ("m_GMOD_float", "004") => "aim",
                        ("m_GMOD_bool", "000") => "passive",
                        ("m_GMOD_bool", "001") => "reload",
                        _ => null,
                    };
                    var text = weaponValue switch
                    {
                        PropValue.Float f => f.Value.ToString(System.Globalization.CultureInfo.InvariantCulture),
                        PropValue.Int i => i.Value.ToString(System.Globalization.CultureInfo.InvariantCulture),
                        _ => null,
                    };
                    if (kind is not null && text is not null)
                    {
                        WeaponLog.Add(new WeaponLogEntry(tick, kind, entity.Index, text, 0, entity.Serial));
                    }
                }
            }
        }

        if (entity.Class.Name == "CPredictedViewModel")
        {
            if (!_viewModels.TryGetValue(entity.Index, out var vm))
            {
                vm = new ViewModelState();
                _viewModels[entity.Index] = vm;
            }

            var vmChanged = false;
            foreach (var (vmProp, vmValue) in updates)
            {
                var number = vmValue switch
                {
                    PropValue.Int i => (long?)i.Value,
                    _ => null,
                };
                switch (vmProp.Name)
                {
                    case "m_nModelIndex" when number is { } model: vm.Model = (int)model; vmChanged = true; break;
                    case "m_nSequence" when number is { } sequence: vm.Sequence = (int)sequence; vmChanged = true; break;
                    case "m_nViewModelIndex" when number is { } slot: vm.VmIndex = (int)slot; vmChanged = true; break;
                    case "m_nNewSequenceParity" when number is { } parity: vm.NewSequenceParity = (int)parity; vmChanged = true; break;
                    case "m_nAnimationParity" when number is { } parity: vm.AnimationParity = (int)parity; vmChanged = true; break;
                    case "m_hWeapon" when number is { } weapon: vm.Weapon = (int)weapon; vmChanged = true; break;
                    case "m_hOwner" when number is { } owner: vm.Owner = (int)owner; vmChanged = true; break;
                    case "m_fEffects" when number is { } effects: vm.Effects = (int)effects; vmChanged = true; break;
                    case "m_flPlaybackRate" when vmValue is PropValue.Float rate: vm.Rate = rate.Value; vmChanged = true; break;
                }
            }

            if (vmChanged)
            {
                var vmEntry = new ViewModelEntry(
                    tick, entity.Index, vm.VmIndex, vm.Owner, vm.Model, vm.Sequence, vm.Rate,
                    vm.NewSequenceParity, vm.AnimationParity, vm.Weapon, vm.Effects);
                // Properties are often re-sent unchanged; only real changes are logged.
                if (vm.Last is null || vm.Last with { Tick = tick } != vmEntry)
                {
                    ViewModelLog.Add(vmEntry);
                    vm.Last = vmEntry;
                }
            }
        }

        if (entity.Class.Name == "CRagdollProp")
        {
            // Ragdolls are simulated on the server and networked bone by bone, so the exact
            // pose of every knocked-out player or corpse is in the demo.
            if (!_ragdolls.TryGetValue(entity.Index, out var rag) || rag.Serial != entity.Serial)
            {
                rag = new RagdollState { Serial = entity.Serial };
                _ragdolls[entity.Index] = rag;
            }

            var ragChanged = false;
            foreach (var (ragProp, ragValue) in updates)
            {
                if (ragValue is not PropValue.Vector vector
                    || (!ragProp.Name.StartsWith("m_ragPos", StringComparison.Ordinal)
                        && !ragProp.Name.StartsWith("m_ragAngles", StringComparison.Ordinal)))
                {
                    continue;
                }

                var open = ragProp.Name.IndexOf('[');
                var close = ragProp.Name.IndexOf(']');
                if (open < 0 || close <= open
                    || !int.TryParse(ragProp.Name.AsSpan(open + 1, close - open - 1).Trim(), out var boneIndex)
                    || boneIndex < 0 || boneIndex >= rag.Pos.Length)
                {
                    continue;
                }

                if (ragProp.Name.StartsWith("m_ragPos", StringComparison.Ordinal))
                {
                    rag.Pos[boneIndex] = vector.Value;
                }
                else
                {
                    rag.Ang[boneIndex] = vector.Value;
                }

                rag.Count = Math.Max(rag.Count, boneIndex + 1);
                ragChanged = true;
            }

            if (ragChanged && rag.Count > 0)
            {
                RagdollLog.Add(new RagdollEntry(
                    tick, entity.Index, entity.Serial, entity.ModelIndex ?? 0,
                    rag.Pos[..rag.Count], rag.Ang[..rag.Count]));
            }
        }

        if (entity.IsVehicle)
        {
            // Source animates a car from its pose parameters (steering, spin and suspension
            // height of each wheel). Only values that differ from the baseline are sent when
            // the car enters view, so the missing ones are the baseline's zero.
            const int PoseCount = 24;
            if (!_lastPose.TryGetValue(entity.Index, out var lastPose))
            {
                lastPose = new float?[PoseCount];
                _lastPose[entity.Index] = lastPose;
            }

            var sent = new float?[PoseCount];
            foreach (var (poseProp, poseValue) in updates)
            {
                if (poseProp.Table == "m_flPoseParameter" && poseValue is PropValue.Float poseFloat
                    && int.TryParse(poseProp.Name, out var poseIndex) && poseIndex >= 0 && poseIndex < PoseCount)
                {
                    sent[poseIndex] = poseFloat.Value;
                }
            }

            var poseIndices = new List<int>();
            var poseValues = new List<float>();
            for (var i = 0; i < PoseCount; i++)
            {
                var value = sent[i] ?? (spawned ? 0f : null);
                if (value is not { } v)
                {
                    continue;
                }

                if (spawned || lastPose[i] is not { } previous || Math.Abs(previous - v) >= 0.002f)
                {
                    poseIndices.Add(i);
                    poseValues.Add(v);
                    lastPose[i] = v;
                }
            }

            if (poseIndices.Count > 0)
            {
                PoseLog.Add(new PoseEntry(tick, entity.Index, entity.Serial, [.. poseIndices], [.. poseValues]));
            }
        }

        if (entity.Class.Name == "CPlayerResource")
        {
            // The scoreboard entity carries every player's team, indexed by player entity.
            foreach (var (resourceProp, resourceValue) in updates)
            {
                if (resourceProp.Table != "m_iTeam"
                    || !int.TryParse(resourceProp.Name, out var teamSlot)
                    || resourceValue is not PropValue.Int teamValue)
                {
                    continue;
                }

                var teamNumber = checked((int)teamValue.Value);
                if (!_lastTeam.TryGetValue(teamSlot, out var previousTeam) || previousTeam != teamNumber)
                {
                    _lastTeam[teamSlot] = teamNumber;
                    TeamHistory.Add(new TeamEntry(tick, teamSlot, teamNumber));
                }
            }
        }

        foreach (var (prop, value) in updates)
        {
            switch (prop.Name)
            {
                case "m_vecOrigin":
                case "m_vecOrigin[0]":
                case "m_vecOrigin[1]":
                case "m_vecOrigin[2]":
                    originChanged = true;
                    break;
                case "m_angRotation":
                case "m_angRotation[0]":
                case "m_angRotation[1]":
                case "m_angRotation[2]":
                    anglesChanged = true;
                    break;
                case "m_angEyeAngles":
                case "m_angEyeAngles[0]":
                case "m_angEyeAngles[1]":
                case "m_angEyeAngles[2]":
                    eyeChanged = true;
                    break;
                case "m_nSequence":
                case "m_flCycle":
                case "m_flPlaybackRate":
                    animChanged = true;
                    break;
                case "m_iHealth":
                case "m_lifeState":
                case "m_fFlags":
                case "m_iTeamNum":
                case "m_nModelIndex":
                case "m_nSkin":
                case "m_nBody":
                    stateChanged = true;
                    break;
                case "m_strScriptName":
                    nameChanged = true;
                    break;
                case "m_hOwnerEntity":
                case "m_hOwner":
                case "m_hPlayer":
                    ownerChanged = true;
                    break;
                case "m_hActiveWeapon":
                    weaponChanged = true;
                    break;
                case "m_eDoorState":
                case "m_fEffects":
                    doorChanged = true;
                    break;
            }
        }

        if (originChanged && entity.Origin is { } origin)
        {
            if (entity.LastEmittedOrigin is null || !NearlyEqual(origin, entity.LastEmittedOrigin.Value))
            {
                entity.LastEmittedOrigin = origin;
                changes.Add(new EntityOriginChange(tick, entity.Index, origin));
                Stats.OriginUpdates++;
                MaybeMatchCamera(entity, origin, camera);
            }
        }

        if (anglesChanged && entity.Angles is { } angles
            && (entity.LastEmittedAngles is null || !NearlyEqual(angles, entity.LastEmittedAngles.Value)))
        {
            entity.LastEmittedAngles = angles;
            changes.Add(new EntityAnglesChange(tick, entity.Index, angles));
        }

        if (eyeChanged && entity.EyeAngles is { } eye
            && (entity.LastEmittedEye is null || !NearlyEqual(eye, entity.LastEmittedEye.Value)))
        {
            entity.LastEmittedEye = eye;
            changes.Add(new EntityEyeChange(tick, entity.Index, eye));
        }

        if (animChanged)
        {
            changes.Add(new EntityAnimChange(tick, entity.Index, entity.Sequence, entity.Cycle, entity.PlaybackRate));
        }

        if (stateChanged)
        {
            changes.Add(new EntityStateChange(
                tick,
                entity.Index,
                entity.Health,
                entity.LifeState,
                entity.Flags,
                entity.Team,
                entity.ModelIndex,
                entity.Skin,
                entity.Body));
        }

        if (entity.Class.IsPlayer && (weaponChanged || spawned)
            && (!_lastActiveWeapon.TryGetValue(entity.Index, out var lastActive)
                || lastActive.Index != entity.ActiveWeapon
                || lastActive.Serial != entity.ActiveWeaponSerial))
        {
            // Remembered per entity index, not per LiveEntity: a player leaving and re-entering
            // view gets a fresh LiveEntity, and "now holding nothing" must still be reported.
            _lastActiveWeapon[entity.Index] = (entity.ActiveWeapon, entity.ActiveWeaponSerial);
            entity.LastEmittedWeapon = entity.ActiveWeapon;
            entity.LastEmittedWeaponSerial = entity.ActiveWeaponSerial;
            WeaponLog.Add(new WeaponLogEntry(
                tick, "active", entity.Index, NameOf(entity.ActiveWeapon), entity.ActiveWeapon ?? -1,
                entity.ActiveWeaponSerial));
            changes.Add(new EntityWeaponChange(tick, entity.Index, entity.ActiveWeapon, NameOf(entity.ActiveWeapon)));
        }

        if (vehicleChanged)
        {
            changes.Add(new EntityVehicleChange(tick, entity.Index, entity.Vehicle, entity.Parent));
            entity.LastEmittedVehicle = entity.Vehicle;
            entity.LastEmittedParent = entity.Parent;
        }

        if (entity.TracksOwner && !spawned && (ownerChanged || nameChanged)
            && (entity.EffectiveOwner != entity.LastEmittedOwner || entity.DisplayName != entity.LastEmittedName))
        {
            entity.LastEmittedOwner = entity.EffectiveOwner;
            entity.LastEmittedName = entity.DisplayName;
            changes.Add(new EntityOwnerChange(tick, entity.Index, entity.EffectiveOwner, entity.DisplayName));
        }

        if (entity.Class.Name == "CDynamicProp" && (spawned || doorChanged))
        {
            var hidden = ((entity.Effects ?? 0) & 32) != 0;
            if (spawned || entity.LastEmittedVisibility != hidden)
            {
                entity.LastEmittedVisibility = hidden;
                changes.Add(new EntityVisibilityChange(tick,entity.Index,hidden));
            }
        }

        if (entity.IsDoor
            && (entity.DoorState is not null || entity.Origin is not null)
            && (doorChanged || originChanged || spawned))
        {
            var doorOrigin = entity.Origin;
            // EF_NODRAW (32): a door blown off its frame stays networked but is not drawn.
            var hidden = ((entity.Effects ?? 0) & 32) != 0;
            if (entity.LastEmittedDoorState != entity.DoorState
                || entity.LastEmittedDoorHidden != hidden
                || entity.LastEmittedDoorOrigin is null
                || (doorOrigin is { } next && !NearlyEqual(next, entity.LastEmittedDoorOrigin.Value)))
            {
                entity.LastEmittedDoorState = entity.DoorState;
                entity.LastEmittedDoorHidden = hidden;
                entity.LastEmittedDoorOrigin = doorOrigin;
                changes.Add(new EntityDoorChange(tick, entity.Index, doorOrigin, entity.DoorState, hidden));
            }
        }
    }

    private void MaybeMatchCamera(LiveEntity entity, Vec3 origin, Vec3? camera)
    {
        if (camera is null || !entity.Class.IsPlayer)
        {
            return;
        }

        var viewZ = entity.ViewOffset?.Z ?? 64f;
        var dx = origin.X - camera.Value.X;
        var dy = origin.Y - camera.Value.Y;
        var dz = origin.Z + viewZ - camera.Value.Z;
        if ((dx * dx) + (dy * dy) + (dz * dz) < 90f * 90f)
        {
            Stats.CameraMatches++;
            Stats.OriginsTrusted = true;
            RecorderEntity = entity.Index;
        }
    }

    private static bool NearlyEqual(Vec3 a, Vec3 b) =>
        Math.Abs(a.X - b.X) < 0.01f
        && Math.Abs(a.Y - b.Y) < 0.01f
        && Math.Abs(a.Z - b.Z) < 0.01f;

    private sealed class LiveEntity(int index, int classId, int serial, ServerClass serverClass)
    {
        public int Index { get; } = index;

        public int ClassId { get; } = classId;

        public int Serial { get; } = serial;

        public ServerClass Class { get; } = serverClass;

        public Vec3? Origin { get; private set; }

        public Vec3? Angles { get; private set; }

        public Vec3? EyeAngles { get; private set; }

        public Vec3? ViewOffset { get; private set; }

        public int? Sequence { get; private set; }

        public float? Cycle { get; private set; }

        public float? PlaybackRate { get; private set; }

        public int? Health { get; private set; }

        public int? LifeState { get; private set; }

        public void SetLifeState(int lifeState) => LifeState = lifeState;

        public int? Flags { get; private set; }

        public void SetFlags(int flags) => Flags = flags;

        public int? Team { get; private set; }

        public int? ModelIndex { get; private set; }

        public int? Skin { get; private set; }

        public int? Body { get; private set; }

        public int? UserId { get; private set; }

        public string? ScriptName { get; private set; }

        public int? OwnerEntity { get; private set; }

        public int? HandleOwner { get; private set; }

        public int? ActiveWeapon { get; private set; }

        /// <summary>The raw networked weapon handle (index and serial), invalid when none.</summary>
        public long ActiveWeaponRaw { get; private set; } = 0x7FFFFF;

        /// <summary>Adopts the class baseline's weapon handle when it was not sent.</summary>
        public void SetActiveWeaponRaw(long raw)
        {
            ActiveWeaponRaw = raw;
            ActiveWeapon = EntityIdentity.DecodeHandle(raw);
            ActiveWeaponSerial = EntityIdentity.DecodeSerial(raw);
        }

        public int ActiveWeaponSerial { get; private set; } = -1;

        public int LastEmittedWeaponSerial { get; set; } = -1;

        public int? Vehicle { get; private set; }

        public int? LastEmittedVehicle { get; set; }

        public int? Driver { get; private set; }

        public int? DoorState { get; private set; }

        public int? Parent { get; private set; }

        public int? LastEmittedParent { get; set; }

        public int? CreationId { get; private set; }

        public int? MapCreatedId { get; private set; }

        public float? CreationTime { get; private set; }

        public float? ModelScale { get; private set; }

        public int? RealClassIndex { get; private set; }

        public string? GmodString0 { get; private set; }

        public string? GmodString1 { get; private set; }

        public string? GmodString2 { get; private set; }

        public string? GmodString3 { get; private set; }

        public Vec3? LastEmittedOrigin { get; set; }

        public Vec3? LastEmittedAngles { get; set; }

        public Vec3? LastEmittedEye { get; set; }

        public int? LastEmittedOwner { get; set; }

        public string? LastEmittedName { get; set; }

        public int? LastEmittedWeapon { get; set; }

        public int? LastEmittedDoorState { get; set; }
        public bool LastEmittedDoorHidden { get; set; }
        public bool? LastEmittedVisibility { get; set; }
        public int? Effects { get; private set; }

        public Vec3? LastEmittedDoorOrigin { get; set; }

        public bool IsWeapon => EntityIdentity.IsWeaponClass(Class.Name);

        public bool IsDoor => EntityIdentity.IsDoorClass(Class.Name);

        public bool IsVehicle => EntityIdentity.IsVehicleClass(Class.Name);

        public bool TracksOwner => EntityIdentity.TracksOwner(Class.Name);

        public int? EffectiveOwner => HandleOwner ?? OwnerEntity ?? (IsVehicle ? Driver : null);

        public string? DisplayName =>
            !string.IsNullOrEmpty(ScriptName) ? ScriptName : EntityIdentity.StockEntityName(Class.Name);

        public void MarkSpawnLinks()
        {
            LastEmittedOwner = EffectiveOwner;
            LastEmittedName = DisplayName;
        }

        public void Apply(List<(FlattenedProp Prop, PropValue Value)> updates)
        {
            foreach (var (prop, value) in updates)
            {
                switch (prop.Name)
                {
                    case "m_vecOrigin":
                        Origin = AsVec3(value) ?? Origin;
                        break;
                    case "m_vecOrigin[0]":
                        Origin = SetX(Origin, AsFloat(value));
                        break;
                    case "m_vecOrigin[1]":
                        Origin = SetY(Origin, AsFloat(value));
                        break;
                    case "m_vecOrigin[2]":
                        Origin = SetZ(Origin, AsFloat(value));
                        break;
                    case "m_angRotation":
                        Angles = AsVec3(value) ?? Angles;
                        break;
                    case "m_angRotation[0]":
                        Angles = SetX(Angles, AsFloat(value));
                        break;
                    case "m_angRotation[1]":
                        Angles = SetY(Angles, AsFloat(value));
                        break;
                    case "m_angRotation[2]":
                        Angles = SetZ(Angles, AsFloat(value));
                        break;
                    case "m_angEyeAngles":
                        EyeAngles = AsVec3(value) ?? EyeAngles;
                        break;
                    case "m_angEyeAngles[0]":
                        EyeAngles = SetX(EyeAngles, AsFloat(value));
                        break;
                    case "m_angEyeAngles[1]":
                        EyeAngles = SetY(EyeAngles, AsFloat(value));
                        break;
                    case "m_angEyeAngles[2]":
                        EyeAngles = SetZ(EyeAngles, AsFloat(value));
                        break;
                    case "m_vecViewOffset":
                    case "m_vecViewOffset[0]":
                    case "m_vecViewOffset[1]":
                    case "m_vecViewOffset[2]":
                        if (prop.Name == "m_vecViewOffset")
                        {
                            ViewOffset = AsVec3(value) ?? ViewOffset;
                        }
                        else if (prop.Name.EndsWith("[0]", StringComparison.Ordinal))
                        {
                            ViewOffset = SetX(ViewOffset, AsFloat(value));
                        }
                        else if (prop.Name.EndsWith("[1]", StringComparison.Ordinal))
                        {
                            ViewOffset = SetY(ViewOffset, AsFloat(value));
                        }
                        else
                        {
                            ViewOffset = SetZ(ViewOffset, AsFloat(value));
                        }

                        break;
                    case "m_nSequence":
                        Sequence = AsInt(value);
                        break;
                    case "m_flCycle":
                        Cycle = AsFloat(value);
                        break;
                    case "m_flPlaybackRate":
                        PlaybackRate = AsFloat(value);
                        break;
                    case "m_iHealth":
                        Health = AsInt(value);
                        break;
                    case "m_lifeState":
                        LifeState = AsInt(value);
                        break;
                    case "m_fFlags":
                        Flags = AsInt(value);
                        break;
                    case "m_iTeamNum":
                        Team = AsInt(value);
                        break;
                    case "m_nModelIndex":
                        ModelIndex = AsInt(value);
                        break;
                    case "m_nSkin":
                        Skin = AsInt(value);
                        break;
                    case "m_nBody":
                        Body = AsInt(value);
                        break;
                    case "m_iUserID":
                    case "m_iUserId":
                        UserId = AsInt(value);
                        break;
                    case "m_strScriptName":
                        ScriptName = AsString(value);
                        break;
                    case "m_hOwnerEntity":
                        OwnerEntity = EntityIdentity.DecodeHandle(AsLong(value));
                        break;
                    case "m_hOwner":
                        HandleOwner = EntityIdentity.DecodeHandle(AsLong(value));
                        break;
                    case "m_hActiveWeapon":
                        ActiveWeaponRaw = AsLong(value);
                        ActiveWeapon = EntityIdentity.DecodeHandle(ActiveWeaponRaw);
                        ActiveWeaponSerial = EntityIdentity.DecodeSerial(ActiveWeaponRaw);
                        break;
                    case "m_hVehicle":
                        Vehicle = EntityIdentity.DecodeHandle(AsLong(value));
                        break;
                    case "m_hPlayer":
                        Driver = EntityIdentity.DecodeHandle(AsLong(value));
                        break;
                    case "m_eDoorState":
                        DoorState = AsInt(value);
                        break;
                    case "m_fEffects":
                        Effects = AsInt(value);
                        break;
                    case "moveparent":
                        Parent = EntityIdentity.DecodeHandle(AsLong(value));
                        break;
                    case "m_iCreationID":
                        CreationId = AsInt(value);
                        break;
                    case "m_iMapCreatedID":
                        MapCreatedId = AsInt(value);
                        break;
                    case "m_CreationTime":
                        CreationTime = AsFloat(value);
                        break;
                    case "m_flModelScale":
                        ModelScale = AsFloat(value);
                        break;
                    case "m_RealClassName":
                        RealClassIndex = AsInt(value);
                        break;
                    case "m_GMOD_String0":
                        GmodString0 = AsString(value);
                        break;
                    case "m_GMOD_String1":
                        GmodString1 = AsString(value);
                        break;
                    case "m_GMOD_String2":
                        GmodString2 = AsString(value);
                        break;
                    case "m_GMOD_String3":
                        GmodString3 = AsString(value);
                        break;
                }
            }
        }

        public EntitySnapshot ToSnapshot() => new(
            Index, ClassId, Serial, Class.Name, Class.IsPlayer,
            Origin, Angles, EyeAngles, ViewOffset,
            Sequence, Cycle, PlaybackRate,
            Health, LifeState, Flags, Team, ModelIndex, Skin, Body, UserId,
            DisplayName, EffectiveOwner, ActiveWeapon, Vehicle, Driver, DoorState,
            Parent, CreationId, MapCreatedId, CreationTime, ModelScale, RealClassIndex,
            GmodString0, GmodString1, GmodString2, GmodString3);

        private static Vec3? AsVec3(PropValue value) => value is PropValue.Vector vector ? vector.Value : null;

        private static float? AsFloat(PropValue value) => value switch
        {
            PropValue.Float f => f.Value,
            PropValue.Int i => i.Value,
            _ => null,
        };

        private static int? AsInt(PropValue value) => value switch
        {
            PropValue.Int i => (int)i.Value,
            PropValue.Float f => (int)f.Value,
            _ => null,
        };

        private static long AsLong(PropValue value) => value switch
        {
            PropValue.Int i => i.Value,
            PropValue.Float f => (long)f.Value,
            _ => 0,
        };

        private static string? AsString(PropValue value) =>
            value is PropValue.String s && s.Value.Length > 0 ? s.Value : null;

        private static Vec3 SetX(Vec3? current, float? value) =>
            new(value ?? current?.X ?? 0f, current?.Y ?? 0f, current?.Z ?? 0f);

        private static Vec3 SetY(Vec3? current, float? value) =>
            new(current?.X ?? 0f, value ?? current?.Y ?? 0f, current?.Z ?? 0f);

        private static Vec3 SetZ(Vec3? current, float? value) =>
            new(current?.X ?? 0f, current?.Y ?? 0f, value ?? current?.Z ?? 0f);
    }
}
