using GmodDemo.Parser;

namespace GmodDemo.Parser.Tests;

public sealed class DataTableTests
{
    [Fact]
    public void Vehicle_entry_restores_class_skin_and_body_then_applies_selected_baseline_and_deltas()
    {
        var tables = new DataTableSet();
        var cls = new ServerClass { ClassId = 0, Name = "CPropVehicleDriveable", DataTableName = "DT_Test" };
        foreach (var name in new[] { "m_nSkin", "m_nBody" })
            cls.Flattened.Add(new(cls.Flattened.Count, "DT_BaseAnimating", name,
                new(SendPropType.Int, name, SendPropFlags.Unsigned, null, null, 0, 0, 0, 8), null));
        tables.Classes.Add(cls);
        tables.ClassesById[0] = cls;
        var baselines = new Dictionary<int, byte[]>
        {
            [0] = PackBits((1,1),(0,6),(11,8),(1,1),(0,6),(22,8),(0,1)),
        };
        var world = new EntityWorld { InstanceBaselines = baselines };
        world.AttachTables(tables);
        world.ForceMode(PropIndexMode.Source2013);
        static byte[] Enter(int baseline = 0, bool promote = false, int? skin = null, int? body = null)
        {
            var data = new List<(int,int)> { (4,6),(0,1),(1,1),(0,1),(7,10) };
            if (skin is { } s) data.AddRange([(1,1),(0,6),(s,8)]);
            if (body is { } b) data.AddRange([(1,1),(skin is null ? 1 : 0,6),(b,8)]);
            data.Add((0,1));
            var fields = new List<(int,int)>
            {
                ((int)NetMessageType.SvcPacketEntities,6),(32,13),(1,1),(1,32),
                (baseline,1),(1,13),(data.Sum(v => v.Item2),24),(promote ? 1 : 0,1),
            };
            fields.AddRange(data);
            return PackBits(fields.ToArray());
        }
        void Check(int tick, byte[] packet, int skin, int body)
        {
            var state = Assert.Single(world.ApplyPacket(tick, packet, null).OfType<EntityStateChange>());
            Assert.Equal(skin, state.Skin);
            Assert.Equal(body, state.Body);
            Assert.Equal(skin, world.Snapshot(1)?.Skin);
            Assert.Equal(body, world.Snapshot(1)?.Body);
        }
        Check(0, Enter(), 11, 22);
        Check(1, Enter(promote: true, skin: 12, body: 0), 12, 0);
        Check(2, Enter(baseline: 1), 12, 0);
        Check(3, Enter(), 11, 22);
        Check(4, Enter(skin: 0), 0, 22);
        baselines[0] = PackBits((1,1),(0,6),(9,8),(1,1),(0,6),(3,8),(0,1));
        Check(5, Enter(), 9, 3);
        Check(6, Enter(baseline: 1), 12, 0);
        Assert.Equal(0, world.Stats.PacketsFailed);
    }

    [Fact]
    public void Bits_for_array_count_match_the_engine()
    {
        Assert.Equal(0, PropDecoder.BitsForCount(0));
        Assert.Equal(1, PropDecoder.BitsForCount(1));
        Assert.Equal(6, PropDecoder.BitsForCount(32));
        Assert.Equal(12, PropDecoder.BitsForCount(2048));
    }

    [Fact]
    public void Sendprop_strings_are_nine_bit_length_then_bytes()
    {
        var packed = PackBits((4, 9), ((int)'t', 8), ((int)'e', 8), ((int)'s', 8), ((int)'t', 8));
        var reader = new BitReader(packed);
        var prop = new FlattenedProp(
            0,
            "DT_Test",
            "m_strScriptName",
            new SendPropDef(SendPropType.String, "m_strScriptName", 0, null, null, 0, 0f, 0f, 0),
            null);

        var value = PropDecoder.Decode(ref reader, prop);

        Assert.Equal("test", Assert.IsType<PropValue.String>(value).Value);
        Assert.Equal(9 + 32, reader.BitsRead);
        Assert.False(reader.IsOverflowed);
    }

    [Fact]
    public void Ehandle_uses_gmod_13_plus_10_bits()
    {
        Assert.Null(EntityIdentity.DecodeHandle(0));
        Assert.Null(EntityIdentity.DecodeHandle(EntityIdentity.InvalidHandle));
        Assert.Equal(70, EntityIdentity.DecodeHandle(70));
        Assert.Equal(70, EntityIdentity.DecodeHandle(70 | (3 << 13)));
        Assert.True(EntityIdentity.IsWeaponClass("CWeaponSWEP"));
        Assert.True(EntityIdentity.IsDoorClass("CPropDoorRotating"));
        Assert.Equal("open", EntityIdentity.DoorStateName(2));
        Assert.Equal(true, EntityIdentity.DoorIsOpen(2));
        Assert.Equal(false, EntityIdentity.DoorIsOpen(0));
    }

    [Fact]
    public void Zero_bit_int64_uses_the_full_64_bit_width()
    {
        var packed = PackBits((1, 1), (unchecked((int)0x89abcdef), 32), (0x01234567, 31), (1, 1));
        var reader = new BitReader(packed);
        var prop = new FlattenedProp(
            0,
            "DT_Test",
            "m_iLargeValue",
            new SendPropDef(SendPropType.Int64, "m_iLargeValue", 0, null, null, 0, 0f, 0f, 0),
            null);

        var value = Assert.IsType<PropValue.Int>(PropDecoder.Decode(ref reader, prop));

        Assert.Equal(-0x0123456789abcdef, value.Value);
        Assert.Equal(64, reader.BitsRead);
        Assert.Equal(1, reader.ReadOneBit());
    }

    [Fact]
    public void Dormant_weapon_deletions_and_reused_indexes_end_the_exact_handle()
    {
        var tables = new DataTableSet();
        var serverClass = new ServerClass
        {
            ClassId = 0, Name = "CWeaponSWEP", DataTableName = "DT_Test",
        };
        tables.Classes.Add(serverClass);
        tables.ClassesById[0] = serverClass;
        var world = new EntityWorld();
        world.AttachTables(tables);
        world.ForceMode(PropIndexMode.Source2013);
        static byte[] Packet(int updated, params (int Value,int Width)[] body)
        {
            var fields = new List<(int,int)>
            {
                ((int)NetMessageType.SvcPacketEntities,6),(64,13),(1,1),(1,32),
                (0,1),(updated,13),(body.Sum(v => v.Width),24),(0,1),
            };
            fields.AddRange(body);
            return PackBits(fields.ToArray());
        }
        world.ApplyPacket(1,Packet(1,(4,6),(0,1),(1,1),(0,1),(7,10),(0,1),(0,1)),null);
        world.ApplyPacket(2,Packet(1,(4,6),(1,1),(0,1),(0,1)),null);
        Assert.NotNull(world.Snapshot(1));
        Assert.DoesNotContain(world.WeaponLog,e => e.Kind == "gone");
        var deleted = world.ApplyPacket(3,Packet(0,(1,1),(1,13),(0,1)),null);
        Assert.Contains(deleted,c => c is EntityRemoveChange { Entity: 1, Deleted: true });
        Assert.Null(world.Snapshot(1));
        Assert.Contains(world.WeaponLog,e => e is { Tick: 3, Kind: "gone", Entity: 1, Serial: 7 });
        world.ApplyPacket(4,Packet(1,(4,6),(0,1),(1,1),(0,1),(8,10),(0,1),(0,1)),null);
        world.ApplyPacket(5,Packet(1,(4,6),(0,1),(1,1),(0,1),(9,10),(0,1),(0,1)),null);
        Assert.Equal(9,world.Snapshot(1)?.Serial);
        Assert.Contains(world.WeaponLog,e => e is { Tick: 5, Kind: "gone", Entity: 1, Serial: 8 });
        Assert.DoesNotContain(world.WeaponLog,e => e is { Kind: "gone", Serial: 9 });
        Assert.Equal(0,world.Stats.PacketsFailed);
    }

    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    public void Gmod_table_variants_leave_the_next_property_aligned(int cleared)
    {
        var fields = new List<(int, int)> { (8,12), (cleared,1) };
        for (var type = 0; type < 8; ++type)
        {
            fields.Add((type+20,12));
            fields.Add((type,3));
            switch (type)
            {
                case 1: fields.Add((BitConverter.SingleToInt32Bits(1.5f),32)); break;
                case 2: fields.Add((-17,32)); break;
                case 3: fields.Add((1,1)); break;
                case 4:
                case 5:
                    fields.AddRange([(BitConverter.SingleToInt32Bits(1),32),
                        (BitConverter.SingleToInt32Bits(2),32),
                        (BitConverter.SingleToInt32Bits(3),32)]);
                    break;
                case 6: fields.Add((70 | (3<<13),23)); break;
                case 7: fields.AddRange([(4,9),('t',8),('e',8),('s',8),('t',8)]); break;
            }
        }
        fields.Add((0x5a,8));
        var reader = new BitReader(PackBits(fields.ToArray()));
        var prop = new FlattenedProp(0,"DT_BaseEntity","m_GMOD_DataTable",
            new SendPropDef(SendPropType.GmodTable,"m_GMOD_DataTable",0,null,null,0,0,0,0),null);
        var value = Assert.IsType<PropValue.GmodTable>(PropDecoder.Decode(ref reader,prop));
        Assert.Equal(cleared != 0,value.Cleared);
        Assert.Equal(8,value.Values.Count);
        Assert.Null(value.Values[20]);
        Assert.Equal(1.5f,Assert.IsType<PropValue.Float>(value.Values[21]).Value);
        Assert.Equal(-17,Assert.IsType<PropValue.Int>(value.Values[22]).Value);
        Assert.Equal(1,Assert.IsType<PropValue.Int>(value.Values[23]).Value);
        Assert.Equal(new Vec3(1,2,3),Assert.IsType<PropValue.Vector>(value.Values[24]).Value);
        Assert.Equal(new Vec3(1,2,3),Assert.IsType<PropValue.Vector>(value.Values[25]).Value);
        Assert.Equal(70 | (3<<13),Assert.IsType<PropValue.Int>(value.Values[26]).Value);
        Assert.Equal("test",Assert.IsType<PropValue.String>(value.Values[27]).Value);
        Assert.Equal(0x5au,reader.ReadUBitLong(8));
        Assert.False(reader.IsOverflowed);
    }

    [RealDemoFact]
    public void Captured_demo_entity_packets_decode_through_the_complete_recording()
    {
        var collector = Interpret(RealDemo.Path);
        Assert.True(collector.EntityStats.Packets > 0);
        Assert.Equal(0,collector.EntityStats.PacketsFailed);
        Assert.NotEmpty(collector.Entities.ViewModelLog);
    }

    [Fact]
    public void Player_reentry_exports_flags_from_the_selected_baseline_not_the_last_pose()
    {
        var tables = new DataTableSet();
        var serverClass = new ServerClass
        {
            ClassId = 0, Name = "CGMOD_Player", DataTableName = "DT_Test", IsPlayer = true,
        };
        serverClass.Flattened.Add(new FlattenedProp(0,"DT_BasePlayer","m_fFlags",
            new SendPropDef(SendPropType.Int,"m_fFlags",SendPropFlags.Unsigned,
                null,null,0,0,0,10),null));
        tables.Classes.Add(serverClass);
        tables.ClassesById[0] = serverClass;
        var world = new EntityWorld();
        world.AttachTables(tables);
        world.ForceMode(PropIndexMode.Source2013);
        static byte[] Enter(int baseline, bool updateBaseline, int? flags)
        {
            var body = new List<(int,int)> { (4,6),(0,1),(1,1),(0,1),(7,10) };
            if (flags is { } value) body.AddRange([(1,1),(0,6),(value,10)]);
            body.Add((0,1));
            var fields = new List<(int,int)>
            {
                ((int)NetMessageType.SvcPacketEntities,6),(32,13),(1,1),(1,32),
                (baseline,1),(1,13),(body.Sum(v => v.Item2),24),(updateBaseline ? 1 : 0,1),
            };
            fields.AddRange(body);
            return PackBits(fields.ToArray());
        }
        void AssertFlags(int tick, byte[] packet, int expected)
        {
            var changes = world.ApplyPacket(tick,packet,null);
            var state = Assert.Single(changes.OfType<EntityStateChange>());
            Assert.Equal(expected,state.Flags);
            Assert.Equal(expected,world.Snapshot(1)?.Flags);
        }
        AssertFlags(0,Enter(0,false,null),0);
        AssertFlags(1,Enter(0,true,257),257); // Promote standing into slot 1.
        AssertFlags(2,Enter(1,true,263),263); // Promote crouched into slot 0.
        AssertFlags(3,Enter(1,false,null),257);
        AssertFlags(4,Enter(0,false,null),263);
        AssertFlags(5,Enter(1,false,null),257);
        AssertFlags(6,Enter(0,false,0),0);
        Assert.Equal(0,world.Stats.PacketsFailed);
    }

    [Fact]
    public void Player_reentry_restores_alive_baseline_after_death_without_explicit_life_state()
    {
        var tables = new DataTableSet();
        var cls = new ServerClass { ClassId = 0, Name = "CGMOD_Player", DataTableName = "DT_Test", IsPlayer = true };
        cls.Flattened.Add(new(0, "DT_BasePlayer", "m_lifeState",
            new(SendPropType.Int, "m_lifeState", SendPropFlags.Unsigned, null, null, 0, 0, 0, 3), null));
        tables.Classes.Add(cls);
        tables.ClassesById[0] = cls;
        var world = new EntityWorld();
        world.AttachTables(tables);
        world.ForceMode(PropIndexMode.Source2013);
        static byte[] Enter(int baseline, bool promote, int? life)
        {
            var body = new List<(int, int)> { (4, 6), (0, 1), (1, 1), (0, 1), (7, 10) };
            if (life is { } value) body.AddRange([(1, 1), (0, 6), (value, 3)]);
            body.Add((0, 1));
            var fields = new List<(int, int)>
            {
                ((int)NetMessageType.SvcPacketEntities, 6), (32, 13), (1, 1), (1, 32),
                (baseline, 1), (1, 13), (body.Sum(v => v.Item2), 24), (promote ? 1 : 0, 1),
            };
            fields.AddRange(body);
            return PackBits(fields.ToArray());
        }
        void AssertLife(int tick, byte[] packet, int expected)
        {
            var state = Assert.Single(world.ApplyPacket(tick, packet, null).OfType<EntityStateChange>());
            Assert.Equal(expected, state.LifeState);
            Assert.Equal(expected, world.Snapshot(1)?.LifeState);
        }
        AssertLife(0, Enter(0, true, null), 0);
        AssertLife(1, Enter(1, true, 2), 2);
        AssertLife(2, Enter(1, false, null), 0);
        AssertLife(3, Enter(0, false, null), 2);
        AssertLife(4, Enter(0, false, 0), 0);
        Assert.Equal(0, world.Stats.PacketsFailed);
    }

    [Fact]
    public void Weapon_reentry_restores_identity_and_model_for_the_new_serial()
    {
        var tables = new DataTableSet();
        var cls = new ServerClass { ClassId = 0, Name = "CWeaponSWEP", DataTableName = "DT_Test" };
        cls.Flattened.Add(new(0, "DT_Test", "m_strScriptName",
            new(SendPropType.String, "m_strScriptName", 0, null, null, 0, 0, 0, 0), null));
        cls.Flattened.Add(new(1, "DT_Test", "m_nModelIndex",
            new(SendPropType.Int, "m_nModelIndex", SendPropFlags.Unsigned, null, null, 0, 0, 0, 10), null));
        tables.Classes.Add(cls);
        tables.ClassesById[0] = cls;
        var world = new EntityWorld();
        world.AttachTables(tables);
        world.ForceMode(PropIndexMode.Source2013);
        static byte[] Enter(int baseline, bool promote, int serial, string? script = null, int model = 0)
        {
            var body = new List<(int, int)> { (4, 6), (0, 1), (1, 1), (0, 1), (serial, 10) };
            if (script is not null)
            {
                body.AddRange([(1, 1), (0, 6), (script.Length, 9)]);
                body.AddRange(script.Select(c => ((int)c, 8)));
                body.AddRange([(1, 1), (0, 6), (model, 10)]);
            }
            body.Add((0, 1));
            var fields = new List<(int, int)>
            {
                ((int)NetMessageType.SvcPacketEntities, 6), (32, 13), (1, 1), (1, 32),
                (baseline, 1), (1, 13), (body.Sum(v => v.Item2), 24), (promote ? 1 : 0, 1),
            };
            fields.AddRange(body);
            return PackBits(fields.ToArray());
        }
        world.ApplyPacket(0, Enter(0, true, 958, "weapon_perp_phys_glock17", 321), null);
        world.ApplyPacket(1, Enter(1, true, 959), null);
        Assert.Equal(959, world.Snapshot(1)?.Serial);
        Assert.Equal(321, world.Snapshot(1)?.ModelIndex);
        Assert.Equal("weapon_perp_phys_glock17", world.HandleNames[(1, 959)]);
        world.ApplyPacket(2, Enter(0, false, 960, "weapon_perp_fists", 123), null);
        Assert.Equal(123, world.Snapshot(1)?.ModelIndex);
        Assert.Equal("weapon_perp_fists", world.HandleNames[(1, 960)]);
        Assert.Equal(0, world.Stats.PacketsFailed);
    }

    [Fact]
    public void Leaving_pvs_retains_state_for_a_later_delta_but_deletion_does_not()
    {
        var tables = new DataTableSet();
        var serverClass = new ServerClass
        {
            ClassId = 0, Name = "CPhysicsProp", DataTableName = "DT_Test",
        };
        serverClass.Flattened.Add(new FlattenedProp(0,"DT_Test","m_nModelIndex",
            new SendPropDef(SendPropType.Int,"m_nModelIndex",SendPropFlags.Unsigned,
                null,null,0,0,0,10),null));
        tables.Classes.Add(serverClass);
        tables.ClassesById[0] = serverClass;
        var world = new EntityWorld();
        world.AttachTables(tables);
        world.ForceMode(PropIndexMode.Source2013);
        static byte[] Packet(params (int Value,int Width)[] body)
        {
            var fields = new List<(int,int)>
            {
                ((int)NetMessageType.SvcPacketEntities,6),(32,13),(1,1),(1,32),
                (0,1),(1,13),(body.Sum(v => v.Width),24),(0,1),
            };
            fields.AddRange(body);
            return PackBits(fields.ToArray());
        }
        world.ApplyPacket(1,Packet((4,6),(0,1),(1,1),(0,1),(7,10),
            (1,1),(0,6),(42,10),(0,1)),null);
        Assert.Equal(42,world.Snapshot(1)?.ModelIndex);
        var leave = world.ApplyPacket(2,Packet((4,6),(1,1),(0,1)),null);
        Assert.Contains(leave,c => c is EntityRemoveChange { Deleted: false });
        Assert.Equal(42,world.Snapshot(1)?.ModelIndex);
        var reappear = world.ApplyPacket(3,Packet((4,6),(0,1),(0,1),
            (1,1),(0,6),(43,10),(0,1)),null);
        Assert.Contains(reappear,c => c is EntitySpawnChange { Serial: 7, ClassName: "CPhysicsProp" });
        Assert.Equal(43,world.Snapshot(1)?.ModelIndex);
        world.ApplyPacket(4,Packet((4,6),(1,1),(1,1)),null);
        Assert.Null(world.Snapshot(1));
        Assert.Equal(0,world.Stats.PacketsFailed);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void Net_file_uses_gmod_request_fields(bool requested)
    {
        var packed = requested
            ? PackBits(((int)NetMessageType.NetFile, 6), (123, 32), (1, 1), (0, 1), (456, 32))
            : PackBits(((int)NetMessageType.NetFile, 6), (123, 32), (0, 1));

        var result = new NetMessageWalker().Walk(packed);

        Assert.Equal(PacketWalkStatus.Clean, result.Status);
        var message = Assert.Single(result.Messages);
        Assert.Equal(NetMessageType.NetFile, message.Type);
        Assert.Equal(requested ? 72 : 39, message.LengthBits);
    }

    [ShortDemoFact]
    public void Short_demo_send_tables_parse_cleanly()
    {
        var tables = LoadTables(RealDemo.ShortPath);

        Assert.False(tables.Overflowed);
        Assert.InRange(tables.BitsLeft, 0, 7);
        Assert.True(tables.Tables.Count > 50, $"tables={tables.Tables.Count}");
        Assert.True(tables.Classes.Count > 20, $"classes={tables.Classes.Count}");
        Assert.True(tables.HasBaseEntity);
        Assert.True(tables.HasOrigin);
        Assert.Contains(tables.Classes, c => c.IsPlayer);
        Assert.Contains(tables.Tables.SelectMany(t => t.Props), p => p.Name == "m_nSequence");
    }

    [ShortDemoFact]
    public void First_enter_pvs_class_id_matches_a_server_class()
    {
        var tables = LoadTables(RealDemo.ShortPath);
        var world = new EntityWorld();
        world.AttachTables(tables);

        using var reader = DemoReader.Open(RealDemo.ShortPath);
        foreach (var command in reader.ReadCommands())
        {
            if (command is not PacketCommand packet)
            {
                continue;
            }

            var probe = world.ProbeFirstEnter(packet.Payload);
            if (probe is null)
            {
                continue;
            }

            Assert.True(
                probe.Value.KnownClass,
                $"classId={probe.Value.ClassId} serial={probe.Value.Serial} bits={probe.Value.ClassBits} updated={probe.Value.UpdatedEntries} delta={probe.Value.IsDelta}");
            Assert.Equal("CWorld", probe.Value.ClassName);
            Assert.False(probe.Value.IsDelta);
            Assert.True(probe.Value.UpdatedEntries > 100, $"updated={probe.Value.UpdatedEntries}");

            world.Calibrate([(packet.Tick, packet.Payload, null)]);
            var progress = world.ProbeApplyUntilFail(packet.Payload, world.Mode);
            Assert.True(progress.StartsWith("all ", StringComparison.Ordinal), progress);
            Assert.Contains("leftover=0", progress);
            Assert.True(progress.Contains("entered=1269") || progress.Contains($"entered={probe.Value.UpdatedEntries}"), progress);
            return;
        }

        Assert.Fail("no PacketEntities message found");
    }

    [ShortDemoFact]
    public void Short_demo_full_snapshot_enters_world_and_map_entities()
    {
        var collector = Interpret(RealDemo.ShortPath);
        var stats = collector.EntityStats;

        Assert.NotEqual(PropIndexMode.Unknown, stats.Mode);
        Assert.True(stats.Packets > 0, "no PacketEntities messages");
        Assert.True(stats.EntitiesEntered >= 100, $"entered={stats.EntitiesEntered} failed={stats.PacketsFailed} mode={stats.Mode}");
        Assert.True(stats.PacketsClean > 0, $"clean={stats.PacketsClean} failed={stats.PacketsFailed} packets={stats.Packets}");
        Assert.True(stats.PacketsFailed < stats.Packets, $"every packet failed: clean={stats.PacketsClean} failed={stats.PacketsFailed}");
        Assert.Contains(collector.Events.OfType<SpawnEvent>(), e => e.ClassName == "CWorld");
        Assert.True(stats.OriginsTrusted, $"cameraMatches={stats.CameraMatches} recorder={collector.Entities.RecorderEntity}");
        Assert.NotNull(collector.Entities.RecorderEntity);
        var recorderEntity = collector.Entities.RecorderEntity!.Value;

        var cameras = collector.Events.OfType<CameraMoveEvent>().Where(e => e.T > 0).ToList();
        Assert.True(cameras.Count > 10, $"camera events={cameras.Count}");
        var camera = cameras[cameras.Count / 2];
        var nearby = collector.Events.OfType<MoveEvent>().Where(m =>
            m.T >= camera.T - 15 && m.T <= camera.T + 15
            && Math.Abs(m.Origin.X - camera.Origin.X) < 128
            && Math.Abs(m.Origin.Y - camera.Origin.Y) < 128);
        Assert.True(nearby.Any(), $"no entity origin near camera around tick {camera.T}");
        Assert.Contains(nearby, m => m.P is not null && m.Entity == recorderEntity);
    }

    [ShortDemoFact]
    public void Custom_lua_chat_stays_in_the_net_log()
    {
        var collector = Interpret(RealDemo.ShortPath);
        var luaAscii = collector.NetLog.Rows
            .Where(r => r.Type == "lua" && r.Ascii is { Length: > 0 })
            .Select(r => r.Ascii!)
            .ToList();
        Assert.True(luaAscii.Count > 0, "expected Lua net payloads in the log");
        Assert.DoesNotContain(collector.Events.OfType<ChatEvent>(), e => luaAscii.Contains(e.Text));
    }

    [ShortDemoFact]
    public void Player_entities_emit_move_look_and_anim()
    {
        var collector = Interpret(RealDemo.ShortPath);
        var playerEntities = collector.Events.OfType<SpawnEvent>()
            .Where(s => s.IsPlayer)
            .Select(s => s.Entity)
            .Distinct()
            .ToList();
        Assert.True(playerEntities.Count >= 2, $"playerEntities={playerEntities.Count}");
        var recorderEntity = collector.Entities.RecorderEntity;
        Assert.NotNull(recorderEntity);
        Assert.Contains(playerEntities, e => e == recorderEntity);
        Assert.Contains(collector.Events.OfType<MoveEvent>(), m => m.Entity == recorderEntity && m.P is not null);
        Assert.Contains(collector.Events.OfType<MoveEvent>(), m => playerEntities.Contains(m.Entity) && m.Entity != recorderEntity);
        Assert.Contains(collector.Events.OfType<LookEvent>(), e => playerEntities.Contains(e.Entity));
        Assert.True(collector.Events.OfType<AnimEvent>().Any(), "no animation changes");
        Assert.True(collector.Events.OfType<StateEvent>().Any(), "no state changes");
    }

    [ShortDemoFact]
    public void Every_player_entity_event_carries_a_compact_player_id()
    {
        var collector = Interpret(RealDemo.ShortPath);
        Assert.NotNull(collector.Entities.RecorderEntity);
        var recorderEntity = collector.Entities.RecorderEntity!.Value;

        var players = PlayerDirectory.FromDemo(RealDemo.ShortPath).Snapshot();
        var recorderName = RealDemo.ClientName(RealDemo.ShortPath);
        var recorder = players.Single(p => p.Name == recorderName);
        Assert.Equal(recorderEntity, recorder.Entity);
        Assert.Equal(recorder.Slot + 1, recorder.Entity);
        Assert.True(players.Count(p => p.Slot >= 0) >= 2, $"slotted={players.Count(p => p.Slot >= 0)} total={players.Count}");

        var playerEntities = collector.Events.OfType<SpawnEvent>()
            .Where(s => s.IsPlayer && s.Entity is >= 1 and <= 128)
            .Select(s => s.Entity)
            .ToHashSet();
        Assert.True(playerEntities.Count >= 2, $"playerEntities={playerEntities.Count}");

        var missingEntities = collector.Events
            .Where(e => e.E is { } entity && playerEntities.Contains(entity) && e.P is null)
            .Select(e => e.E!.Value)
            .Distinct()
            .OrderBy(e => e)
            .ToList();
        Assert.True(missingEntities.Count == 0, $"player events without p: [{string.Join(",", missingEntities)}]");
        Assert.Contains(collector.Events.OfType<MoveEvent>(), m => m.Entity != recorderEntity && m.P is not null);
    }

    [ShortDemoFact]
    public void Weapons_register_script_name_and_owner()
    {
        var collector = Interpret(RealDemo.ShortPath);
        var sweps = collector.Events.OfType<SpawnEvent>()
            .Where(s => s.IsWeapon && s.ClassName == "CWeaponSWEP")
            .ToList();
        Assert.True(sweps.Count >= 50, $"sweps={sweps.Count}");
        Assert.Contains(sweps, s => s.Name is { } name && name.StartsWith("weapon_", StringComparison.Ordinal));
        Assert.Contains(sweps, s => s.Owner is >= 1 and <= 128 && s.P is not null);

        var named = sweps.Where(s => s.Name is { Length: > 0 }).Select(s => s.Name!).Distinct().OrderBy(n => n).ToList();
        Assert.True(named.Count >= 2, $"named={string.Join(",", named.Take(12))}");

        var held = collector.Events.OfType<WeaponEvent>().Where(w => w.Name is { Length: > 0 }).ToList();
        Assert.True(held.Count > 0, "no weapon-hold events with a name");
        Assert.Contains(held, w => w.P is not null && w.Weapon is not null);
        Assert.True(
            held.GroupBy(w => w.Entity).Any(g => g.Select(w => w.Name).Distinct().Count() >= 2)
            || named.Count >= 2,
            "expected either a weapon switch or multiple distinct SWEP names");
    }

    [ShortDemoFact]
    public void Doors_register_origin_and_open_state()
    {
        var collector = Interpret(RealDemo.ShortPath);
        var doors = collector.Events.OfType<DoorEvent>().ToList();
        Assert.True(doors.Count >= 50, $"doors={doors.Count}");
        Assert.Contains(doors, d => d.Origin is not null);
        Assert.Contains(doors, d => d.DoorState is 0 or 1 or 2 or 3 or 4);
        Assert.Contains(
            collector.Events.OfType<SpawnEvent>(),
            s => s.IsDoor && s.ClassName is "CPropDoorRotating" or "CBaseDoor");
    }

    [ShortDemoFact]
    public void Vehicles_and_scripted_ents_carry_an_owner_player()
    {
        var collector = Interpret(RealDemo.ShortPath);
        var owned = collector.Events.OfType<SpawnEvent>()
            .Where(s => (s.IsVehicle || s.ClassName.StartsWith("CSENT", StringComparison.Ordinal))
                        && s.P is not null)
            .ToList();
        Assert.True(owned.Count > 0, "no vehicle/SENT spawn with owner p");
        Assert.Contains(owned, s => s.Owner is not null || s.Name is { Length: > 0 });
        Assert.Contains(owned, s =>
            s.P is { } id
            && collector.Players.FirstOrDefault(p => p.Id == id)?.SteamId64 is { Length: > 0 });

        var ownedEntities = owned.Select(s => s.Entity).ToHashSet();
        Assert.Contains(
            collector.Events.OfType<MoveEvent>(),
            m => ownedEntities.Contains(m.Entity) && m.P is not null
                 && collector.Players.FirstOrDefault(p => p.Id == m.P)?.SteamId64 is { Length: > 0 });
    }

    internal static DataTableSet LoadTables(string path)
    {
        using var reader = DemoReader.Open(path);
        foreach (var command in reader.ReadCommands())
        {
            if (command is DataTablesCommand dataTables)
            {
                return DataTableSet.Parse(dataTables.Payload);
            }
        }

        throw new InvalidOperationException("No dem_datatables command.");
    }

    internal static EventCollector Interpret(string path)
    {
        var collector = new EventCollector();
        using var reader = DemoReader.Open(path);
        collector.SetRecorderName(reader.Header.ClientName);
        var players = new PlayerDirectory();
        foreach (var command in reader.ReadCommands())
        {
            switch (command)
            {
                case DataTablesCommand dataTables:
                    collector.AddDataTables(dataTables);
                    break;
                case PacketCommand packet:
                    players.AddPacket(packet.Payload);
                    collector.AddPacket(packet);
                    break;
                case StringTablesCommand stringTables:
                    players.AddStringTablesDump(stringTables.Payload);
                    collector.Precache.AddPayload(stringTables.Payload);
                    break;
                case ConsoleCommand console:
                    collector.AddConsole(console);
                    break;
            }
        }

        collector.BindPlayers(players.Snapshot());
        return collector;
    }

    private static byte[] PackBits(params (int Value, int Bits)[] fields)
    {
        var total = fields.Sum(f => f.Bits);
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
}
