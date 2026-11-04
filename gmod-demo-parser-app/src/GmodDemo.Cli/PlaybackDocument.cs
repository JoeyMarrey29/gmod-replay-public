using System.Text.Json;

namespace GmodDemo.Parser;

/// <summary>
/// Unreal-oriented layout: each player owns a tick-by-tick pose, and world ents live in
/// <c>objects</c> (spawns, doors, vehicles) instead of a mixed event log.
/// </summary>
public sealed class PlaybackDocument
{
    public const int SchemaVersion = 4;

    private readonly IReadOnlyList<PlayerTrack> _tracks;
    private readonly IReadOnlyList<ObjectRow> _objects;
    private readonly double _tickRate;
    private readonly PrecacheTables _precache;

    private PlaybackDocument(
        IReadOnlyList<PlayerTrack> tracks,
        IReadOnlyList<ObjectRow> objects,
        double tickRate,
        PrecacheTables precache)
    {
        _tracks = tracks;
        _objects = objects;
        _tickRate = tickRate;
        _precache = precache;
    }

    public static PlaybackDocument Build(
        IReadOnlyList<DemoPlayer> players,
        IReadOnlyList<GameEvent> events,
        PrecacheTables precache,
        double tickRate,
        IReadOnlyList<VoiceClip>? clips = null,
        int? fromTick = null,
        int? toTick = null)
    {
        var playerEntities = events
            .OfType<SpawnEvent>()
            .Where(spawn => spawn.IsPlayer)
            .Select(spawn => spawn.Entity)
            .ToHashSet();
        var entityToPlayer = new Dictionary<int, int>();
        foreach (var group in players
                     .Select(player => (
                         Player: player,
                         Entity: player.Slot >= 0 ? player.Entity : player.RawEntity))
                     .Where(entry => entry.Entity is >= 1)
                     .Select(entry => (
                         entry.Player,
                         Entity: entry.Entity!.Value))
                     .GroupBy(entry => entry.Entity))
        {
            var selected = group
                .OrderByDescending(entry => entry.Player.UserId is not null)
                .ThenByDescending(entry => entry.Player.Name is { Length: > 0 })
                .First();
            if (selected.Player.Slot >= 0 || playerEntities.Contains(selected.Entity))
            {
                entityToPlayer[selected.Entity] = selected.Player.Id;
            }
        }

        var tracks = players.ToDictionary(
            p => p.Id,
            p => new PlayerTrack(p, ClipsFor(clips, p, fromTick, toTick), fromTick, toTick));
        var objects = new List<ObjectRow>();
        var spawnByEntity = new Dictionary<int, ObjectRow>();

        foreach (var gameEvent in events)
        {
            switch (gameEvent)
            {
                case ChatEvent chat when chat.P is { } chatId && tracks.TryGetValue(chatId, out var chatTrack):
                    if (InWindow(chat.T, fromTick, toTick)
                        && !chatTrack.Chat.Any(c => c.T == chat.T && c.Text == chat.Text))
                    {
                        chatTrack.Chat.Add(new ChatRow(chat.T, chat.Text, chat.SenderEntity, chat.TeamOnly));
                    }

                    break;

                case CameraMoveEvent camera when camera.P is { } camId && tracks.TryGetValue(camId, out var camTrack):
                    camTrack.Apply(camera.T, pose =>
                    {
                        pose.Origin = camera.Origin;
                        pose.Angles = camera.Angles;
                    });
                    break;

                case MoveEvent move when IsBody(move, entityToPlayer, tracks, out var moveTrack):
                    moveTrack.Apply(move.T, pose => pose.Origin = move.Origin);
                    break;

                case LookEvent look when IsBody(look, entityToPlayer, tracks, out var lookTrack):
                    lookTrack.Apply(look.T, pose => pose.Angles = look.Angles);
                    break;

                // Vehicles, physics props and scripted props: their full rotation over time.
                case LookEvent turn when spawnByEntity.TryGetValue(turn.Entity, out var turned)
                    && (turned.IsVehicle || turned.IsDoor || IsPlacedClass(turned.ClassName)
                        || IsMapMover(turned,precache))
                    && InWindow(turn.T, fromTick, toTick):
                    objects.Add(new ObjectRow
                    {
                        T = turn.T,
                        Type = "turn",
                        Entity = turn.E,
                        Angles = turn.Angles,
                    });
                    break;

                case StateEvent state when IsBody(state, entityToPlayer, tracks, out var stateTrack):
                    stateTrack.Apply(state.T, pose =>
                    {
                        pose.Health = state.Health ?? pose.Health;
                        pose.LifeState = state.LifeState ?? pose.LifeState;
                        pose.Flags = state.Flags ?? pose.Flags;
                        pose.Team = state.Team ?? pose.Team;
                        pose.ModelIndex = state.ModelIndex ?? pose.ModelIndex;
                        pose.Skin = state.Skin ?? pose.Skin;
                        pose.Body = state.Body ?? pose.Body;
                    });
                    break;

                case PlayerVehicleEvent vehicle
                    when IsBody(vehicle, entityToPlayer, tracks, out var vehicleTrack):
                    vehicleTrack.Apply(vehicle.T, pose =>
                    {
                        pose.VehicleKnown = true;
                        pose.VehicleChanged = true;
                        pose.Vehicle = vehicle.Vehicle;
                        pose.Parent = vehicle.Parent;
                    });
                    break;

                case AnimEvent anim when IsBody(anim, entityToPlayer, tracks, out var animTrack):
                    animTrack.Apply(anim.T, pose =>
                    {
                        pose.Sequence = anim.Sequence ?? pose.Sequence;
                        pose.Cycle = anim.Cycle ?? pose.Cycle;
                        pose.PlaybackRate = anim.PlaybackRate ?? pose.PlaybackRate;
                    });
                    break;

                case WeaponEvent weapon when IsBody(weapon, entityToPlayer, tracks, out var weaponTrack):
                    weaponTrack.Apply(weapon.T, pose => pose.Weapon = weapon.Name ?? pose.Weapon);
                    break;

                case SpawnEvent spawn when spawn.IsPlayer && IsBody(spawn, entityToPlayer, tracks, out var spawnTrack):
                    spawnTrack.Apply(spawn.T, pose =>
                    {
                        pose.Spawned = true;
                        pose.Origin = spawn.Origin ?? pose.Origin;
                        pose.ModelIndex = spawn.ModelIndex ?? pose.ModelIndex;
                        pose.Health = spawn.Health ?? pose.Health;
                        pose.Weapon = spawn.HeldWeapon ?? pose.Weapon;
                    });
                    break;

                case RemoveEvent remove when IsBody(remove, entityToPlayer, tracks, out var removeTrack):
                    removeTrack.Apply(remove.T, pose => pose.Gone = true);
                    break;

                case SpawnEvent spawn:
                {
                    if (spawn.IsWeapon && spawn.Name is { Length: > 0 } && spawn.P is { } holderId
                        && tracks.TryGetValue(holderId, out var holder))
                    {
                        holder.Apply(spawn.T, pose => pose.Weapon ??= spawn.Name);
                    }

                    var row = ObjectRow.FromSpawn(spawn);
                    if (spawnByEntity.TryGetValue(spawn.Entity,out var previous)
                        && IsMapMover(previous,precache) && row.ClassName == previous.ClassName
                        && (row.CreationId is null || row.CreationId == previous.CreationId)
                        && (row.ModelIndex is null || row.ModelIndex == previous.ModelIndex))
                    {
                        row.ModelIndex ??= previous.ModelIndex;
                        row.Parent ??= previous.Parent;
                        row.CreationId ??= previous.CreationId;
                        row.MapCreatedId ??= previous.MapCreatedId;
                    }
                    if (InWindow(spawn.T, fromTick, toTick))
                    {
                        objects.Add(row);
                    }

                    spawnByEntity[spawn.Entity] = row;
                    break;
                }

                case AnimEvent anim when InWindow(anim.T,fromTick,toTick)
                    && spawnByEntity.TryGetValue(anim.Entity,out var animated) && IsBankShutter(animated,precache):
                    objects.Add(new ObjectRow { T=anim.T,Type="animation",Entity=anim.Entity,
                        Sequence=anim.Sequence,Cycle=anim.Cycle,PlaybackRate=anim.PlaybackRate });
                    break;
                case MoveEvent move:
                {
                    if (spawnByEntity.TryGetValue(move.Entity, out var spawned) && spawned.Origin is null
                        && !IsMapMover(spawned,precache))
                    {
                        spawned.Origin = move.Origin;
                        break;
                    }

                    if (move.P is null && spawned is { IsVehicle: false, IsWeapon: false }
                        && !IsPlacedClass(spawned.ClassName) && !IsMapMover(spawned,precache))
                    {
                        break;
                    }

                    if (InWindow(move.T, fromTick, toTick))
                    {
                        objects.Add(new ObjectRow
                        {
                            T = move.T,
                            Type = "move",
                            Entity = move.E,
                            P = move.P,
                            Origin = move.Origin,
                            Kind = spawned?.Kind,
                            ClassName = spawned?.ClassName,
                            Name = spawned?.Name,
                            IsVehicle = spawned?.IsVehicle ?? false,
                            IsWeapon = spawned?.IsWeapon ?? false,
                            IsDoor = spawned?.IsDoor ?? false,
                        });
                    }

                    break;
                }

                case VisibilityEvent visibility when InWindow(visibility.T,fromTick,toTick):
                    objects.Add(new ObjectRow {
                        T=visibility.T,Type="visibility",Entity=visibility.E,Hidden=visibility.Hidden
                    });
                    break;
                case DoorEvent door when InWindow(door.T, fromTick, toTick):
                    objects.Add(new ObjectRow
                    {
                        T = door.T,
                        Type = "door",
                        Entity = door.E,
                        P = door.P,
                        Origin = door.Origin,
                        Kind = "door",
                        IsDoor = true,
                        DoorState = door.DoorState,
                        Hidden = door.Hidden,
                    });
                    break;

                case OwnerEvent owner when InWindow(owner.T, fromTick, toTick):
                    objects.Add(new ObjectRow
                    {
                        T = owner.T,
                        Type = "owner",
                        Entity = owner.E,
                        P = owner.P,
                        Owner = owner.Owner,
                        Name = owner.Name,
                        Kind = spawnByEntity.GetValueOrDefault(owner.Entity)?.Kind,
                    });
                    break;

                case RemoveEvent remove when InWindow(remove.T, fromTick, toTick):
                    objects.Add(new ObjectRow
                    {
                        T = remove.T,
                        Type = "remove",
                        Entity = remove.E,
                        P = remove.P,
                        Deleted = remove.Deleted,
                        Kind = spawnByEntity.GetValueOrDefault(remove.Entity)?.Kind,
                    });
                    if (remove.Deleted && spawnByEntity.TryGetValue(remove.Entity,out var removed)
                        && IsMapMover(removed,precache)) spawnByEntity.Remove(remove.Entity);
                    break;

                case SourceGameEvent game when InWindow(game.T, fromTick, toTick):
                    objects.Add(new ObjectRow
                    {
                        T = game.T,
                        Type = "game",
                        Entity = game.E,
                        P = game.P,
                        Name = game.Name,
                        GameId = game.Id,
                        GameKeys = game.Keys,
                    });
                    break;
            }
        }

        foreach (var track in tracks.Values)
        {
            track.Flush();
        }

        var ordered = players.Select(p => tracks[p.Id]).ToList();
        return new PlaybackDocument(ordered, objects, tickRate, precache);
    }

    public void Write(Utf8JsonWriter writer)
    {
        writer.WriteStartArray("players");
        foreach (var track in _tracks)
        {
            WritePlayer(writer, track);
        }

        writer.WriteEndArray();

        writer.WriteStartArray("objects");
        foreach (var row in _objects)
        {
            WriteObject(writer, row);
        }

        writer.WriteEndArray();
    }

    private void WritePlayer(Utf8JsonWriter writer, PlayerTrack track)
    {
        var player = track.Player;
        writer.WriteStartObject();
        writer.WriteNumber("id", player.Id);
        if (player.Slot >= 0)
        {
            writer.WriteNumber("slot", player.Slot);
        }

        if (player.RawEntity is { } rawEntity)
        {
            writer.WriteNumber("entity", rawEntity);
        }
        else if (player.Slot >= 0)
        {
            writer.WriteNumber("entity", player.Entity);
        }

        if (player.UserId is { } userId)
        {
            writer.WriteNumber("userId", userId);
        }

        writer.WriteString("name", player.Name);
        writer.WriteString("steamId", player.SteamId);
        writer.WriteString("steamId64", player.SteamId64);
        if (player.ConnectedTick is { } connected)
        {
            writer.WriteNumber("connectedAt", Time(connected));
        }
        if (player.DisconnectedTick is { } disconnected)
        {
            writer.WriteNumber("disconnectedAt", Time(disconnected));
        }
        if (player.FakePlayer)
        {
            writer.WriteBoolean("fakePlayer", true);
        }

        if (player.IsHltv)
        {
            writer.WriteBoolean("isHltv", true);
        }

        if (track.Voice.Count > 0)
        {
            writer.WriteStartArray("voice");
            foreach (var clip in track.Voice)
            {
                writer.WriteStartObject();
                writer.WriteNumber("t", clip.StartTick);
                writer.WriteNumber("time", clip.Time);
                writer.WriteNumber("tEnd", clip.EndTick);
                writer.WriteNumber("duration", clip.Duration);
                writer.WriteBoolean("proximity", clip.Proximity);
                writer.WriteString("file", clip.File);
                writer.WriteEndObject();
            }

            writer.WriteEndArray();
        }

        writer.WriteStartArray("ticks");
        foreach (var pose in track.Ticks)
        {
            writer.WriteStartObject();
            writer.WriteNumber("t", pose.T);
            writer.WriteNumber("time", Time(pose.T));
            WriteVec(writer, pose.Origin);
            WriteAngles(writer, pose.Angles);
            if (pose.Weapon is { Length: > 0 })
            {
                writer.WriteString("weapon", pose.Weapon);
            }

            WriteOptionalInt(writer, "health", pose.Health);
            WriteOptionalInt(writer, "lifeState", pose.LifeState);
            WriteOptionalInt(writer, "flags", pose.Flags);
            WriteOptionalInt(writer, "team", pose.Team);
            WriteModel(writer, pose.ModelIndex);
            WriteOptionalInt(writer, "skin", pose.Skin);
            WriteOptionalInt(writer, "body", pose.Body);
            WriteOptionalInt(writer, "sequence", pose.Sequence);
            if (pose.Cycle is { } cycle)
            {
                writer.WriteNumber("cycle", cycle);
            }

            if (pose.PlaybackRate is { } rate)
            {
                writer.WriteNumber("playbackRate", rate);
            }

            if (pose.VehicleChanged)
            {
                writer.WriteNumber("vehicle", pose.Vehicle ?? -1);
                writer.WriteNumber("parent", pose.Parent ?? -1);
            }

            if (pose.Spawned)
            {
                writer.WriteBoolean("spawn", true);
            }

            if (pose.Gone)
            {
                writer.WriteBoolean("gone", true);
            }

            writer.WriteEndObject();
        }

        writer.WriteEndArray();

        if (track.Chat.Count > 0)
        {
            writer.WriteStartArray("chat");
            foreach (var line in track.Chat)
            {
                writer.WriteStartObject();
                writer.WriteNumber("t", line.T);
                writer.WriteNumber("time", Time(line.T));
                writer.WriteString("text", line.Text);
                WriteOptionalInt(writer, "senderEntity", line.SenderEntity);
                if (line.TeamOnly)
                {
                    writer.WriteBoolean("team", true);
                }
                writer.WriteEndObject();
            }

            writer.WriteEndArray();
        }

        writer.WriteEndObject();
    }

    private void WriteObject(Utf8JsonWriter writer, ObjectRow row)
    {
        writer.WriteStartObject();
        writer.WriteNumber("t", row.T);
        writer.WriteNumber("time", Time(row.T));
        writer.WriteString("type", row.Type);
        if (row.Kind is { Length: > 0 })
        {
            writer.WriteString("kind", row.Kind);
        }

        WriteOptionalInt(writer, "p", row.P);
        WriteOptionalInt(writer, "e", row.Entity);
        if (row.ClassName is { Length: > 0 })
        {
            writer.WriteString("class", row.ClassName);
        }

        if (row.Type != "game" && row.Name is { Length: > 0 })
        {
            writer.WriteString("name", row.Name);
        }

        if (row.Type == "spawn")
        {
            if (_precache.NetworkName(row.RealClassIndex) is { Length: > 0 } luaClass)
            {
                writer.WriteString("luaClass", luaClass);
            }

            WriteModel(writer, row.ModelIndex);
            if (row.Skin is > 0)
            {
                writer.WriteNumber("skin", row.Skin.Value);
            }

            if (row.Body is > 0)
            {
                writer.WriteNumber("body", row.Body.Value);
            }
            WriteOptionalInt(writer, "owner", row.Owner);
            WriteOptionalInt(writer, "parent", row.Parent);
            WriteOptionalInt(writer, "creationId", row.CreationId);
            if (row.MapCreatedId is > 0)
            {
                writer.WriteNumber("mapCreatedId", row.MapCreatedId.Value);
                writer.WriteBoolean("mapEntity", true);
            }

            if (row.CreationTime is { } created)
            {
                writer.WriteNumber("created", created);
            }

            if (row.ModelScale is { } scale && Math.Abs(scale - 1f) > 0.001f)
            {
                writer.WriteNumber("scale", scale);
            }

            JsonExporter.WriteGmodString(writer, "nw0", row.GmodString0);
            JsonExporter.WriteGmodString(writer, "nw1", row.GmodString1);
            JsonExporter.WriteGmodString(writer, "nw2", row.GmodString2);
            JsonExporter.WriteGmodString(writer, "nw3", row.GmodString3);
        }

        if(row.Type=="animation") {
            WriteOptionalInt(writer,"sequence",row.Sequence);
            if(row.Cycle is {} cycle) writer.WriteNumber("cycle",cycle);
            if(row.PlaybackRate is {} rate) writer.WriteNumber("playbackRate",rate);
        }
        WriteVec(writer, row.Origin);
        WriteAngles(writer, row.Angles);
        if (row.Type == "owner")
        {
            WriteOptionalInt(writer, "owner", row.Owner);
        }
        if (EntityIdentity.DoorStateName(row.DoorState) is { } doorState)
        {
            writer.WriteString("state", doorState);
        }

        if (EntityIdentity.DoorIsOpen(row.DoorState) is { } doorOpen)
        {
            writer.WriteBoolean("open", doorOpen);
        }

        if (row.Type == "visibility" || (row.Type == "door" && row.Hidden))
        {
            writer.WriteBoolean("hidden", row.Hidden);
        }

        if (row.Deleted)
        {
            writer.WriteBoolean("deleted", true);
        }

        if (row.GameId is { } gameId)
        {
            writer.WriteNumber("id", gameId);
            if (row.Name is { Length: > 0 } && row.Type == "game")
            {
                writer.WriteString("event", row.Name);
            }

            if (row.GameKeys is { Count: > 0 })
            {
                writer.WriteStartObject("keys");
                foreach (var (key, value) in row.GameKeys)
                {
                    switch (value)
                    {
                        case string s:
                            writer.WriteString(key, s);
                            break;
                        case int i:
                            writer.WriteNumber(key, i);
                            break;
                        case float f:
                            writer.WriteNumber(key, f);
                            break;
                        case bool b:
                            writer.WriteBoolean(key, b);
                            break;
                        default:
                            writer.WriteNull(key);
                            break;
                    }
                }

                writer.WriteEndObject();
            }
        }

        writer.WriteEndObject();
    }

    private double Time(int tick) => _tickRate > 0 ? tick / _tickRate : 0;

    private void WriteModel(Utf8JsonWriter writer, int? modelIndex)
    {
        WriteOptionalInt(writer, "modelIndex", modelIndex);
        if (_precache.ModelPath(modelIndex) is { Length: > 0 } path)
        {
            writer.WriteString("model", path);
        }
    }

    private static void WriteVec(Utf8JsonWriter writer, Vec3? origin)
    {
        if (origin is not { } v)
        {
            return;
        }

        writer.WriteNumber("x", v.X);
        writer.WriteNumber("y", v.Y);
        writer.WriteNumber("z", v.Z);
    }

    private static void WriteAngles(Utf8JsonWriter writer, Vec3? angles)
    {
        if (angles is not { } v)
        {
            return;
        }

        writer.WriteNumber("pitch", v.X);
        writer.WriteNumber("yaw", v.Y);
        writer.WriteNumber("roll", v.Z);
    }

    private static void WriteOptionalInt(Utf8JsonWriter writer, string name, int? value)
    {
        if (value is { } v)
        {
            writer.WriteNumber(name, v);
        }
    }

    private static bool InWindow(int tick, int? fromTick, int? toTick) =>
        (fromTick is null || tick >= fromTick) && (toTick is null || tick <= toTick);

    private static IReadOnlyList<VoiceClip> ClipsFor(
        IReadOnlyList<VoiceClip>? clips,
        DemoPlayer player,
        int? fromTick,
        int? toTick)
    {
        if (clips is null || clips.Count == 0)
        {
            return [];
        }

        return clips.Where(c =>
            c.HasPlayerAssignment
            && (c.P is { } clipPlayerId
                ? clipPlayerId == player.Id
                : (c.SteamId64 is { Length: > 0 } && c.SteamId64 == player.SteamId64)
                  || (player.Slot >= 0 && c.Slot == player.Slot))
            && InWindow(c.StartTick, fromTick, toTick)).ToList();
    }

    private static bool IsBody(
        GameEvent gameEvent,
        Dictionary<int, int> entityToPlayer,
        Dictionary<int, PlayerTrack> tracks,
        out PlayerTrack track)
    {
        var entity = gameEvent.E;
        var playerId = gameEvent.P;
        track = null!;
        if (playerId is { } id
            && tracks.TryGetValue(id, out track!)
            && entity is { } body
            && track.Player.Entity == body)
        {
            return true;
        }

        if (!gameEvent.PlayerBindingKnown && entity is { } e
            && entityToPlayer.TryGetValue(e, out var fromEntity)
            && tracks.TryGetValue(fromEntity, out track!))
        {
            return true;
        }

        return false;
    }

    private sealed class PlayerTrack
    {
        public PlayerTrack(DemoPlayer player, IReadOnlyList<VoiceClip> voice, int? fromTick, int? toTick)
        {
            Player = player;
            Voice = voice;
            _fromTick = fromTick;
            _toTick = toTick;
        }

        public DemoPlayer Player { get; private set; }

        public IReadOnlyList<VoiceClip> Voice { get; }

        public List<PlayerPose> Ticks { get; } = [];

        public List<ChatRow> Chat { get; } = [];

        public void SetPlayer(DemoPlayer player) => Player = player;

        private readonly PlayerPose _state = new();
        private readonly int? _fromTick;
        private readonly int? _toTick;
        private int? _openTick;

        public void Apply(int tick, Action<PlayerPose> update)
        {
            if (_openTick is { } open && open != tick)
            {
                Emit(open);
            }

            _openTick = tick;
            update(_state);
        }

        public void Flush()
        {
            if (_openTick is { } open)
            {
                Emit(open);
                _openTick = null;
            }
        }

        private void Emit(int tick)
        {
            var snapshot = _state.Snapshot(tick);
            _state.Spawned = false;
            _state.Gone = false;
            if (InWindow(tick, _fromTick, _toTick))
            {
                if (!_vehicleWrittenInWindow && snapshot.VehicleKnown)
                {
                    snapshot.VehicleChanged = true;
                    _vehicleWrittenInWindow = true;
                }

                Ticks.Add(snapshot);
            }

            _state.VehicleChanged = false;
        }

        private bool _vehicleWrittenInWindow;
    }

    private sealed class PlayerPose
    {
        public int T;
        public Vec3? Origin;
        public Vec3? Angles;
        public string? Weapon;
        public int? Health;
        public int? LifeState;
        public int? Flags;
        public int? Team;
        public int? ModelIndex;
        public int? Skin;
        public int? Body;
        public int? Sequence;
        public float? Cycle;
        public float? PlaybackRate;
        public int? Vehicle;
        public int? Parent;
        public bool VehicleKnown;
        public bool VehicleChanged;
        public bool Spawned;
        public bool Gone;

        public PlayerPose Snapshot(int tick) => new()
        {
            T = tick,
            Origin = Origin,
            Angles = Angles,
            Weapon = Weapon,
            Health = Health,
            LifeState = LifeState,
            Flags = Flags,
            Team = Team,
            ModelIndex = ModelIndex,
            Skin = Skin,
            Body = Body,
            Sequence = Sequence,
            Cycle = Cycle,
            PlaybackRate = PlaybackRate,
            Vehicle = Vehicle,
            Parent = Parent,
            VehicleKnown = VehicleKnown,
            VehicleChanged = VehicleChanged,
            Spawned = Spawned,
            Gone = Gone,
        };
    }

    private sealed class ChatRow(int t, string text, int? senderEntity, bool teamOnly)
    {
        public int T { get; } = t;

        public string Text { get; } = text;

        public int? SenderEntity { get; } = senderEntity;

        public bool TeamOnly { get; } = teamOnly;
    }

    private static bool IsPlacedClass(string? className) =>
        className is "CPhysicsProp" or "CSENT_anim";

    private static bool IsBankShutter(ObjectRow row,PrecacheTables precache) =>
        row.ClassName=="CDynamicProp" && row.MapCreatedId>=1234 && row.ModelIndex is {} index
        && precache.ModelPath(index) is "models/perpheads/props/bankshutters.mdl" or "models/perpheads/props/bankshutters2.mdl";

    private static bool IsMapMover(ObjectRow row, PrecacheTables precache) =>
        IsBankShutter(row,precache) ||
        (row.ModelIndex is { } index && precache.ModelPath(index)?.StartsWith('*') == true)
        || (row.ClassName == "CDynamicProp" && row.Parent > 0 && row.MapCreatedId >= 1234);

    private sealed class ObjectRow
    {
        public int? Sequence { get; set; }
        public float? Cycle { get; set; }
        public float? PlaybackRate { get; set; }
        public int T { get; set; }

        public string Type { get; set; } = "";

        public string? Kind { get; set; }

        public int? P { get; set; }

        public int? Entity { get; set; }

        public string? ClassName { get; set; }

        public string? Name { get; set; }

        public Vec3? Origin { get; set; }

        public Vec3? Angles { get; set; }

        public int? Skin { get; set; }

        public int? Body { get; set; }

        public int? Owner { get; set; }

        public int? Parent { get; set; }

        public int? ModelIndex { get; set; }

        public int? CreationId { get; set; }

        public int? MapCreatedId { get; set; }

        public float? CreationTime { get; set; }

        public float? ModelScale { get; set; }

        public int? RealClassIndex { get; set; }

        public string? GmodString0 { get; set; }

        public string? GmodString1 { get; set; }

        public string? GmodString2 { get; set; }

        public string? GmodString3 { get; set; }

        public int? DoorState { get; set; }

        public bool Hidden { get; set; }

        public bool Deleted { get; set; }

        public bool IsVehicle { get; set; }

        public bool IsWeapon { get; set; }

        public bool IsDoor { get; set; }

        public int? GameId { get; set; }

        public IReadOnlyDictionary<string, object?>? GameKeys { get; set; }

        public static ObjectRow FromSpawn(SpawnEvent spawn) => new()
        {
            T = spawn.T,
            Type = "spawn",
            Kind = spawn.IsVehicle ? "vehicle" : spawn.IsDoor ? "door" : spawn.IsWeapon ? "weapon" : spawn.IsPlayer ? "player" : "entity",
            P = spawn.P,
            Entity = spawn.E,
            ClassName = spawn.ClassName,
            Name = spawn.Name,
            Origin = spawn.Origin,
            Angles = spawn.Angles,
            Skin = spawn.Skin,
            Body = spawn.Body,
            Owner = spawn.Owner,
            Parent = spawn.Parent,
            ModelIndex = spawn.ModelIndex,
            CreationId = spawn.CreationId,
            MapCreatedId = spawn.MapCreatedId,
            CreationTime = spawn.CreationTime,
            ModelScale = spawn.ModelScale,
            RealClassIndex = spawn.RealClassIndex,
            GmodString0 = spawn.GmodString0,
            GmodString1 = spawn.GmodString1,
            GmodString2 = spawn.GmodString2,
            GmodString3 = spawn.GmodString3,
            IsVehicle = spawn.IsVehicle,
            IsWeapon = spawn.IsWeapon,
            IsDoor = spawn.IsDoor,
        };
    }
}
