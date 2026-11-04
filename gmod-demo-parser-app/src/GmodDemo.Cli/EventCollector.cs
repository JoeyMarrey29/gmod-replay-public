using System.Buffers.Binary;

namespace GmodDemo.Parser;

/// <summary>
/// Compact gameplay events derived from already-decoded fields. Anything still behind an
/// undecoded bitstream is omitted rather than guessed.
/// </summary>
public abstract record GameEvent(int T, string Type, int? P = null, int? E = null)
{
    public bool PlayerBindingKnown { get; init; }
}

public sealed record CameraMoveEvent(int T, Vec3 Origin, Vec3 Angles, int? P = null)
    : GameEvent(T, "camera", P);

public sealed record ChatEvent(int T, string Text, int? P = null, int? SenderEntity = null, int? UserId = null, bool TeamOnly = false)
    : GameEvent(T, "chat", P);

public sealed record ConsoleEvent(int T, string Command)
    : GameEvent(T, "console");

public sealed record MoveEvent(int T, int Entity, Vec3 Origin, int? P = null)
    : GameEvent(T, "move", P, Entity);

public sealed record LookEvent(int T, int Entity, Vec3 Angles, int? P = null)
    : GameEvent(T, "look", P, Entity);

public sealed record AnimEvent(int T, int Entity, int? Sequence, float? Cycle, float? PlaybackRate, int? P = null)
    : GameEvent(T, "anim", P, Entity);

public sealed record StateEvent(
    int T,
    int Entity,
    int? Health,
    int? LifeState,
    int? Flags,
    int? Team,
    int? ModelIndex,
    int? Skin,
    int? Body,
    int? P = null) : GameEvent(T, "state", P, Entity);

public sealed record SpawnEvent(
    int T,
    int Entity,
    string ClassName,
    bool IsPlayer,
    int? P = null,
    string? Name = null,
    int? Owner = null,
    bool IsWeapon = false,
    bool IsDoor = false,
    bool IsVehicle = false,
    int? ModelIndex = null,
    int? CreationId = null,
    int? MapCreatedId = null,
    float? CreationTime = null,
    float? ModelScale = null,
    int? RealClassIndex = null,
    int? Parent = null,
    string? GmodString0 = null,
    string? GmodString1 = null,
    string? GmodString2 = null,
    string? GmodString3 = null,
    Vec3? Origin = null,
    int? Health = null,
    string? HeldWeapon = null,
    Vec3? Angles = null,
    int? Skin = null,
    int? Body = null) : GameEvent(T, "spawn", P, Entity);

public sealed record RemoveEvent(int T, int Entity, bool Deleted, int? P = null)
    : GameEvent(T, "remove", P, Entity);

public sealed record WeaponEvent(int T, int Entity, int? Weapon, string? Name, int? P = null)
    : GameEvent(T, "weapon", P, Entity);

public sealed record OwnerEvent(int T, int Entity, int? Owner, string? Name, int? P = null)
    : GameEvent(T, "owner", P, Entity);

public sealed record PlayerVehicleEvent(int T, int Entity, int? Vehicle, int? Parent, int? P = null)
    : GameEvent(T, "playerVehicle", P, Entity);

public sealed record DoorEvent(int T, int Entity, Vec3? Origin, int? DoorState, int? P = null, bool Hidden = false)
    : GameEvent(T, "door", P, Entity);

public sealed record VisibilityEvent(int T, int Entity, bool Hidden)
    : GameEvent(T, "visibility", null, Entity);

public sealed record SourceGameEvent(
    int T,
    int Id,
    string? Name,
    IReadOnlyDictionary<string, object?> Keys,
    int? P = null,
    int? E = null) : GameEvent(T, "game", P, E);

public sealed record VoiceEvent(int T, int Slot, bool Proximity, int Bytes, int? P = null)
    : GameEvent(T, "voice", P);

/// <summary>
/// Collects events while walking demo packets.
/// </summary>
/// <remarks>
/// Chat in the JSON is engine <c>player_say</c> only
/// (<see href="https://wiki.facepunch.com/gmod/gameevent/player_say">wiki</see>).
/// Custom gamemode chat is Lua <c>net</c> and goes in the net log, not here.
/// </remarks>
public sealed class EventCollector
{
    public EventCollector() { _entities.Fires.ResolveClass = _precache.NetworkName; }

    private readonly NetMessageWalker _walker = new();
    private readonly NetMessageLog _netLog = new();
    private readonly PerpheadsMessageDecoder _perpheads = new();
    private readonly List<GameEvent> _events = [];
    private readonly List<PacketCommand> _pendingPackets = [];
    private readonly Dictionary<int, string> _entityClass = [];
    private readonly Dictionary<int, int?> _lastHeldWeapon = [];
    private readonly EntityWorld _entities = new();
    private readonly VoiceTrack _voice = new();
    private readonly PrecacheTables _precache = new();
    private readonly List<SoundRow> _sounds = [];
    private readonly Dictionary<int, List<(int Tick, int? PlayerId)>> _slotHistory = [];
    public TempEntityDecoder TempEntities { get; } = new();
    private readonly Gluapack _gluapack = new();
    private GameEventList? _gameEvents;
    private Vec3? _lastOrigin;
    private Vec3? _lastAngles;
    private string? _recorderName;
    private (int Entity, int PlayerId)? _recorderBodyId;
    private bool _tablesLoaded;

    public IReadOnlyList<GameEvent> Events => _events;

    internal void AddEvent(GameEvent gameEvent) => _events.Add(gameEvent);

    /// <summary>Every sound the server started, changed or stopped (svc_Sounds), in order.</summary>
    public IReadOnlyList<SoundRow> Sounds => _sounds;

    public IReadOnlyList<DemoPlayer> Players { get; private set; } = [];

    public EntityWorld Entities => _entities;

    public EntityDecodeStats EntityStats => _entities.Stats;

    public VoiceTrack Voice => _voice;

    public PrecacheTables Precache => _precache;

    public Gluapack Gluapack => _gluapack;

    public NetMessageLog NetLog => _netLog;

    /// <summary>Structured Perpheads messages: networked variables and clothing data.</summary>
    public PerpheadsMessageDecoder Perpheads => _perpheads;

    public GameEventList? GameEvents => _gameEvents;

    public void SetRecorderName(string? name) => _recorderName = name;

    public void AddDataTables(DataTablesCommand command)
    {
        _entities.LoadTables(command.Payload);
        _tablesLoaded = _entities.Tables is { Overflowed: false, Classes.Count: > 0 };
        if (!_tablesLoaded)
        {
            return;
        }

        _entities.Calibrate(_pendingPackets
            .Select(p => (
                p.Tick,
                p.Payload,
                p.Type == DemoCommandType.Packet ? (Vec3?)p.Info.EffectiveViewOrigin : null))
            .ToList());

        foreach (var packet in _pendingPackets)
        {
            AddEntityPacket(packet);
        }

        _pendingPackets.Clear();
    }

    public void AddConsole(ConsoleCommand command)
    {
        var text = command.Command.Trim();
        if (text.Length == 0)
        {
            return;
        }

        _netLog.AddConsole(command.Tick, text);
        if (IsNoise(text))
        {
            return;
        }

        _events.Add(new ConsoleEvent(command.Tick, text));
    }

    public void AddPacket(PacketCommand packet)
    {
        if (packet.Type == DemoCommandType.Packet)
        {
            MaybeAddCamera(packet);
        }

        if (packet.Type is DemoCommandType.Packet or DemoCommandType.Signon)
        {
            AddNetMessages(packet);
            _precache.AddPayload(packet.Payload);
            if (packet.Type == DemoCommandType.Signon)
            {
                _gameEvents ??= GameEventList.TryFind(packet.Payload);
                _voice.TryReadInitFromPayload(packet.Payload);
            }

            if (_tablesLoaded)
            {
                AddEntityPacket(packet);
            }
            else
            {
                _pendingPackets.Add(packet);
            }
        }
    }

    /// <summary>
    /// Fills compact player indexes on events that can be attributed. Camera events belong to
    /// the recorder (matched by demo header client name). Entity events use the player slot
    /// recovered from the packed userinfo run (entity = slot + 1), after that run has been
    /// checked against the recorder's entity. Player edicts in 1..128 with no GUID still get
    /// a compact id: Source allocates those edicts as slot + 1 even when userinfo has no
    /// <c>STEAM_</c> blob (bots, SourceTV, or a GUID the scan missed).
    /// </summary>
    public IReadOnlyList<DemoPlayer> BindPlayers(IReadOnlyList<DemoPlayer> players)
    {
        _slotHistory.Clear();
        var ordered = _events.OrderBy(e => e.T)
            .ThenBy(e => e is SourceGameEvent game && IsPlayerConnect(game.Name) ? -1
                : e is SourceGameEvent { Name: GmodGameEvents.PlayerDisconnect } ? 1 : 0)
            .ToList();
        _events.Clear();
        _events.AddRange(ordered);
        int? recorderId = null;
        DemoPlayer? recorder = null;
        if (_recorderName is { Length: > 0 } name)
        {
            recorder = players.FirstOrDefault(p =>
                string.Equals(p.Name, name, StringComparison.Ordinal));
            recorderId = recorder?.Id;
        }

        // Entity snapshots describe the final occupant, not the occupant at each event tick.
        // Resolve player entities against the connection lifecycle below instead of matching
        // a final snapshot by name or user ID.
        var slotted = players.ToList();
        // The slot table is only trusted when it puts the recorder on their own body. The
        // local-player tables identify that body exactly; the camera match is only a fallback
        // (it can land on someone standing next to the recorder or sharing their car).
        if (recorder is { Slot: >= 0 } recPlayer
            && (_entities.LocalPlayerEntity ?? _entities.RecorderEntity) is { } recEntity
            && recPlayer.Entity != recEntity)
        {
            slotted = players.Select(p => p with { Slot = -1 }).ToList();
        }

        slotted = AppendSlotPlaceholders(slotted, _entities.LocalPlayerEntity ?? _entities.RecorderEntity);

        // The local-player tables identify the recorder's body better than the camera match.
        var recorderBody = _entities.LocalPlayerEntity ?? _entities.RecorderEntity;
        _recorderBodyId = null;
        if (recorderId is { } rec && recorderBody is { } recorderEntity)
        {
            slotted = slotted
                .Select(player => player.Id == rec
                    ? player with { RawEntity = recorderEntity, Slot = -1 }
                    : player.Entity == recorderEntity && player.Slot >= 0 && player.Name is not { Length: > 0 }
                        ? player with { Slot = -1, RawEntity = -1 }
                        : player)
                .ToList();
            _entities.BindPlayer(recorderEntity, rec);
            _recorderBodyId = (recorderEntity, rec);
        }

        var owners = new Dictionary<int, int?>();
        var activeBySlot = new Dictionary<int, int>();
        var activeByUserId = new Dictionary<int, int>();
        var managedSlots = new HashSet<int>();
        var joiningUsers = _events.OfType<SourceGameEvent>()
            .Where(e => e.T > 0 && IsPlayerConnect(e.Name))
            .Select(e => GmodGameEvents.UserId(e.Keys))
            .OfType<int>().ToHashSet();
        foreach (var player in slotted)
        {
            if (player.Entity is >= 1 and <= 128)
            {
                var slot = player.Entity - 1;
                if (managedSlots.Add(slot))
                {
                    RecordSlot(slot, int.MinValue, null);
                }
                if (player.UserId is not { } initialUser || !joiningUsers.Contains(initialUser))
                {
                    activeBySlot[slot] = player.Id;
                    RecordSlot(slot, int.MinValue, player.Id);
                }
            }

            if (player.UserId is { } userId && !joiningUsers.Contains(userId))
            {
                activeByUserId[userId] = player.Id;
            }

        }

        int? PlayerIdAt(int? entity)
        {
            if (entity is not { } index)
            {
                return null;
            }

            if (_recorderBodyId is { } body && body.Entity == index)
            {
                return body.PlayerId;
            }

            var slot = index - 1;
            if (managedSlots.Contains(slot))
            {
                return activeBySlot.TryGetValue(slot, out var activePlayer) ? activePlayer : null;
            }

            return null;
        }

        int? ActorIdAt(int entity, int? knownOwner)
        {
            if (PlayerIdAt(entity) is { } direct)
            {
                return direct;
            }

            if (PlayerIdAt(knownOwner) is { } fromKnown)
            {
                return fromKnown;
            }

            if (owners.TryGetValue(entity, out var mapped) && PlayerIdAt(mapped) is { } fromMap)
            {
                return fromMap;
            }

            return null;
        }

        int? ResolveUser(int? userId) =>
            userId is { } id && activeByUserId.TryGetValue(id, out var activePlayer) ? activePlayer : null;

        for (var i = 0; i < _events.Count; i++)
        {
            if (_events[i] is SourceGameEvent connect && IsPlayerConnect(connect.Name))
            {
                var userId = GmodGameEvents.UserId(connect.Keys);
                var slot = GmodGameEvents.Int(connect.Keys, "index");
                var steamId = StringKey(connect.Keys, "networkid");
                var connectionName = StringKey(connect.Keys, "name");
                var playerId = FindOrAddConnectedPlayer(slotted, activeByUserId, steamId, connectionName, userId, slot);
                if (slot is { } connectedSlot && playerId is { } connectedPlayerId)
                {
                    var connectedIndex = slotted.FindIndex(p => p.Id == connectedPlayerId);
                    slotted[connectedIndex] = slotted[connectedIndex] with
                    {
                        ConnectedTick = slotted[connectedIndex].ConnectedTick ?? connect.T,
                    };
                    managedSlots.Add(connectedSlot);
                    if (activeBySlot.TryGetValue(connectedSlot, out var displacedPlayerId)
                        && displacedPlayerId != connectedPlayerId)
                    {
                        EndSession(displacedPlayerId, connect.T);
                        foreach (var staleUserId in activeByUserId
                                     .Where(pair => pair.Value == displacedPlayerId)
                                     .Select(pair => pair.Key).ToList())
                        {
                            activeByUserId.Remove(staleUserId);
                        }
                    }

                    foreach (var oldSlot in activeBySlot
                                 .Where(pair => pair.Value == connectedPlayerId && pair.Key != connectedSlot)
                                 .Select(pair => pair.Key).ToList())
                    {
                        activeBySlot.Remove(oldSlot);
                        RecordSlot(oldSlot, connect.T, null);
                    }

                    activeBySlot[connectedSlot] = connectedPlayerId;
                    RecordSlot(connectedSlot, connect.T, connectedPlayerId);
                }

                if (userId is { } connectedUserId && playerId is { } connectedId)
                {
                    foreach (var oldUserId in activeByUserId
                                 .Where(pair => pair.Value == connectedId && pair.Key != connectedUserId)
                                 .Select(pair => pair.Key).ToList())
                    {
                        activeByUserId.Remove(oldUserId);
                    }

                    activeByUserId[connectedUserId] = connectedId;
                }
            }

            switch (_events[i])
            {
                case SpawnEvent spawn:
                    owners[spawn.Entity] = spawn.Owner;
                    _events[i] = spawn with
                    {
                        P = spawn.IsPlayer
                            ? PlayerIdAt(spawn.Entity)
                            : ActorIdAt(spawn.Entity, spawn.Owner),
                    };
                    break;
                case OwnerEvent owner:
                    owners[owner.Entity] = owner.Owner;
                    _events[i] = owner with
                    {
                        P = ActorIdAt(owner.Entity, owner.Owner),
                        Name = owner.Name ?? NameOf(owner.Entity),
                    };
                    break;
                case CameraMoveEvent camera:
                    _events[i] = camera with { P = recorderId };
                    break;
                case MoveEvent move:
                    _events[i] = move with { P = ActorIdAt(move.Entity, null) };
                    break;
                case LookEvent look:
                    _events[i] = look with { P = ActorIdAt(look.Entity, null) };
                    break;
                case AnimEvent anim:
                    _events[i] = anim with { P = ActorIdAt(anim.Entity, null) };
                    break;
                case StateEvent state:
                    _events[i] = state with { P = ActorIdAt(state.Entity, null) };
                    break;
                case RemoveEvent remove:
                    _events[i] = remove with { P = ActorIdAt(remove.Entity, null) };
                    break;
                case WeaponEvent weapon:
                    _events[i] = weapon with
                    {
                        P = PlayerIdAt(weapon.Entity),
                        Name = weapon.Name ?? NameOf(weapon.Weapon),
                    };
                    break;
                case PlayerVehicleEvent vehicle:
                    _events[i] = vehicle with { P = PlayerIdAt(vehicle.Entity) };
                    break;
                case DoorEvent door:
                    _events[i] = door with { P = ActorIdAt(door.Entity, null) };
                    break;
                case ChatEvent chat:
                    _events[i] = chat with
                    {
                        P = chat.UserId is { } chatUserId
                            ? ResolveUser(chatUserId)
                            : PlayerIdAt(chat.SenderEntity) ?? chat.P,
                        SenderEntity = chat.SenderEntity
                            ?? (chat.UserId is { } senderUserId && ResolveUser(senderUserId) is { } chatPlayer
                                ? slotted.First(pl => pl.Id == chatPlayer).Entity
                                : null),
                    };
                    break;
                case SourceGameEvent game:
                {
                    var userId = GmodGameEvents.UserId(game.Keys);
                    var entity = GmodGameEvents.EntityFromKeys(game.Name, game.Keys) ?? game.E;
                    _events[i] = game with
                    {
                        P = userId is { } gameUserId
                            ? ResolveUser(gameUserId)
                            : PlayerIdAt(entity) ?? game.P,
                        E = entity,
                    };
                    if (string.Equals(game.Name, GmodGameEvents.PlayerDisconnect, StringComparison.Ordinal)
                        && userId is { } disconnectedUserId
                        && activeByUserId.Remove(disconnectedUserId, out var disconnectedPlayerId))
                    {
                        EndSession(disconnectedPlayerId, game.T);
                        var disconnectedPlayer = slotted.FirstOrDefault(player => player.Id == disconnectedPlayerId);
                        var disconnectedSlot = GmodGameEvents.Int(game.Keys, "index")
                            ?? (disconnectedPlayer is { Entity: > 0 } ? disconnectedPlayer.Entity - 1 : null);
                        if (disconnectedSlot is { } slot
                            && activeBySlot.TryGetValue(slot, out var occupant)
                            && occupant == disconnectedPlayerId)
                        {
                            activeBySlot.Remove(slot);
                            RecordSlot(slot, game.T, null);
                        }
                    }

                    break;
                }
                case VoiceEvent voice:
                    _events[i] = voice with
                    {
                        P = activeBySlot.TryGetValue(voice.Slot, out var voicePlayer) ? voicePlayer : null,
                    };
                    break;
            }

            _events[i] = _events[i] with { PlayerBindingKnown = true };
        }

        Players = slotted;
        return slotted;

        void RecordSlot(int slot, int tick, int? playerId)
        {
            if (!_slotHistory.TryGetValue(slot, out var history))
            {
                history = [];
                _slotHistory[slot] = history;
            }

            history.Add((tick, playerId));
        }

        void EndSession(int playerId, int tick)
        {
            var index = slotted.FindIndex(p => p.Id == playerId);
            slotted[index] = slotted[index] with { DisconnectedTick = tick };
        }
    }

    public DemoPlayer? PlayerAtSlot(int slot, int tick)
    {
        if (!_slotHistory.TryGetValue(slot, out var history))
        {
            return Players.FirstOrDefault(player => player.Slot == slot);
        }

        int? playerId = null;
        foreach (var change in history)
        {
            if (change.Tick > tick)
            {
                break;
            }

            playerId = change.PlayerId;
        }

        return playerId is { } id ? Players.FirstOrDefault(player => player.Id == id) : null;
    }

    private static bool IsPlayerConnect(string? name) =>
        string.Equals(name, GmodGameEvents.PlayerConnect, StringComparison.Ordinal)
        || string.Equals(name, GmodGameEvents.PlayerConnectClient, StringComparison.Ordinal);

    private static string? StringKey(IReadOnlyDictionary<string, object?> keys, string key) =>
        keys.TryGetValue(key, out var value) ? value as string : null;

    private static int? FindOrAddConnectedPlayer(
        List<DemoPlayer> players,
        Dictionary<int, int> activeByUserId,
        string? steamId,
        string? name,
        int? userId,
        int? slot)
    {
        DemoPlayer? player = null;
        if (userId is { } user && activeByUserId.TryGetValue(user, out var activeId))
        {
            player = players.FirstOrDefault(candidate => candidate.Id == activeId);
            if (player is not null
                && steamId is { Length: > 0 }
                && player.SteamId is { Length: > 0 }
                && !string.Equals(player.SteamId, steamId, StringComparison.Ordinal))
            {
                player = null;
            }
        }

        if (player is null && userId is { } id)
        {
            var sameSession = players.FirstOrDefault(candidate => candidate.UserId == id
                && candidate.DisconnectedTick is null);
            if (sameSession is not null
                && (steamId is not { Length: > 0 }
                    || sameSession.SteamId is not { Length: > 0 }
                    || string.Equals(sameSession.SteamId, steamId, StringComparison.Ordinal)))
            {
                player = sameSession;
            }
        }

        if (player is null)
        {
            player = new DemoPlayer(
                players.Count == 0 ? 0 : players.Max(candidate => candidate.Id) + 1,
                slot ?? -1,
                userId,
                name,
                steamId,
                steamId is { Length: > 0 } steamFor64 ? PlayerDirectory.ToSteamId64(steamFor64) : null,
                FakePlayer: false,
                IsHltv: false);
            players.Add(player);
        }
        else
        {
            var updated = player with
            {
                Slot = slot ?? player.Slot,
                UserId = userId ?? player.UserId,
                Name = name ?? player.Name,
                SteamId = steamId ?? player.SteamId,
                SteamId64 = steamId is { Length: > 0 } connectedSteamId
                    ? PlayerDirectory.ToSteamId64(connectedSteamId) ?? player.SteamId64
                    : player.SteamId64,
            };
            var index = players.FindIndex(candidate => candidate.Id == player.Id);
            players[index] = updated;
            player = updated;
        }

        return player.Id;
    }

    private int? ActorId(int entity, int? knownOwner, IReadOnlyList<DemoPlayer> players, Dictionary<int, int?> owners)
    {
        if (PlayerId(entity, players) is { } direct)
        {
            return direct;
        }

        if (PlayerId(knownOwner, players) is { } fromKnown)
        {
            return fromKnown;
        }

        if (owners.TryGetValue(entity, out var mapped) && PlayerId(mapped, players) is { } fromMap)
        {
            return fromMap;
        }

        var snap = _entities.Snapshot(entity);
        return PlayerId(snap?.OwnerEntity ?? snap?.Driver, players);
    }

    private List<DemoPlayer> AppendSlotPlaceholders(List<DemoPlayer> players, int? recorderBody)
    {
        var occupied = players.Where(p => p.Slot >= 0).Select(p => p.Slot).ToHashSet();
        // The recorder's own body is already a player; it never gets a nameless twin.
        if (recorderBody is { } body)
        {
            occupied.Add(body - 1);
        }
        var nextId = players.Count == 0 ? 0 : players.Max(p => p.Id) + 1;
        foreach (var entity in _events.OfType<SpawnEvent>()
                     .Where(s => s.IsPlayer && s.Entity is >= 1 and <= 128)
                     .Select(s => s.Entity)
                     .Distinct()
                     .OrderBy(e => e))
        {
            var slot = entity - 1;
            if (occupied.Contains(slot))
            {
                continue;
            }

            occupied.Add(slot);
            var snap = _entities.Snapshot(entity);
            players.Add(new DemoPlayer(
                Id: nextId++,
                Slot: slot,
                UserId: snap?.UserId,
                Name: null,
                SteamId: null,
                SteamId64: null,
                FakePlayer: false,
                IsHltv: false));
        }

        return players;
    }

    private int? PlayerId(int entity, IReadOnlyList<DemoPlayer> players)
    {
        if (_recorderBodyId is { } body && body.Entity == entity)
        {
            return body.PlayerId;
        }

        var snap = _entities.Snapshot(entity);
        if (snap?.UserId is { } userId)
        {
            var byUser = players.FirstOrDefault(p => p.UserId == userId);
            if (byUser is not null)
            {
                _entities.BindPlayer(entity, byUser.Id);
                return byUser.Id;
            }
        }

        if (_entities.PlayerIdFor(entity) is { } bound)
        {
            return bound;
        }

        var player = players.FirstOrDefault(p => p.Slot >= 0 && p.Entity == entity);
        if (player is not null)
        {
            _entities.BindPlayer(entity, player.Id);
            return player.Id;
        }

        return null;
    }

    private string? NameOf(int? entity) =>
        entity is { } index ? _entities.NameOfEntity(index) : null;

    private int? PlayerId(int? entity, IReadOnlyList<DemoPlayer> players) =>
        entity is { } index ? PlayerId(index, players) : null;

    private void AddEntityPacket(PacketCommand packet)
    {
        _entities.InstanceBaselines = _precache.InstanceBaselines;
        Vec3? camera = packet.Type == DemoCommandType.Packet
            ? packet.Info.EffectiveViewOrigin
            : null;
        var changes = _entities.ApplyPacket(packet.Tick, packet.Payload, camera);
        foreach (var change in changes)
        {
            switch (change)
            {
                case EntitySpawnChange spawn:
                    _entityClass[spawn.Entity] = spawn.ClassName;
                    var snap = _entities.Snapshot(spawn.Entity);
                    _events.Add(new SpawnEvent(
                        spawn.Tick,
                        spawn.Entity,
                        spawn.ClassName,
                        spawn.IsPlayer,
                        Name: spawn.Name,
                        Owner: spawn.OwnerEntity ?? snap?.OwnerEntity,
                        IsWeapon: spawn.IsWeapon,
                        IsDoor: spawn.IsDoor,
                        IsVehicle: spawn.IsVehicle,
                        ModelIndex: snap?.ModelIndex,
                        CreationId: snap?.CreationId,
                        MapCreatedId: snap?.MapCreatedId,
                        CreationTime: snap?.CreationTime,
                        ModelScale: snap?.ModelScale,
                        RealClassIndex: snap?.RealClassIndex,
                        Parent: snap?.Parent,
                        GmodString0: snap?.GmodString0,
                        GmodString1: snap?.GmodString1,
                        GmodString2: snap?.GmodString2,
                        GmodString3: snap?.GmodString3,
                        Origin: snap?.Origin,
                        Health: snap?.Health,
                        HeldWeapon: spawn.IsPlayer ? NameOf(snap?.ActiveWeapon) : null,
                        Angles: snap?.Angles,
                        Skin: snap?.Skin,
                        Body: snap?.Body));
                    break;
                case EntityRemoveChange remove:
                    _entityClass.Remove(remove.Entity);
                    _lastHeldWeapon.Remove(remove.Entity);
                    _events.Add(new RemoveEvent(remove.Tick, remove.Entity, remove.Deleted));
                    break;
                case EntityOriginChange origin:
                    _events.Add(new MoveEvent(origin.Tick, origin.Entity, origin.Origin));
                    break;
                case EntityEyeChange eye:
                    _events.Add(new LookEvent(eye.Tick, eye.Entity, eye.EyeAngles));
                    break;
                case EntityAnglesChange angles:
                    if (!string.Equals(
                            _entityClass.GetValueOrDefault(angles.Entity),
                            "player",
                            StringComparison.OrdinalIgnoreCase))
                    {
                        _events.Add(new LookEvent(angles.Tick, angles.Entity, angles.Angles));
                    }

                    break;
                case EntityAnimChange anim:
                    _events.Add(new AnimEvent(anim.Tick, anim.Entity, anim.Sequence, anim.Cycle, anim.PlaybackRate));
                    break;
                case EntityStateChange state:
                    _events.Add(new StateEvent(
                        state.Tick,
                        state.Entity,
                        state.Health,
                        state.LifeState,
                        state.Flags,
                        state.Team,
                        state.ModelIndex,
                        state.Skin,
                        state.Body));
                    break;
                case EntityWeaponChange weapon:
                    if (_lastHeldWeapon.TryGetValue(weapon.Entity, out var held) && held == weapon.Weapon)
                    {
                        break;
                    }

                    _lastHeldWeapon[weapon.Entity] = weapon.Weapon;
                    _events.Add(new WeaponEvent(weapon.Tick, weapon.Entity, weapon.Weapon, weapon.Name ?? NameOf(weapon.Weapon)));
                    break;
                case EntityOwnerChange owner:
                    _events.Add(new OwnerEvent(owner.Tick, owner.Entity, owner.Owner, owner.Name));
                    break;
                case EntityVehicleChange vehicle:
                    _events.Add(new PlayerVehicleEvent(
                        vehicle.Tick, vehicle.Entity, vehicle.Vehicle, vehicle.Parent));
                    break;
                case EntityDoorChange door:
                    _events.Add(new DoorEvent(door.Tick, door.Entity, door.Origin, door.DoorState, null, door.Hidden));
                    break;
                case EntityVisibilityChange visibility:
                    _events.Add(new VisibilityEvent(visibility.Tick,visibility.Entity,visibility.Hidden));
                    break;
            }
        }
    }

    private void MaybeAddCamera(PacketCommand packet)
    {
        var origin = packet.Info.EffectiveViewOrigin;
        var angles = packet.Info.EffectiveViewAngles;
        if (_lastOrigin is { } previousOrigin && _lastAngles is { } previousAngles
            && NearlyEqual(origin, previousOrigin) && NearlyEqual(angles, previousAngles))
        {
            return;
        }

        _lastOrigin = origin;
        _lastAngles = angles;
        _events.Add(new CameraMoveEvent(packet.Tick, origin, angles));
    }

    private int? PlayerIdBySlot(int slot, IReadOnlyList<DemoPlayer> players)
    {
        var player = players.FirstOrDefault(p => p.Slot == slot);
        return player?.Id ?? PlayerId(slot + 1, players);
    }

    private SourceGameEvent BindGameEvent(SourceGameEvent game, IReadOnlyList<DemoPlayer> players)
    {
        var userId = GmodGameEvents.UserId(game.Keys);
        var entity = GmodGameEvents.EntityFromKeys(game.Name, game.Keys) ?? game.E;
        var p = userId is { } id
            ? players.FirstOrDefault(pl => pl.UserId == id)?.Id ?? game.P
            : game.P;
        if (p is null && entity is { } ent)
        {
            p = PlayerId(ent, players);
        }

        return game with { P = p, E = entity };
    }

    private void AddNetMessages(PacketCommand packet)
    {
        var walk = _walker.Walk(packet.Payload);
        _precache.AddPacket(packet.Payload, walk.Messages, packet.Tick);
        foreach (var span in walk.Messages)
        {
            var reader = new BitReader(packet.Payload);
            reader.Seek(span.StartBit);
            reader.ReadUBitLong(NetMessageLayout.TypeBits);

            switch (span.Type)
            {
                case NetMessageType.NetStringCmd:
                {
                    var text = reader.ReadString();
                    if (text.Length > 0 && !reader.IsOverflowed)
                    {
                        _netLog.AddConsole(packet.Tick, text);
                    }

                    break;
                }

                case NetMessageType.SvcPrint:
                {
                    var text = reader.ReadString();
                    if (text.Length > 0 && !reader.IsOverflowed)
                    {
                        _netLog.Add(new NetLogRow { T = packet.Tick, Type = "print", Text = text });
                    }

                    break;
                }

                case NetMessageType.NetSetConVar:
                {
                    var count = reader.ReadByte();
                    for (var i = 0; i < count && !reader.IsOverflowed; i++)
                    {
                        var name = reader.ReadString();
                        var value = reader.ReadString();
                        _gluapack.ReadConVar(name, value);
                    }

                    break;
                }

                case NetMessageType.SvcVoiceInit:
                {
                    var codec = reader.ReadString(32);
                    var quality = reader.ReadByte();
                    var rate = quality == 255 ? (int)reader.ReadUBitLong(16) : 0;
                    if (!reader.IsOverflowed)
                    {
                        _voice.AddInit(codec, quality, rate);
                    }

                    break;
                }

                case NetMessageType.SvcVoiceData:
                {
                    var before = _voice.Packets.Count;
                    VoiceTrack.ReadVoiceData(ref reader, packet.Tick, _voice);
                    if (_voice.Packets.Count > before)
                    {
                        var spoken = _voice.Packets[^1];
                        _events.Add(new VoiceEvent(spoken.Tick, spoken.Slot, spoken.Proximity, spoken.Payload.Length));
                        _netLog.Add(new NetLogRow
                        {
                            T = packet.Tick,
                            Type = "voice",
                            Slot = spoken.Slot,
                            Bytes = spoken.Payload.Length,
                        });
                    }

                    break;
                }

                case NetMessageType.SvcSounds:
                {
                    var reliable = reader.ReadOneBit() != 0;
                    int soundBits;
                    var soundCount = 1;
                    if (reliable)
                    {
                        soundBits = (int)reader.ReadUBitLong(8);
                    }
                    else
                    {
                        soundCount = (int)reader.ReadUBitLong(8);
                        soundBits = (int)reader.ReadUBitLong(16);
                    }

                    _sounds.AddRange(SoundMessage.Read(ref reader, packet.Tick, soundCount, soundBits));
                    _netLog.Add(new NetLogRow
                    {
                        T = packet.Tick,
                        Type = "sound",
                        Bits = soundBits,
                        Keys = new Dictionary<string, object?> { ["reliable"] = reliable },
                    });
                    break;
                }

                case NetMessageType.SvcSetView:
                    _netLog.Add(new NetLogRow
                    {
                        T = packet.Tick,
                        Type = "setview",
                        Entity = (int)reader.ReadUBitLong(NetMessageLayout.MaxEdictBits),
                    });
                    break;

                case NetMessageType.SvcFixAngle:
                {
                    var relative = reader.ReadOneBit() != 0;
                    _netLog.Add(new NetLogRow
                    {
                        T = packet.Tick,
                        Type = "fixangle",
                        Keys = new Dictionary<string, object?> { ["relative"] = relative },
                    });
                    break;
                }

                case NetMessageType.SvcUserMessage:
                {
                    var id = (int)reader.ReadByte();
                    var lengthBits = (int)reader.ReadUBitLong(NetMessageLayout.NetMessageLengthBits);
                    var body = reader.ReadBitsAsBytes(Math.Min(lengthBits, reader.BitsLeft));
                    var (hex, ascii) = NetMessageLog.BodyFields(body);
                    _netLog.Add(new NetLogRow
                    {
                        T = packet.Tick,
                        Type = "usermessage",
                        Id = id,
                        Bits = lengthBits,
                        Hex = hex,
                        Ascii = ascii,
                    });
                    break;
                }

                case NetMessageType.SvcEntityMessage:
                {
                    var entity = (int)reader.ReadUBitLong(NetMessageLayout.MaxEdictBits);
                    reader.ReadUBitLong(NetMessageLayout.MaxServerClassBits);
                    var lengthBits = (int)reader.ReadUBitLong(NetMessageLayout.NetMessageLengthBits);
                    _netLog.Add(new NetLogRow
                    {
                        T = packet.Tick,
                        Type = "entitymessage",
                        Entity = entity,
                        Bits = lengthBits,
                    });
                    break;
                }

                case NetMessageType.SvcGameEvent:
                {
                    var lengthBits = (int)reader.ReadUBitLong(NetMessageLayout.NetMessageLengthBits);
                    if (_gameEvents is { } list)
                    {
                        var decoded = list.ReadEvent(ref reader, lengthBits);
                        if (decoded is not null)
                        {
                            _events.Add(new SourceGameEvent(packet.Tick, decoded.Id, decoded.Name, decoded.Keys));
                            _netLog.Add(new NetLogRow
                            {
                                T = packet.Tick,
                                Type = "game",
                                Id = decoded.Id,
                                Name = decoded.Name,
                                Keys = decoded.Keys,
                            });
                            if (decoded.Name == GmodGameEvents.PlayerSay
                                && GmodGameEvents.SayText(decoded.Keys) is { Length: > 0 } say)
                            {
                                _events.Add(new ChatEvent(
                                    packet.Tick,
                                    say,
                                    UserId: GmodGameEvents.UserId(decoded.Keys),
                                    TeamOnly: GmodGameEvents.TeamOnly(decoded.Keys)));
                            }
                        }
                    }
                    else
                    {
                        var id = (int)reader.ReadUBitLong(NetMessageLayout.MaxEventBits);
                        if (!reader.IsOverflowed)
                        {
                            _events.Add(new SourceGameEvent(packet.Tick, id, null, new Dictionary<string, object?>()));
                            _netLog.Add(new NetLogRow { T = packet.Tick, Type = "game", Id = id, Bits = lengthBits });
                        }
                    }

                    break;
                }

                case NetMessageType.SvcPacketEntities:
                {
                    reader.ReadUBitLong(NetMessageLayout.MaxEdictBits);
                    var isDelta = reader.ReadOneBit() != 0;
                    if (isDelta)
                    {
                        reader.ReadUBitLong(32);
                    }

                    reader.ReadUBitLong(1);
                    reader.ReadUBitLong(NetMessageLayout.MaxEdictBits);
                    var lengthBits = (int)reader.ReadUBitLong(NetMessageLayout.DeltaSizeBits);
                    _netLog.Add(new NetLogRow
                    {
                        T = packet.Tick,
                        Type = "entities",
                        Bits = lengthBits,
                        Delta = isDelta,
                    });
                    break;
                }

                case NetMessageType.SvcTempEntities:
                {
                    var start = reader.BitsRead;
                    var count = (int)reader.ReadUBitLong(8);
                    var lengthBits = (int)reader.ReadVarInt32();
                    _netLog.Add(new NetLogRow
                    {
                        T = packet.Tick,
                        Type = "tempentities",
                        Id = count,
                        Bits = lengthBits,
                    });
                    if (_entities.Tables is { } tables)
                    {
                        reader.Seek(start);
                        TempEntities.Read(ref reader, packet.Tick, tables);
                    }
                    break;
                }

                case NetMessageType.SvcGModServerToClient:
                {
                    var lengthBits = (int)reader.ReadUBitLong(NetMessageLayout.GModMessageLengthBits);
                    var subType = reader.ReadByte();
                    int? nameId = null;
                    int bodyBits;
                    byte[] body;
                    if (subType == 0 && !reader.IsOverflowed)
                    {
                        // wiki net.ReadHeader: unsigned short = util.AddNetworkString id
                        nameId = (int)reader.ReadWord();
                        bodyBits = lengthBits - 8 - 16;
                    }
                    else
                    {
                        bodyBits = lengthBits - 8;
                    }

                    if (bodyBits < 0 || bodyBits > reader.BitsLeft || reader.IsOverflowed)
                    {
                        reader.SetOverflow();
                        break;
                    }

                    body = reader.ReadBitsAsBytes(bodyBits);
                    var (hex, ascii) = NetMessageLog.BodyFields(body);
                    _perpheads.Process(
                        packet.Tick,
                        nameId is { } perpheadsId ? _precache.NetworkName(perpheadsId) : null,
                        body,
                        bodyBits);
                    var clothingKeys = DecodeBonemerge(
                        nameId is { } decodedNameId
                            ? _precache.NetworkName(decodedNameId)
                            : null,
                        body);
                    _netLog.Add(new NetLogRow
                    {
                        T = packet.Tick,
                        Type = "lua",
                        SubType = subType,
                        Id = nameId,
                        Name = nameId is { } id ? _precache.NetworkName(id) : null,
                        Bits = lengthBits,
                        Hex = hex,
                        Ascii = ascii,
                        Keys = clothingKeys,
                    });

                    break;
                }
            }
        }
    }

    private static IReadOnlyDictionary<string, object?>? DecodeBonemerge(
        string? name,
        byte[] body)
    {
        if (name is null
            || !name.StartsWith("perpheads_bonemerge", StringComparison.Ordinal)
            || body.Length < 4)
        {
            return null;
        }

        var keys = new Dictionary<string, object?>
        {
            ["entity"] = (int)BinaryPrimitives.ReadUInt16LittleEndian(body.AsSpan(0, 2)),
            ["entry"] = (int)BinaryPrimitives.ReadUInt16LittleEndian(body.AsSpan(2, 2)),
        };

        if (name.EndsWith("_skin", StringComparison.Ordinal)
                 && body.Length >= 5)
        {
            keys["skin"] = body[4].ToString();
        }
        else if (name.EndsWith("_visible", StringComparison.Ordinal)
                 && body.Length >= 5)
        {
            keys["visible"] = body[4] == 0 ? "0" : "1";
        }

        return keys;
    }

    private static bool IsNoise(string command)
    {
        // The engine writes this fade command on almost every tick; it is not player input.
        return command.StartsWith("soundfade ", StringComparison.OrdinalIgnoreCase)
            || command.Equals("gameui_preventescapetoshow", StringComparison.OrdinalIgnoreCase)
            || command.Equals("gameui_allowescapetoshow", StringComparison.OrdinalIgnoreCase);
    }

    private static bool NearlyEqual(Vec3 a, Vec3 b) =>
        Math.Abs(a.X - b.X) < 0.01f
        && Math.Abs(a.Y - b.Y) < 0.01f
        && Math.Abs(a.Z - b.Z) < 0.01f;
}
