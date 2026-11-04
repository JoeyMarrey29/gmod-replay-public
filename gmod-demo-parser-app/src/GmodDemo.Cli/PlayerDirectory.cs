using System.Buffers.Binary;
using System.Text;
using System.Text.RegularExpressions;

namespace GmodDemo.Parser;

public sealed record DemoPlayer(
    int Id,
    int Slot,
    int? UserId,
    string? Name,
    string? SteamId,
    string? SteamId64,
    bool FakePlayer,
    bool IsHltv,
    int? RawEntity = null)
{
    public int? ConnectedTick { get; init; }
    public int? DisconnectedTick { get; init; }

    /// <summary>Use decoded entity evidence when available; slot is only a fallback.</summary>
    public int Entity => RawEntity ?? (Slot >= 0 ? Slot + 1 : 0);
}

/// <summary>
/// Builds the player list from userinfo <c>player_info_t</c> blobs.
/// </summary>
/// <remarks>
/// Each connected client is a userinfo string-table entry whose index is the player slot and
/// whose user-data is a <c>player_info_t</c> (name, engine user ID, <c>STEAM_X:Y:Z</c> GUID).
/// SteamID64 is derived from that GUID. The engine's <c>dem_stringtables</c> userinfo dump
/// supplies explicit slots, including vacancies. When that table is unavailable, slots fall
/// back to the longest packed run of <c>player_info_t</c> blobs. That assignment is discarded if it
/// disagrees with the recorder entity (Source: player entity index = slot + 1). Identity is
/// also recovered by locating the GUID field directly so truncated tables still list who was
/// in the session.
/// </remarks>
public sealed class PlayerDirectory
{
    public const long SteamId64Base = 76561197960265728L;

    private static readonly Regex Steam2 = new(
        @"^STEAM_[0-5]:([0-1]):(\d+)$",
        RegexOptions.Compiled | RegexOptions.CultureInvariant);

    /// <summary>
    /// GMod's networked <c>player_info_t</c> widens the name field to 128 bytes (SDK 2013 used 32).
    /// Observed against 2026 demos: <c>name[128]</c>, <c>userID</c> at 128, <c>guid[33]</c> at 132.
    /// </summary>
    public const int WideNameBytes = 128;

    public const int ClassicNameBytes = 32;

    private readonly List<StringTable> _tables = [];
    private readonly NetMessageWalker _walker = new();
    private readonly Dictionary<string, DemoPlayer> _bySteamId = new(StringComparer.Ordinal);
    private readonly Dictionary<string, int> _packedSlots = new(StringComparer.Ordinal);
    private readonly Dictionary<string, DemoPlayer> _dumpPlayers = new(StringComparer.Ordinal);
    private int _packedRunLength;

    public IReadOnlyList<StringTable> Tables => _tables;

    public StringTable? UserInfo =>
        _tables.FirstOrDefault(t => t.Name.Equals("userinfo", StringComparison.OrdinalIgnoreCase));

    public void AddPacket(byte[] payload)
    {
        ScanPlayerInfo(payload);
        WalkStringTables(payload);
    }

    public void AddPacket(ReadOnlySpan<byte> payload) => AddPacket(payload.ToArray());

    public IReadOnlyList<DemoPlayer> Snapshot()
    {
        foreach (var player in _dumpPlayers.Values)
        {
            MergeRecordedPlayer(player);
        }

        if (UserInfo is { } table)
        {
            foreach (var entry in table.Entries.Values)
            {
                var parsed = ParseUserInfo(entry);
                if (parsed?.SteamId is not null)
                {
                    MergeRecordedPlayer(parsed);
                }
            }
        }

        if (_dumpPlayers.Count == 0)
        {
            foreach (var (steamId, slot) in _packedSlots)
            {
                if (_bySteamId.TryGetValue(steamId, out var player) && player.Slot < 0)
                {
                    _bySteamId[steamId] = player with { Slot = slot };
                }
            }
        }

        return _bySteamId.Values
            .OrderBy(p => p.Slot < 0 ? int.MaxValue : p.Slot)
            .ThenBy(p => p.Name, StringComparer.Ordinal)
            .Select((p, id) => p with { Id = id })
            .ToList();
    }

    private void MergeRecordedPlayer(DemoPlayer player)
    {
        if (player.SteamId is not { } steamId) return;
        Merge(player);
        _bySteamId[steamId] = _bySteamId[steamId] with
        {
            Slot = player.Slot,
            Name = player.Name,
            UserId = player.UserId,
        };
    }

    public static PlayerDirectory FromDemo(string path, Action<long>? progress = null)
    {
        var directory = new PlayerDirectory();
        using var reader = DemoReader.Open(path);
        foreach (var command in reader.ReadCommands())
        {
            progress?.Invoke(reader.Position);
            switch (command)
            {
                case PacketCommand packet:
                    directory.AddPacket(packet.Payload);
                    break;
                case StringTablesCommand stringTables:
                    directory.AddStringTablesDump(stringTables.Payload);
                    break;
            }
        }

        return directory;
    }

    /// <summary>
    /// Walks <c>svc_CreateStringTable</c> / <c>svc_UpdateStringTable</c> using the same
    /// message spans as the net-message walker, so a failed entry decode cannot skip later
    /// tables. userinfo's table index is the player slot.
    /// </summary>
    private void WalkStringTables(byte[] payload)
    {
        var walk = _walker.Walk(payload);
        foreach (var span in walk.Messages)
        {
            if (span.Type == NetMessageType.SvcCreateStringTable)
            {
                var reader = new BitReader(payload);
                reader.Seek(span.StartBit);
                reader.ReadUBitLong(NetMessageLayout.TypeBits);
                var table = StringTableParser.ReadCreate(ref reader);
                if (table is not null)
                {
                    _tables.Add(table);
                }

                continue;
            }

            if (span.Type != NetMessageType.SvcUpdateStringTable || _tables.Count == 0)
            {
                continue;
            }

            var update = new BitReader(payload);
            update.Seek(span.StartBit);
            update.ReadUBitLong(NetMessageLayout.TypeBits);
            var tableId = (int)update.PeekUBitLong(5);
            if ((uint)tableId >= (uint)_tables.Count)
            {
                continue;
            }

            var updates = StringTableParser.ReadUpdate(ref update, _tables[tableId]);
            if (updates is not null)
            {
                _tables[tableId].Apply(updates);
            }
        }
    }

    /// <summary>
    /// Reads explicit userinfo indexes from the engine dump, retaining GUID scanning as a
    /// fallback when truncation prevents the userinfo table from being reached.
    /// </summary>
    public void AddStringTablesDump(byte[] payload)
    {
        ScanPlayerInfo(payload, assignPackedSlots: true);
        var dump = DemoStringTables.ParseFull(payload);
        if (dump is null || !dump.TryGetValue("userinfo", out var entries)) return;
        foreach (var entry in entries)
        {
            if (entry.UserData is not { Length: > 0 }) continue;
            var player = ParseUserInfo(entry);
            if (player?.SteamId is { } steamId)
            {
                _dumpPlayers[steamId] = player;
            }
        }
    }

    public void ScanPlayerInfo(byte[] payload) => ScanPlayerInfo(payload, assignPackedSlots: true);

    /// <summary>
    /// Locates <c>player_info_t</c> blobs by their <c>STEAM_X:Y:Z</c> GUID field. This does not
    /// depend on CreateStringTable framing; the struct itself is what we decode.
    /// </summary>
    public void ScanPlayerInfo(byte[] payload, bool assignPackedSlots)
    {
        var found = new List<(int GuidBit, DemoPlayer Player)>();
        foreach (var guidBit in FindAscii(payload, "STEAM_"u8.ToArray()))
        {
            if (!TryReadPlayerAtGuid(payload, guidBit, out var player, out _))
            {
                continue;
            }

            found.Add((guidBit, player));
        }

        var ordered = found
            .OrderBy(f => f.GuidBit)
            .DistinctBy(f => f.Player.SteamId)
            .ToList();

        int? lastSlot = null;
        foreach (var (guidBit, player) in ordered)
        {
            var userdataBit = guidBit - (WideNameBytes + 4) * 8;
            var slot = TryReadSlot(payload, userdataBit, lastSlot);
            Merge(player with { Slot = slot });
            lastSlot = slot >= 0 ? slot : lastSlot;
        }

        if (assignPackedSlots)
        {
            ConsiderPackedRun(ordered);
        }
    }

    /// <summary>
    /// userinfo updates write every occupied slot in index order as consecutive
    /// <c>player_info_t</c> blobs. When a packet contains one tightly packed run of those
    /// structs, the position in the run is the slot — but only the longest run is kept, and
    /// <see cref="EventCollector.BindPlayers"/> still checks it against the recorder's entity
    /// (slot + 1) before trusting it for anyone else.
    /// </summary>
    private void ConsiderPackedRun(List<(int GuidBit, DemoPlayer Player)> ordered)
    {
        const int minStrideBits = 2000;
        const int maxStrideBits = 4000;
        if (ordered.Count < 8)
        {
            return;
        }

        var bestStart = 0;
        var bestLength = 1;
        var start = 0;
        for (var i = 1; i <= ordered.Count; i++)
        {
            var gap = i < ordered.Count ? ordered[i].GuidBit - ordered[i - 1].GuidBit : int.MaxValue;
            if (gap is >= minStrideBits and <= maxStrideBits)
            {
                continue;
            }

            var length = i - start;
            if (length > bestLength)
            {
                bestStart = start;
                bestLength = length;
            }

            start = i;
        }

        if (bestLength <= _packedRunLength)
        {
            return;
        }

        _packedRunLength = bestLength;
        _packedSlots.Clear();
        for (var i = 0; i < bestLength; i++)
        {
            var steamId = ordered[bestStart + i].Player.SteamId;
            if (steamId is not null)
            {
                _packedSlots[steamId] = i;
            }
        }
    }

    private void Merge(DemoPlayer player)
    {
        var key = player.SteamId;
        if (key is null)
        {
            return;
        }

        if (_bySteamId.TryGetValue(key, out var existing))
        {
            _bySteamId[key] = existing with
            {
                Slot = existing.Slot >= 0 ? existing.Slot : player.Slot,
                UserId = player.UserId ?? existing.UserId,
                Name = DisconnectStatus(player.Name) ? existing.Name : player.Name ?? existing.Name,
                SteamId64 = player.SteamId64 ?? existing.SteamId64,
                FakePlayer = player.FakePlayer || existing.FakePlayer,
                IsHltv = player.IsHltv || existing.IsHltv,
            };
            return;
        }

        _bySteamId[key] = player with
        {
            Name = DisconnectStatus(player.Name) ? null : player.Name,
        };
    }

    private static bool DisconnectStatus(string? name) =>
        name is not null
        && name.Contains("disconnect", StringComparison.OrdinalIgnoreCase)
        && name.Contains("by user", StringComparison.OrdinalIgnoreCase);

    private static bool TryReadPlayerAtGuid(
        byte[] payload,
        int guidBit,
        out DemoPlayer player,
        out int nameBytes)
    {
        player = null!;
        nameBytes = WideNameBytes;

        foreach (var width in new[] { WideNameBytes, ClassicNameBytes })
        {
            var nameBit = guidBit - (width + 4) * 8;
            if (nameBit < 0 || guidBit + 33 * 8 > payload.Length * 8)
            {
                continue;
            }

            var reader = new BitReader(payload);
            reader.Seek(nameBit);
            var blob = reader.ReadBytes(width + 4 + 33);
            if (reader.IsOverflowed)
            {
                continue;
            }

            var steamId = NullIfEmpty(ReadCString(blob, width + 4, 33));
            if (!LookedLikeSteam2(steamId))
            {
                continue;
            }

            var name = NullIfEmpty(ReadCString(blob, 0, width));
            if (width == WideNameBytes && name is not null && !LooksLikePaddedName(blob.AsSpan(0, width)))
            {
                continue;
            }

            var userId = BinaryPrimitives.ReadInt32LittleEndian(blob.AsSpan(width));
            if (userId is < 1 or > 1_000_000)
            {
                userId = 0;
            }
            nameBytes = width;
            player = new DemoPlayer(
                Id: 0,
                Slot: -1,
                UserId: userId == 0 ? null : userId,
                Name: name,
                SteamId: steamId,
                SteamId64: ToSteamId64(steamId!),
                FakePlayer: false,
                IsHltv: false);
            return true;
        }

        return false;
    }

    /// <summary>
    /// userinfo entries are packed as: increment/index, no string, userdata length, then
    /// <c>player_info_t</c>. The index is the player slot.
    /// </summary>
    private static int TryReadSlot(byte[] payload, int userdataBit, int? lastSlot)
    {
        if (userdataBit < 17)
        {
            return -1;
        }

        foreach (var indexBits in new[] { 7, 8, 6, 5, 9 })
        {
            var explicitPrefix = 1 + indexBits + 1 + 1 + StringTableParser.UserDataLengthBits;
            if (userdataBit >= explicitPrefix)
            {
                var r = new BitReader(payload);
                r.Seek(userdataBit - explicitPrefix);
                var increment = r.ReadOneBit() != 0;
                var index = (int)r.ReadUBitLong(indexBits);
                var hasString = r.ReadOneBit() != 0;
                var hasUserData = r.ReadOneBit() != 0;
                var length = (int)r.ReadUBitLong(StringTableParser.UserDataLengthBits);
                if (!increment && !hasString && hasUserData
                    && length >= WideNameBytes + 4 + 33 && length <= 2048
                    && index >= 0 && index < (1 << indexBits)
                    && r.BitsRead == userdataBit)
                {
                    return index;
                }
            }

            const int incrementPrefix = 1 + 1 + 1 + StringTableParser.UserDataLengthBits;
            if (userdataBit >= incrementPrefix)
            {
                var r = new BitReader(payload);
                r.Seek(userdataBit - incrementPrefix);
                var increment = r.ReadOneBit() != 0;
                var hasString = r.ReadOneBit() != 0;
                var hasUserData = r.ReadOneBit() != 0;
                var length = (int)r.ReadUBitLong(StringTableParser.UserDataLengthBits);
                if (increment && !hasString && hasUserData
                    && length >= WideNameBytes + 4 + 33 && length <= 2048
                    && r.BitsRead == userdataBit)
                {
                    return lastSlot is { } previous ? previous + 1 : 0;
                }
            }
        }

        return -1;
    }

    private static bool LooksLikePaddedName(ReadOnlySpan<byte> field)
    {
        var i = 0;
        while (i < field.Length && field[i] != 0)
        {
            if (field[i] < 32)
            {
                return false;
            }

            i++;
        }

        if (i == 0)
        {
            return true;
        }

        for (; i < field.Length; i++)
        {
            if (field[i] != 0)
            {
                return false;
            }
        }

        return true;
    }

    private static IEnumerable<int> FindAscii(byte[] payload, byte[] needle)
    {
        var totalBits = payload.Length * 8;
        var needleBytes = needle;
        for (var shift = 0; shift < 8; shift++)
        {
            var availableBytes = (totalBits - shift) / 8;
            if (availableBytes < needleBytes.Length)
            {
                continue;
            }

            var reader = new BitReader(payload);
            reader.Seek(shift);
            var shifted = reader.ReadBytes(availableBytes);
            var from = 0;
            while (from <= shifted.Length - needleBytes.Length)
            {
                var at = shifted.AsSpan(from).IndexOf(needleBytes);
                if (at < 0)
                {
                    break;
                }

                yield return shift + ((from + at) * 8);
                from += at + 1;
            }
        }
    }

    public static DemoPlayer? ParseUserInfo(StringTableEntry entry)
    {
        string? name = NullIfEmpty(entry.Text);
        string? steamId = null;
        string? steamId64 = null;
        int? userId = null;
        var fake = false;
        var hltv = false;

        if (entry.UserData is { Length: > 0 } data)
        {
            var wide = data.Length >= WideNameBytes + 4 + 33
                && LookedLikeSteam2(ReadCString(data, WideNameBytes + 4, 33));
            var nameBytes = wide ? WideNameBytes : ClassicNameBytes;

            if (data.Length >= nameBytes + 4)
            {
                // userinfo's entry text is its numeric slot, not the Steam display name.
                name = NullIfEmpty(ReadCString(data, 0, nameBytes)) ?? name;
                userId = BinaryPrimitives.ReadInt32LittleEndian(data.AsSpan(nameBytes));
            }

            if (data.Length >= nameBytes + 4 + 33)
            {
                steamId = NullIfEmpty(ReadCString(data, nameBytes + 4, 33));
            }

            if (steamId is null)
            {
                steamId = FindSteam2(data);
            }
        }

        if (steamId is not null && LookedLikeSteam2(steamId))
        {
            steamId64 ??= ToSteamId64(steamId);
        }
        else if (steamId is not null && steamId.StartsWith("7656", StringComparison.Ordinal))
        {
            steamId64 = steamId;
            steamId = FromSteamId64(steamId);
        }
        else
        {
            steamId = FindSteam2FromText(name);
            if (steamId is not null)
            {
                steamId64 ??= ToSteamId64(steamId);
            }
        }

        if (name is null && steamId is null && steamId64 is null)
        {
            return null;
        }

        return new DemoPlayer(
            Id: 0,
            Slot: entry.Index,
            UserId: userId is 0 ? null : userId,
            Name: name,
            SteamId: steamId,
            SteamId64: steamId64,
            FakePlayer: fake,
            IsHltv: hltv);
    }

    public static string? ToSteamId64(string steam2)
    {
        var match = Steam2.Match(steam2);
        if (!match.Success)
        {
            return null;
        }

        var y = int.Parse(match.Groups[1].Value);
        var z = long.Parse(match.Groups[2].Value);
        return (z * 2 + y + SteamId64Base).ToString();
    }

    public static string? FromSteamId64(string steamId64)
    {
        if (!long.TryParse(steamId64, out var id) || id < SteamId64Base)
        {
            return null;
        }

        var account = id - SteamId64Base;
        var y = account & 1;
        var z = account >> 1;
        return $"STEAM_0:{y}:{z}";
    }

    private static string ReadCString(byte[] data, int offset, int max)
    {
        var end = offset;
        var limit = Math.Min(data.Length, offset + max);
        while (end < limit && data[end] != 0)
        {
            end++;
        }

        return Encoding.UTF8.GetString(data, offset, end - offset);
    }

    private static string? FindSteam2(byte[] data)
    {
        var text = Encoding.ASCII.GetString(data);
        var match = Regex.Match(text, @"STEAM_[0-5]:[0-1]:\d+");
        return match.Success ? match.Value : null;
    }

    private static string? FindSteam2FromText(string? text)
    {
        if (text is null)
        {
            return null;
        }

        var match = Steam2.Match(text);
        return match.Success ? match.Value : null;
    }

    private static bool LookedLikeSteam2(string? value) =>
        value is not null && Steam2.IsMatch(value);

    private static string? NullIfEmpty(string? value) =>
        string.IsNullOrWhiteSpace(value) ? null : value;
}
