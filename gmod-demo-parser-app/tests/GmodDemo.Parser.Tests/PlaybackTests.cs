using System.Text;
using System.Text.Json;
using GmodDemo.Parser;

namespace GmodDemo.Parser.Tests;

public sealed class PlaybackTests
{
    [Fact]
    public void Bank_shutters_retain_sequences_and_reentry_without_animating_unrelated_map_props()
    {
        var bits=new List<int>();
        void UInt(uint value,int width) { for(int i=0;i<width;++i) bits.Add((int)((value>>i)&1)); }
        void Text(string text) { foreach(char c in text) UInt(c,8);UInt(0,8); }
        UInt(2,8);Text("downloadables");UInt(0,16);UInt(0,1);
        Text("modelprecache");UInt(4,16);
        foreach(var model in new[]{"","models/perpheads/props/bankshutters.mdl","models/perpheads/props/bankshutters2.mdl","models/props/ceiling_fan.mdl"}) {
            Text(model);UInt(0,1);
        }
        UInt(0,1);
        var payload=new byte[(bits.Count+7)/8];
        for(int i=0;i<bits.Count;++i) payload[i/8]|=(byte)(bits[i]<<(i%8));
        var precache=new PrecacheTables();precache.AddPayload(payload);
        GameEvent[] events=[
            new SpawnEvent(0,700,"CDynamicProp",false,ModelIndex:1,MapCreatedId:2662),
            new AnimEvent(0,700,3,0,1),new AnimEvent(25,700,2,0.25f,1),
            new RemoveEvent(30,700,false),new SpawnEvent(50,700,"CDynamicProp",false),
            new AnimEvent(50,700,2,0.75f,1),
            new SpawnEvent(0,800,"CDynamicProp",false,ModelIndex:3,MapCreatedId:3000),
            new AnimEvent(50,800,1,0.5f,1),
        ];
        var playback=PlaybackDocument.Build([],events,precache,25);
        using var stream=new MemoryStream();
        using(var writer=new Utf8JsonWriter(stream)) {
            writer.WriteStartObject();playback.Write(writer);writer.WriteEndObject();
        }
        using var document=JsonDocument.Parse(stream.ToArray());
        var rows=document.RootElement.GetProperty("objects").EnumerateArray()
            .Where(r=>r.GetProperty("type").GetString()=="animation").ToArray();
        Assert.Equal(3,rows.Length);
        Assert.All(rows,r=>Assert.Equal(700,r.GetProperty("e").GetInt32()));
        Assert.Equal(0.75f,rows[2].GetProperty("cycle").GetSingle());
        Assert.Equal(2,rows[2].GetProperty("time").GetSingle());
    }
    [Fact]
    public void Brush_doors_and_parent_trains_keep_recorded_translation_and_rotation()
    {
        var bits = new List<int>();
        void UInt(uint value,int width) { for (var i=0;i<width;i++) bits.Add((int)((value>>i)&1)); }
        void Text(string value) { foreach (var c in value) UInt(c,8); UInt(0,8); }
        UInt(2,8);
        Text("downloadables"); UInt(0,16); UInt(0,1);
        Text("modelprecache"); UInt(3,16);
        Text(""); UInt(0,1); Text("*85"); UInt(0,1);
        Text("models/props/elevator_office_door.mdl"); UInt(0,1); UInt(0,1);
        var payload = new byte[(bits.Count+7)/8];
        for (var i=0;i<bits.Count;i++) payload[i/8] |= (byte)(bits[i]<<(i%8));
        var precache = new PrecacheTables();
        precache.AddPayload(payload);
        Assert.Equal("*85",precache.ModelPath(1));
        var events = new GameEvent[]
        {
            new SpawnEvent(0,555,"CBaseDoor",false,Name:"func_door",IsDoor:true,
                Origin:new Vec3(101,90,50),ModelIndex:1,Parent:970),
            new SpawnEvent(0,970,"CFuncTrackTrain",false,
                Origin:new Vec3(-3643,3180,1320),ModelIndex:1),
            new MoveEvent(25,555,new Vec3(101,0,50)),
            new MoveEvent(25,970,new Vec3(-3643,3180,2320)),
            new LookEvent(25,970,new Vec3(0,90,0)),
            new SpawnEvent(0,800,"CDynamicProp",false,ModelIndex:2,MapCreatedId:2764,Parent:555,
                Origin:new Vec3(-1,-14,4)),
            new VisibilityEvent(0,800,false),
            new RemoveEvent(50,555,false),
            new RemoveEvent(50,970,false),
            new RemoveEvent(50,800,false),
            new VisibilityEvent(51,800,true),
            new SpawnEvent(75,555,"CBaseDoor",false,IsDoor:true),
            new SpawnEvent(75,970,"CFuncTrackTrain",false),
            new SpawnEvent(75,800,"CDynamicProp",false),
            new VisibilityEvent(75,800,false),
            new MoveEvent(76,555,new Vec3(101,40,50)),
            new MoveEvent(76,970,new Vec3(-3643,3180,1320)),
            new MoveEvent(77,555,new Vec3(101,44,50)),
            new MoveEvent(77,970,new Vec3(-3643,3180,1324)),
            new LookEvent(76,970,new Vec3(0,180,0)),
        };
        var playback = PlaybackDocument.Build([],events,precache,25);
        using var stream = new MemoryStream();
        using (var writer = new Utf8JsonWriter(stream))
        {
            writer.WriteStartObject(); playback.Write(writer); writer.WriteEndObject();
        }
        using var document = JsonDocument.Parse(stream.ToArray());
        var objects = document.RootElement.GetProperty("objects").EnumerateArray().ToArray();
        Assert.Contains(objects,row => row.GetProperty("type").GetString()=="move"
            && row.GetProperty("e").GetInt32()==555 && row.GetProperty("y").GetSingle()==0);
        Assert.Contains(objects,row => row.GetProperty("type").GetString()=="move"
            && row.GetProperty("e").GetInt32()==970 && row.GetProperty("z").GetSingle()==2320);
        Assert.Contains(objects,row => row.GetProperty("type").GetString()=="turn"
            && row.GetProperty("e").GetInt32()==970 && row.GetProperty("yaw").GetSingle()==90);
        Assert.Equal(6,objects.Count(row => row.GetProperty("type").GetString()=="move"));
        Assert.Equal(2,objects.Count(row => row.GetProperty("type").GetString()=="turn"));
        Assert.Contains(objects,row => row.GetProperty("type").GetString()=="spawn"
            && row.GetProperty("e").GetInt32()==555 && row.GetProperty("t").GetInt32()==75
            && row.GetProperty("parent").GetInt32()==970);
        Assert.Contains(objects,row => row.GetProperty("type").GetString()=="spawn"
            && row.GetProperty("e").GetInt32()==800 && row.GetProperty("t").GetInt32()==75
            && row.GetProperty("mapCreatedId").GetInt32()==2764 && row.GetProperty("parent").GetInt32()==555);
        var visibility = objects.Where(row => row.GetProperty("type").GetString()=="visibility").ToArray();
        Assert.Equal(new[]{false,true,false},visibility.Select(row => row.GetProperty("hidden").GetBoolean()));
    }

    [Fact]
    public void Player_id_owns_its_ticks_and_object_owners()
    {
        var players = new[]
        {
            new DemoPlayer(12, 26, 30, "peazel", "STEAM_0:0:10", "76561197960265748", false, false),
            new DemoPlayer(13, 33, 43, "A1L", "STEAM_0:0:11", "76561197960265750", false, false),
        };
        var events = new GameEvent[]
        {
            new SpawnEvent(0, 27, "player", true, P: 12),
            new MoveEvent(10, 27, new Vec3(1, 2, 3), P: 12),
            new PlayerVehicleEvent(11, 27, 90, 91, P: 12),
            new PlayerVehicleEvent(12, 27, null, null, P: 12),
            new SpawnEvent(
                20,
                90,
                "prop_vehicle_jeep",
                false,
                P: 12,
                IsVehicle: true,
                Origin: new Vec3(4, 5, 6)),
            new SpawnEvent(0, 34, "player", true, P: 13),
            new MoveEvent(10, 34, new Vec3(7, 8, 9), P: 13),
        };

        var playback = PlaybackDocument.Build(players, events, new PrecacheTables(), 1);
        using var stream = new MemoryStream();
        using (var writer = new Utf8JsonWriter(stream))
        {
            writer.WriteStartObject();
            playback.Write(writer);
            writer.WriteEndObject();
        }

        using var document = JsonDocument.Parse(stream.ToArray());
        var playerRows = document.RootElement.GetProperty("players").EnumerateArray().ToDictionary(
            player => player.GetProperty("id").GetInt32());
        Assert.Contains(10, Ticks(playerRows[12]));
        Assert.Contains(10, Ticks(playerRows[13]));
        var vehicleTicks = playerRows[12].GetProperty("ticks").EnumerateArray()
            .Where(tick => tick.TryGetProperty("vehicle", out _)).ToArray();
        Assert.Equal(2, vehicleTicks.Length);
        Assert.Equal(90, vehicleTicks[0].GetProperty("vehicle").GetInt32());
        Assert.Equal(91, vehicleTicks[0].GetProperty("parent").GetInt32());
        Assert.Equal(-1, vehicleTicks[1].GetProperty("vehicle").GetInt32());
        Assert.Equal(-1, vehicleTicks[1].GetProperty("parent").GetInt32());
        var vehicle = document.RootElement.GetProperty("objects").EnumerateArray()
            .Single(row => row.GetProperty("type").GetString() == "spawn"
                           && row.GetProperty("e").GetInt32() == 90);
        Assert.Equal(12, vehicle.GetProperty("p").GetInt32());
    }

    private static int[] Ticks(JsonElement player) =>
        player.GetProperty("ticks").EnumerateArray()
            .Where(tick => tick.TryGetProperty("x", out _))
            .Select(tick => tick.GetProperty("t").GetInt32())
            .ToArray();

    [ShortDemoFact]
    public void Json_groups_ticks_per_player_and_world_objects()
    {
        var jsonPath = Path.Combine(Path.GetTempPath(), $"gmoddem-playback-{Guid.NewGuid():N}.json");
        try
        {
            using (var output = File.Create(jsonPath))
            using (var reader = DemoReader.Open(RealDemo.ShortPath))
            {
                new JsonExporter(indent: false).Write(output, reader, new FileInfo(RealDemo.ShortPath).Length, outputPath: jsonPath);
            }

            using var document = JsonDocument.Parse(File.ReadAllBytes(jsonPath));
            var root = document.RootElement;
            Assert.Equal(PlaybackDocument.SchemaVersion, root.GetProperty("schemaVersion").GetInt32());
            Assert.False(root.TryGetProperty("events", out _));
            Assert.True(root.GetProperty("audio").GetProperty("pcm").ValueKind == JsonValueKind.False);

            var recorderName = RealDemo.ClientName(RealDemo.ShortPath);
            var recorder = root.GetProperty("players").EnumerateArray()
                .Single(p => p.GetProperty("name").GetString() == recorderName);
            Assert.True(recorder.GetProperty("steamId64").GetString() is { Length: > 0 });
            Assert.True(recorder.GetProperty("entity").GetInt32() >= 1);

            var ticks = recorder.GetProperty("ticks").EnumerateArray().ToList();
            Assert.True(ticks.Count > 5, $"recorder ticks={ticks.Count}");
            var posed = ticks.First(t => t.GetProperty("t").GetInt32() > 0 && t.TryGetProperty("x", out _));
            Assert.True(posed.TryGetProperty("pitch", out _) || posed.TryGetProperty("yaw", out _));
            Assert.True(posed.GetProperty("time").GetDouble() > 0);
            Assert.Contains(ticks, t => t.TryGetProperty("weapon", out var w) && w.GetString() is { Length: > 0 });
            Assert.Contains(ticks, t => t.TryGetProperty("x", out _));

            if (recorder.TryGetProperty("chat", out var chatProp))
            {
                foreach (var line in chatProp.EnumerateArray())
                {
                    Assert.False(line.TryGetProperty("luaNetId", out _));
                    Assert.True(line.GetProperty("text").GetString() is { Length: > 0 });
                }
            }

            var objects = root.GetProperty("objects").EnumerateArray().ToList();
            var vehicle = objects.FirstOrDefault(e =>
                e.GetProperty("type").GetString() == "spawn"
                && e.TryGetProperty("kind", out var kind) && kind.GetString() == "vehicle"
                && e.TryGetProperty("p", out _));
            Assert.Equal(JsonValueKind.Object, vehicle.ValueKind);
            Assert.True(vehicle.TryGetProperty("x", out _) || vehicle.TryGetProperty("model", out _));

            Assert.Contains(objects, e => e.GetProperty("type").GetString() == "door");

            Assert.True(root.GetProperty("models").EnumerateObject().Count() > 50);
            var recorderSpawn = ticks.First(t => t.TryGetProperty("spawn", out var spawn) && spawn.GetBoolean());
            Assert.True(recorderSpawn.GetProperty("model").GetString()!.Contains("models/", StringComparison.OrdinalIgnoreCase));
        }
        finally
        {
            if (File.Exists(jsonPath))
            {
                File.Delete(jsonPath);
            }

            var audioDir = Path.ChangeExtension(jsonPath, ".audio");
            if (Directory.Exists(audioDir))
            {
                Directory.Delete(audioDir, true);
            }

            var luaDir = Path.ChangeExtension(jsonPath, ".lua");
            if (Directory.Exists(luaDir))
            {
                Directory.Delete(luaDir, true);
            }

            var netPath = Path.ChangeExtension(jsonPath, ".net.jsonl");
            if (File.Exists(netPath))
            {
                File.Delete(netPath);
            }
        }
    }

    [RealDemoFact]
    public void Long_demo_exports_named_game_events_and_voice_sidecars()
    {
        var collector = DataTableTests.Interpret(RealDemo.Path);
        Assert.NotNull(collector.GameEvents);
        Assert.Contains(collector.GameEvents!.Events, e => e.Name == "player_say");
        Assert.Contains(collector.GameEvents.Events, e => e.Name == "server_spawn");
        Assert.Contains(collector.Events.OfType<SourceGameEvent>(), e => e.Name is { Length: > 0 });
        Assert.True(collector.Voice.Packets.Count > 0, "long demo should contain voice packets");

        var jsonPath = Path.Combine(Path.GetTempPath(), $"gmoddem-voice-{Guid.NewGuid():N}.json");
        var audioDir = Path.ChangeExtension(jsonPath, ".audio");
        try
        {
            var clips = collector.Voice.WriteSidecars(jsonPath, collector.Players, 66.6666666667);
            var slots = collector.Voice.Packets.Select(p => p.Slot).Distinct().Count();
            Assert.True(clips.Count >= slots, $"clips={clips.Count} slots={slots}");
            Assert.True(Directory.Exists(audioDir));
            Assert.Contains(collector.Voice.Packets, p => SteamVoice.TryParse(p.Payload, out _));
            foreach (var clip in clips)
            {
                var full = Path.GetFullPath(Path.Combine(Path.GetDirectoryName(jsonPath)!, clip.File));
                Assert.True(File.Exists(full), clip.File);
                Assert.Contains($"_{clip.StartTick}.wav", clip.File.Replace('\\', '/'));
                var bytes = File.ReadAllBytes(full);
                Assert.True(WaveFile.IsRiffWave(bytes), clip.File);
                Assert.Equal(VoiceDecoder.OutputSampleRate, BitConverter.ToInt32(bytes, 24));
                Assert.True(bytes.Length > 44 + 800, clip.File);
                var peak = 0;
                var zc = 0;
                short prev = 0;
                var samples = (bytes.Length - 44) / 2;
                for (var i = 44; i + 1 < bytes.Length; i += 2)
                {
                    var s = BitConverter.ToInt16(bytes, i);
                    peak = Math.Max(peak, Math.Abs((int)s));
                    if (i > 44 && Math.Sign(s) != Math.Sign(prev))
                    {
                        zc++;
                    }

                    prev = s;
                }

                Assert.True(peak > 200, $"quiet wav {clip.File} peak={peak}");
                var zcr = zc / (double)Math.Max(samples, 1);
                Assert.True(zcr < 0.45, $"hiss-like wav {clip.File} zcr={zcr:F3}");
                Assert.True(clip.SteamId64 is { Length: > 0 } || clip.Slot >= 0);
            }

            using (var output = File.Create(jsonPath))
            using (var reader = DemoReader.Open(RealDemo.Path))
            {
                new JsonExporter(indent: false).Write(output, reader, new FileInfo(RealDemo.Path).Length, outputPath: jsonPath);
            }

            using var document = JsonDocument.Parse(File.ReadAllBytes(jsonPath));
            var audio = document.RootElement.GetProperty("audio");
            Assert.Equal(JsonValueKind.True, audio.GetProperty("pcm").ValueKind);
            Assert.Equal("wav", audio.GetProperty("format").GetString());
            Assert.Equal(VoiceDecoder.OutputSampleRate, audio.GetProperty("sampleRate").GetInt32());
            var jsonClips = audio.GetProperty("clips").EnumerateArray().ToList();
            Assert.Equal(clips.Count, jsonClips.Count);
            Assert.All(jsonClips, clip => Assert.True(clip.TryGetProperty("proximity", out _)));
            var speaker = document.RootElement.GetProperty("players").EnumerateArray()
                .First(p => p.TryGetProperty("voice", out var voice) && voice.GetArrayLength() > 0);
            var line = speaker.GetProperty("voice")[0];
            Assert.True(line.GetProperty("time").GetDouble() >= 0);
            Assert.True(line.TryGetProperty("proximity", out _));
            Assert.Contains($"_{line.GetProperty("t").GetInt32()}.wav", line.GetProperty("file").GetString());
            Assert.False(speaker.TryGetProperty("voiceFile", out _));
        }
        finally
        {
            if (Directory.Exists(audioDir))
            {
                Directory.Delete(audioDir, true);
            }

            if (File.Exists(jsonPath))
            {
                File.Delete(jsonPath);
            }
        }
    }
}
