using System.Text.Json;
using GmodDemo.Parser;

namespace GmodDemo.Parser.Tests;

public sealed class PlayerConnectionTests
{
    [Fact]
    public void Reused_slot_rebinds_player_events_and_voice_to_the_active_connection()
    {
        var billy = new DemoPlayer(
            0, 105, 376, "Billy Blowher", "STEAM_0:0:900924143", "76561199762114014", false, false);
        var dekkrior = new DemoPlayer(
            1, 18, 21, "Dekkrior", "STEAM_0:1:51937904", "76561198064141537", false, false);
        var collector = new EventCollector();
        collector.AddEvent(new MoveEvent(5, 106, new Vec3(1, 0, 0)));
        collector.AddEvent(Disconnect(10, 376));
        collector.AddEvent(Connect(20, "hipopatumus", 105, 489, "STEAM_0:1:123231623"));
        collector.AddEvent(new MoveEvent(25, 106, new Vec3(2, 0, 0)));
        collector.AddEvent(Disconnect(30, 489));
        collector.AddEvent(new MoveEvent(35, 106, new Vec3(3, 0, 0)));
        collector.AddEvent(Disconnect(40, 21));
        collector.AddEvent(Connect(50, "Dekkrior", 105, 521, "STEAM_0:1:51937904"));
        collector.AddEvent(new MoveEvent(55, 106, new Vec3(4, 0, 0)));

        var players = collector.BindPlayers([billy, dekkrior]);
        Assert.Equal(2, players.Count(player => player.SteamId == "STEAM_0:1:51937904"));
        var dekkriorTrack = Assert.Single(players, player => player.UserId == 521);
        Assert.Equal(18, players.Single(player => player.UserId == 21).Slot);
        Assert.Equal(105, dekkriorTrack.Slot);
        Assert.Equal(10, players.Single(p => p.Id == billy.Id).DisconnectedTick);
        Assert.Equal(50, dekkriorTrack.ConnectedTick);
        Assert.Equal(0, collector.Events.OfType<MoveEvent>().Single(move => move.T == 5).P);
        Assert.Equal(2, collector.Events.OfType<MoveEvent>().Single(move => move.T == 25).P);
        Assert.Null(collector.Events.OfType<MoveEvent>().Single(move => move.T == 35).P);
        Assert.Equal(dekkriorTrack.Id, collector.Events.OfType<MoveEvent>().Single(move => move.T == 55).P);
        Assert.Equal("Billy Blowher", collector.PlayerAtSlot(105, 5)?.Name);
        Assert.Null(collector.PlayerAtSlot(105, 35));
        Assert.Equal("Dekkrior", collector.PlayerAtSlot(105, 55)?.Name);

        var voice = new VoiceTrack();
        foreach (var tick in new[] { 5, 25, 35, 55 })
        {
            voice.AddPacket(tick, 105, proximity: true, [1, 2]);
        }

        var jsonPath = Path.Combine(Path.GetTempPath(), $"gmoddem-slot-reuse-{Guid.NewGuid():N}.json");
        var audioDirectory = Path.ChangeExtension(jsonPath, ".audio");
        try
        {
            var clips = voice.WriteSidecars(jsonPath, players, 66.6667, collector.PlayerAtSlot);
            Assert.Equal(new int?[] { 0, 2, null, dekkriorTrack.Id }, clips.Select(clip => clip.P));

            var playback = PlaybackDocument.Build(players, collector.Events, new PrecacheTables(), 66.6667, clips);
            using var stream = new MemoryStream();
            using (var writer = new Utf8JsonWriter(stream))
            {
                writer.WriteStartObject();
                playback.Write(writer);
                writer.WriteEndObject();
            }

            using var document = JsonDocument.Parse(stream.ToArray());
            var rows = document.RootElement.GetProperty("players").EnumerateArray()
                .ToDictionary(player => player.GetProperty("id").GetInt32());
            Assert.Equal(1, rows[0].GetProperty("voice").GetArrayLength());
            Assert.Equal(1, rows[2].GetProperty("voice").GetArrayLength());
            Assert.Equal(1, rows[dekkriorTrack.Id].GetProperty("voice").GetArrayLength());
            Assert.Equal(new[] { 5 }, rows[0].GetProperty("ticks").EnumerateArray()
                .Select(t => t.GetProperty("t").GetInt32()));
            Assert.Equal(new[] { 25 }, rows[2].GetProperty("ticks").EnumerateArray()
                .Select(t => t.GetProperty("t").GetInt32()));
            Assert.Equal(new[] { 55 }, rows[dekkriorTrack.Id].GetProperty("ticks").EnumerateArray()
                .Select(t => t.GetProperty("t").GetInt32()));
            Assert.DoesNotContain(rows.Values, row =>
                row.GetProperty("id").GetInt32() != 0
                && row.GetProperty("id").GetInt32() != 2
                && row.GetProperty("id").GetInt32() != dekkriorTrack.Id
                && row.TryGetProperty("voice", out _));
        }

        finally
        {
            if (Directory.Exists(audioDirectory))
            {
                Directory.Delete(audioDirectory, true);
            }
        }
    }

    [Fact]
    public void Late_appended_entity_events_are_bound_in_tick_order()
    {
        var collector = new EventCollector();
        collector.AddEvent(Disconnect(10, 1));
        collector.AddEvent(Connect(20, "Second", 4, 2, "STEAM_0:0:20"));
        collector.AddEvent(new MoveEvent(5, 5, new Vec3(1, 0, 0)));
        collector.AddEvent(new MoveEvent(15, 5, new Vec3(2, 0, 0)));
        collector.AddEvent(new MoveEvent(25, 5, new Vec3(3, 0, 0)));
        var players = collector.BindPlayers([
            new DemoPlayer(0, 4, 1, "First", "STEAM_0:0:10", null, false, false),
            new DemoPlayer(1, 4, 2, "Second", "STEAM_0:0:20", null, false, false),
        ]);

        Assert.Equal(new int?[] { 0, null, 1 }, collector.Events.OfType<MoveEvent>().Select(e => e.P));
        Assert.Equal("First", collector.PlayerAtSlot(4, 5)?.Name);
        Assert.Null(collector.PlayerAtSlot(4, 15));
        Assert.Equal(20, players.Single(p => p.Id == 1).ConnectedTick);
    }

    private static SourceGameEvent Connect(int tick, string name, int slot, int userId, string steamId) =>
        new(
            tick,
            8,
            GmodGameEvents.PlayerConnectClient,
            new Dictionary<string, object?>
            {
                ["name"] = name,
                ["index"] = slot,
                ["userid"] = userId,
                ["networkid"] = steamId,
            });

    private static SourceGameEvent Disconnect(int tick, int userId) =>
        new(
            tick,
            10,
            GmodGameEvents.PlayerDisconnect,
            new Dictionary<string, object?> { ["userid"] = userId });
}
