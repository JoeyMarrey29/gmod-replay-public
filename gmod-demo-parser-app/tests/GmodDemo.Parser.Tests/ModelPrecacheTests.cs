using GmodDemo.Parser;

namespace GmodDemo.Parser.Tests;

public sealed class ModelPrecacheTests
{
    [Fact]
    public void Lightstyles_keep_their_recorded_tick_and_userdata_only_updates()
    {
        var initial = new List<(int Value, int Bits)> { (1,1),(1,1),(0,1) };
        AddString(initial,"0");
        initial.AddRange([(1,1),(2,14),('m',8),(0,8)]);
        var create = new List<(int Value, int Bits)> { ((int)NetMessageType.SvcCreateStringTable,6) };
        AddString(create,"lightstyles");
        create.AddRange([(64,16),(1,7),(initial.Sum(f=>f.Bits),20),(0,1),(0,1)]);
        create.AddRange(initial);
        var precache = new PrecacheTables();
        precache.AddPacket(PackBits(create),[new(NetMessageType.SvcCreateStringTable,0,create.Sum(f=>f.Bits))],10);
        Assert.Equal(new PrecacheTables.LightStyle(10,0,"m"),Assert.Single(precache.LightStyles));
        foreach (var tick in new[] {25,26})
        {
            var data = new List<(int Value,int Bits)> { (1,1),(0,1),(1,1),(2,14),('a',8),(0,8) };
            var update = new List<(int Value,int Bits)> {
                ((int)NetMessageType.SvcUpdateStringTable,6),(0,5),(0,1),(data.Sum(f=>f.Bits),20) };
            update.AddRange(data);
            precache.AddPacket(PackBits(update),[new(NetMessageType.SvcUpdateStringTable,0,update.Sum(f=>f.Bits))],tick);
        }
        Assert.Equal(2,precache.LightStyles.Count);
        Assert.Equal(new PrecacheTables.LightStyle(25,0,"a"),precache.LightStyles[1]);
    }
    [Fact]
    public void Mixed_string_table_lengths_keep_following_messages_aligned()
    {
        var packet = new List<(int Value, int Bits)>();
        foreach (var width in new[] { 32,20,32 })
        {
            var entries = new List<(int Value, int Bits)> { (0,1),(1,5) };
            AddStringEntry(entries, $"models/length_{width}.mdl");
            packet.Add(((int)NetMessageType.SvcCreateStringTable,6));
            AddString(packet, "modelprecache");
            packet.AddRange([(32,16),(1,6),(entries.Sum(f => f.Bits),width),(0,1),(0,1)]);
            packet.AddRange(entries);
        }
        packet.Add((0,6));
        var bytes = PackBits(packet);
        var walk = new NetMessageWalker().Walk(bytes);
        Assert.Equal(PacketWalkStatus.Clean, walk.Status);
        Assert.Equal(3, walk.Messages.Count(m => m.Type == NetMessageType.SvcCreateStringTable));
        var precache = new PrecacheTables();
        precache.AddPacket(bytes, walk.Messages);
        Assert.Equal("models/length_32.mdl", precache.ModelPath(1));
    }

    [Theory]
    [InlineData(20)]
    [InlineData(32)]
    public void Instance_baselines_are_loaded_from_the_dump_and_updated_without_repeating_class_names(int lengthWidth)
    {
        var dump = new List<(int Value, int Bits)> { (2,8) };
        AddString(dump, "downloadables");
        dump.AddRange([(0,16),(0,1)]);
        AddString(dump, "instancebaseline");
        dump.Add((1,16));
        AddString(dump, "17");
        dump.AddRange([(1,1),(2,16),(11,8),(22,8),(0,1)]);
        var precache = new PrecacheTables();
        precache.AddPayload(PackBits(dump));
        Assert.Equal(new byte[] { 11,22 }, precache.InstanceBaselines[17]);

        var initial = new List<(int Value, int Bits)> { (1,1) };
        initial.AddRange([(1,1),(0,1)]);
        AddString(initial, "17");
        initial.AddRange([(1,1),(2,14),(12,8),(23,8)]);
        var create = new List<(int Value, int Bits)> { ((int)NetMessageType.SvcCreateStringTable,6) };
        AddString(create, "instancebaseline");
        create.AddRange([(32,16),(1,6),(initial.Sum(f => f.Bits),lengthWidth),(0,1),(0,1)]);
        create.AddRange(initial);
        var bytes = PackBits(create);
        precache.AddPacket(bytes, [new(NetMessageType.SvcCreateStringTable,0,create.Sum(f => f.Bits))]);
        Assert.Equal(new byte[] { 12,23 }, precache.InstanceBaselines[17]);

        foreach (var skin in new[] { 13,14 })
        {
            var data = new List<(int Value, int Bits)>
            {
                (1,1),(0,1),(1,1),(2,14),(skin,8),(24,8),
            };
            var update = new List<(int Value, int Bits)>
            {
                ((int)NetMessageType.SvcUpdateStringTable,6),(0,5),(0,1),
                (data.Sum(f => f.Bits),20),
            };
            update.AddRange(data);
            precache.AddPacket(PackBits(update),
                [new(NetMessageType.SvcUpdateStringTable,0,update.Sum(f => f.Bits))]);
            Assert.Equal(new byte[] { (byte)skin,24 }, precache.InstanceBaselines[17]);
        }
    }

    [PlankDemoFact]
    public void Recorded_dropped_plank_resolves_its_late_model_path()
    {
        var collector = DataTableTests.Interpret(RealDemo.PlankPath);
        Assert.Equal("models/props_debris/wood_board06a.mdl", collector.Precache.ModelPath(1999));
        Assert.Equal(0, collector.Precache.ModelUpdateFailures);
        Assert.Contains(collector.Events.OfType<SpawnEvent>(),
            spawn => spawn.Entity == 1108 && spawn.T == 35359 && spawn.ModelIndex == 1999);
    }

    [Theory]
    [InlineData("modelprecache", "models/old.mdl", "models/test/dynamic_1999.mdl", 20)]
    [InlineData("soundprecache", "doors/door1_move.wav", "perp2.5/house_alarm_long.mp3", 20)]
    [InlineData("modelprecache", "models/old.mdl", "models/test/dynamic_1999.mdl", 32)]
    [InlineData("soundprecache", "doors/door1_move.wav", "perp2.5/house_alarm_long.mp3", 32)]
    public void Applies_incremental_precache_updates_from_packets(string tableName, string initial, string added, int lengthWidth)
    {
        var payload = new List<(int Value, int Bits)>();
        var createEntries = new List<(int Value, int Bits)>
        {
            (0, 1), (1, 12),
        };
        AddStringEntry(createEntries, initial);

        var createStart = 0;
        payload.Add(((int)NetMessageType.SvcCreateStringTable, NetMessageLayout.TypeBits));
        AddString(payload, tableName);
        payload.Add((4096, 16));
        payload.Add((1, 13));
        payload.Add((createEntries.Sum(field => field.Bits), lengthWidth));
        payload.Add((0, 1));
        payload.Add((0, 1));
        payload.AddRange(createEntries);
        var createLength = payload.Sum(field => field.Bits);

        var updateStart = createLength;
        payload.Add(((int)NetMessageType.SvcUpdateStringTable, NetMessageLayout.TypeBits));
        payload.Add((0, 5));
        payload.Add((1, 1));
        payload.Add((1, 16));
        var updateEntries = new List<(int Value, int Bits)> { (0, 1), (1999, 12) };
        AddStringEntry(updateEntries, added);
        payload.Add((updateEntries.Sum(field => field.Bits), 20));
        payload.AddRange(updateEntries);

        var bytes = PackBits(payload);
        var walk = new NetMessageWalker().Walk(bytes);
        Assert.Equal(PacketWalkStatus.Clean, walk.Status);
        var spans = walk.Messages;
        Assert.Equal(2, spans.Count);
        Assert.Equal(createStart, spans[0].StartBit);
        Assert.Equal(updateStart, spans[1].StartBit);
        var precache = new PrecacheTables();
        precache.AddPacket(bytes, spans);

        var paths = tableName == "modelprecache" ? precache.Models : precache.Sounds;
        Assert.Equal(initial, paths[1]);
        Assert.Equal(added, paths[1999]);
    }

    [Fact]
    public void Retains_incremental_modelprecache_when_the_initial_dump_is_loaded()
    {
        var precache = new PrecacheTables();
        var entries = new List<(int Value, int Bits)> { (0, 1), (1999, 13) };
        AddStringEntry(entries, "models/test/dynamic_1999.mdl");
        var create = new List<(int Value, int Bits)> { ((int)NetMessageType.SvcCreateStringTable, NetMessageLayout.TypeBits) };
        AddString(create, "modelprecache");
        create.AddRange([(13, 5), (1, 14), (entries.Sum(field => field.Bits), 20), (0, 1), (0, 1)]);
        create.AddRange(entries);
        var packet = PackBits(create);
        precache.AddPacket(packet, new NetMessageWalker().Walk(packet).Messages);
        precache.AddPayload(BuildStringTablesDump());

        Assert.Equal("models/test/dynamic_1999.mdl", precache.ModelPath(1999));
    }

    [Theory]
    [InlineData(0)]
    [InlineData(20)]
    [InlineData(32)]
    public void Compact_capacity_and_subbyte_model_flags_preserve_late_models(int lengthWidth)
    {
        var initial = new List<(int Value, int Bits)> { (0, 1), (1, 13) };
        AddStringEntry(initial, "models/old.mdl");
        var payload = new List<(int Value, int Bits)> { ((int)NetMessageType.SvcCreateStringTable, NetMessageLayout.TypeBits) };
        AddString(payload, "modelprecache");
        payload.AddRange([(13, 5), (1, 14)]);
        var length = initial.Sum(field => field.Bits);
        var varintLength = lengthWidth == 0;
        if (varintLength)
        {
            do {
                var next = length & 127;
                length >>= 7;
                payload.Add((next | (length > 0 ? 128 : 0), 8));
            } while (length > 0);
        }
        else payload.Add((length, lengthWidth));
        payload.AddRange([(1, 1), (1, 12), (2, 4), (0, 1)]);
        payload.AddRange(initial);
        var changed = new List<(int Value, int Bits)> { (0, 1), (1999, 13), (1, 1), (0, 1) };
        AddString(changed, "models/props_debris/wood_board06a.mdl");
        changed.AddRange([(1, 1), (3, 2)]);
        payload.AddRange([((int)NetMessageType.SvcUpdateStringTable, NetMessageLayout.TypeBits),
            (0, 5), (0, 1), (changed.Sum(field => field.Bits), 20)]);
        payload.AddRange(changed);
        var packet = PackBits(payload);
        var walk = new NetMessageWalker().Walk(packet);
        var reader = new BitReader(packet);
        reader.Seek(NetMessageLayout.TypeBits);
        var table = Assert.IsType<StringTable>(StringTableParser.ReadCreate(ref reader));
        Assert.Equal(8192, table.MaxEntries);
        Assert.Equal(2, table.UserDataSizeBits);
        Assert.Equal(varintLength, table.LengthIsVarInt);
        Assert.True(walk.Status == PacketWalkStatus.Clean,
            $"{walk.Status} at {walk.FailedAt}, consumed {walk.BitsConsumed}; create end {reader.BitsRead}, varint {table.LengthIsVarInt}");
        Assert.Equal(2, walk.Messages.Count);
        var precache = new PrecacheTables();
        precache.AddPacket(packet, walk.Messages);
        Assert.Equal("models/props_debris/wood_board06a.mdl", precache.ModelPath(1999));
        Assert.Equal(0, precache.ModelUpdateFailures);
    }

    [ShortDemoFact]
    public void Recovers_modelprecache_and_stamps_spawn_models()
    {
        var collector = DataTableTests.Interpret(RealDemo.ShortPath);
        Assert.True(
            collector.Precache.Models.Count > 0,
            $"modelprecache recovered {collector.Precache.Models.Count} paths");
        Assert.Contains(
            collector.Precache.Models.Values,
            p => p.Contains("models/", StringComparison.OrdinalIgnoreCase));

        Assert.NotNull(collector.Entities.RecorderEntity);
        var playerSpawn = collector.Events.OfType<SpawnEvent>()
            .First(s => s.Entity == collector.Entities.RecorderEntity && s.IsPlayer);
        Assert.True(playerSpawn.ModelIndex is > 0, "recorder spawn should carry m_nModelIndex");
        var path = collector.Precache.ModelPath(playerSpawn.ModelIndex);
        Assert.NotNull(path);
        Assert.Contains("models/", path, StringComparison.OrdinalIgnoreCase);

        var withModel = collector.Events.OfType<SpawnEvent>()
            .Count(s => collector.Precache.ModelPath(s.ModelIndex) is not null);
        Assert.True(withModel > 10, $"only {withModel} spawns resolved a model path");

        var mapDoor = collector.Events.OfType<SpawnEvent>()
            .FirstOrDefault(s => s.IsDoor && s.MapCreatedId is > 0);
        Assert.True(mapDoor is not null, "map-placed doors should carry m_iMapCreatedID");
    }

    private static void AddStringEntry(List<(int Value, int Bits)> fields, string value)
    {
        fields.Add((1, 1));
        fields.Add((0, 1));
        AddString(fields, value);
        fields.Add((0, 1));
    }

    private static void AddString(List<(int Value, int Bits)> fields, string value)
    {
        foreach (var character in value)
        {
            fields.Add((character, 8));
        }

        fields.Add((0, 8));
    }

    private static byte[] PackBits(IReadOnlyList<(int Value, int Bits)> fields)
    {
        var total = fields.Sum(field => field.Bits);
        var buffer = new byte[(total + 7) / 8];
        var bit = 0;
        foreach (var (value, bits) in fields)
        {
            for (var i = 0; i < bits; i++)
            {
                if ((value & (1 << i)) != 0)
                {
                    buffer[bit >> 3] |= (byte)(1 << (bit & 7));
                }

                bit++;
            }
        }

        return buffer;
    }

    private static byte[] BuildStringTablesDump()
    {
        var bits = new List<(int Value, int Bits)> { (2, 8) };
        AddString(bits, "downloadables");
        bits.Add((0, 16));
        bits.Add((0, 1));

        AddString(bits, "modelprecache");
        bits.Add((2000, 16));
        for (var index = 0; index < 2000; index++)
        {
            AddString(bits, index == 1 ? "models/old.mdl" : "");
            bits.Add((0, 1));
        }
        bits.Add((0, 1));

        return PackBits(bits);
    }
}
