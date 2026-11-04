using GmodDemo.Parser;

namespace GmodDemo.Parser.Tests;

public sealed class PlayerDirectoryTests
{
    [Fact]
    public void Steam2_converts_to_steam64()
    {
        Assert.Equal("76561197960265730", PlayerDirectory.ToSteamId64("STEAM_0:0:1"));
        Assert.Equal("STEAM_0:0:1", PlayerDirectory.FromSteamId64("76561197960265730"));
    }

    [Theory]
    [InlineData(PlayerDirectory.ClassicNameBytes)]
    [InlineData(PlayerDirectory.WideNameBytes)]
    public void Userinfo_entry_text_is_a_slot_not_a_display_name(int nameBytes)
    {
        var data = new byte[324];
        "Actual Steam name"u8.CopyTo(data);
        BitConverter.TryWriteBytes(data.AsSpan(nameBytes), 180);
        "STEAM_0:0:1"u8.CopyTo(data.AsSpan(nameBytes + 4));
        var player = PlayerDirectory.ParseUserInfo(new StringTableEntry(59, "59", data));
        Assert.NotNull(player);
        Assert.Equal("Actual Steam name", player.Name);
        Assert.Equal(60, player.Entity);
    }

    [Fact]
    public void Engine_dump_preserves_vacant_slots_and_overrides_packed_run()
    {
        var bytes = new List<byte>();
        var position = 0;
        void UInt(uint value, int width)
        {
            for (var i = 0; i < width; i++, position++)
            {
                if (position / 8 == bytes.Count) bytes.Add(0);
                bytes[position / 8] |= (byte)(((value >> i) & 1) << (position % 8));
            }
        }
        void String(string text)
        {
            foreach (var value in System.Text.Encoding.UTF8.GetBytes(text)) UInt(value, 8);
            UInt(0, 8);
        }
        UInt(2, 8);
        String("downloadables");
        UInt(0, 16);
        UInt(0, 1);
        String("userinfo");
        UInt(12, 16);
        for (var slot = 0; slot < 12; slot++)
        {
            String(slot.ToString());
            var occupied = slot != 4 && slot != 8;
            UInt(occupied ? 1u : 0u, 1);
            if (!occupied) continue;
            var data = new byte[324];
            System.Text.Encoding.UTF8.GetBytes($"Player {slot}").CopyTo(data, 0);
            BitConverter.TryWriteBytes(data.AsSpan(128), slot + 100);
            System.Text.Encoding.ASCII.GetBytes($"STEAM_0:0:{slot + 1}").CopyTo(data, 132);
            UInt((uint)data.Length, 16);
            foreach (var value in data) UInt(value, 8);
        }
        UInt(0, 1);
        var directory = new PlayerDirectory();
        directory.AddStringTablesDump([.. bytes]);
        var players = directory.Snapshot();
        Assert.Equal(10, players.Count);
        Assert.Equal(9, players.Single(p => p.Name == "Player 9").Slot);
        Assert.Equal(12, players.Single(p => p.Name == "Player 11").Entity);
        Assert.DoesNotContain(players, p => p.Slot is 4 or 8);
    }

    [Fact]
    public void Wide_player_info_t_puts_the_guid_at_offset_132()
    {
        var data = new byte[165];
        "Player"u8.CopyTo(data);
        BitConverter.TryWriteBytes(data.AsSpan(128), 42);
        "STEAM_0:0:1"u8.CopyTo(data.AsSpan(132));

        var player = PlayerDirectory.ParseUserInfo(new StringTableEntry(3, null, data));

        Assert.NotNull(player);
        Assert.Equal(3, player.Slot);
        Assert.Equal(4, player.Entity);
        Assert.Equal("Player", player.Name);
        Assert.Equal(42, player.UserId);
        Assert.Equal("STEAM_0:0:1", player.SteamId);
        Assert.Equal("76561197960265730", player.SteamId64);
    }
}

public sealed class RealDemoPlayerTests
{
    [ShortDemoFact]
    public void Short_demo_lists_the_recorder_and_the_connected_players()
    {
        var players = PlayerDirectory.FromDemo(RealDemo.ShortPath).Snapshot();
        Assert.True(players.Count >= 2, $"players={players.Count}");

        var recorderName = RealDemo.ClientName(RealDemo.ShortPath);
        var recorder = players.Single(p => p.Name == recorderName);
        Assert.True(recorder.SteamId64 is { Length: > 0 }, "recorder should have a SteamID64");
        Assert.True(recorder.Slot >= 0, "recorder should have a slot");
        Assert.Equal(recorder.Slot + 1, recorder.Entity);
    }

    [RealDemoFact]
    public void Long_demo_also_identifies_the_recorder()
    {
        var recorderName = RealDemo.ClientName(RealDemo.Path);
        var recorder = PlayerDirectory.FromDemo(RealDemo.Path).Snapshot()
            .FirstOrDefault(p => p.Name == recorderName);

        Assert.NotNull(recorder);
        Assert.True(recorder.SteamId64 is { Length: > 0 });
        Assert.True(recorder.Slot >= 0, "packed userinfo run should assign the recorder a slot");
    }
}

public sealed class EventCollectorTests
{
    [Fact]
    public void Player_say_keys_match_the_wiki()
    {
        var keys = new Dictionary<string, object?>
        {
            ["userid"] = 790,
            ["text"] = "hello",
            ["teamonly"] = 1,
        };
        Assert.Equal("hello", GmodGameEvents.SayText(keys));
        Assert.Equal(790, GmodGameEvents.UserId(keys));
        Assert.True(GmodGameEvents.TeamOnly(keys));
        Assert.Equal(12, GmodGameEvents.EntityFromKeys(GmodGameEvents.EntityKilled, new Dictionary<string, object?> { ["entindex_killed"] = 12 }));
        Assert.Equal(70, GmodGameEvents.EntityFromKeys(GmodGameEvents.PlayerInfo, new Dictionary<string, object?> { ["index"] = 69 }));
    }

    [ShortDemoFact]
    public void Short_demo_records_console_and_does_not_invent_lua_chat()
    {
        var collector = new EventCollector();
        using var reader = DemoReader.Open(RealDemo.ShortPath);
        foreach (var command in reader.ReadCommands())
        {
            switch (command)
            {
                case PacketCommand packet:
                    collector.AddPacket(packet);
                    break;
                case ConsoleCommand console:
                    collector.AddConsole(console);
                    break;
                case DataTablesCommand dataTables:
                    collector.AddDataTables(dataTables);
                    break;
            }
        }

        Assert.Contains(collector.Events.OfType<ConsoleEvent>(), e => e.Command.StartsWith("+showscores", StringComparison.Ordinal));
        Assert.Contains(collector.NetLog.Rows, r => r.Type == "lua");
        Assert.All(collector.Events.OfType<ChatEvent>(), e => Assert.True(e.Text.Length > 0));
    }
}
