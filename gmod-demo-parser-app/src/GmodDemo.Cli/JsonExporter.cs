using System.Text.Json;

namespace GmodDemo.Parser;

/// <summary>
/// Streams a demo's container structure to JSON. Commands are written as they are read so that
/// output size is not bounded by memory.
/// </summary>
public sealed class JsonExporter(
    bool includeCommands = false,
    bool includeRawPayloads = false,
    int maxCommands = int.MaxValue,
    bool indent = false)
{
    /// <summary>
    /// Writes the document and returns the summary accumulated during the same pass.
    /// </summary>
    public DemoSummary Write(
        Stream output,
        DemoReader reader,
        long? fileLength = null,
        Action<long>? progress = null,
        string? outputPath = null,
        Func<Gluapack, string?, string?>? choosePackFile = null,
        Action<EventCollector>? afterWalk = null)
    {
        ArgumentNullException.ThrowIfNull(output);
        ArgumentNullException.ThrowIfNull(reader);

        using var writer = new Utf8JsonWriter(output, new JsonWriterOptions { Indented = indent });
        var builder = new DemoSummaryBuilder();

        writer.WriteStartObject();
        writer.WriteNumber("schemaVersion", PlaybackDocument.SchemaVersion);

        WriteHeader(writer, reader.Header);

        if (includeCommands)
        {
            writer.WriteStartArray("commands");
        }

        var written = 0;
        var players = new PlayerDirectory();
        var events = new EventCollector();
        var npcGui = new NpcGuiReplay();
        events.SetRecorderName(reader.Header.ClientName);
        foreach (var command in reader.ReadCommands())
        {
            progress?.Invoke(reader.Position);
            builder.Add(command, reader.Position);

            switch (command)
            {
                case PacketCommand packet:
                    players.AddPacket(packet.Payload);
                    events.AddPacket(packet);
                    break;
                case ConsoleCommand console:
                    events.AddConsole(console);
                    break;
                case UserCommand input:
                    npcGui.Add(input);
                    break;
                case DataTablesCommand dataTables:
                    events.AddDataTables(dataTables);
                    break;
                case StringTablesCommand stringTables:
                    players.AddStringTablesDump(stringTables.Payload);
                    events.Precache.AddPayload(stringTables.Payload, stringTables.Tick);
                    break;
            }

            if (includeCommands && written < maxCommands)
            {
                WriteCommand(writer, command);
                written++;
            }
        }

        if (includeCommands)
        {
            writer.WriteEndArray();
        }

        var snapshot = players.Snapshot();
        snapshot = events.BindPlayers(snapshot);
        var tickRate = reader.Header.TickRate ?? 66.6666666667;

        IReadOnlyList<VoiceClip> clips = [];
        if (outputPath is { Length: > 0 })
        {
            clips = events.Voice.WriteSidecars(outputPath, snapshot, tickRate, events.PlayerAtSlot);
            var dir = Path.GetDirectoryName(outputPath);
            var stem = Path.GetFileNameWithoutExtension(outputPath);
            if (dir is { Length: > 0 } && stem is { Length: > 0 })
            {
                events.NetLog.WriteJsonl(Path.Combine(dir, stem + ".net.jsonl"));
                events.Gluapack.CollectFromDemo(events.Precache);
                if (events.Gluapack.Detected && !events.Gluapack.HasPack)
                {
                    var chosen = choosePackFile?.Invoke(events.Gluapack, Path.GetDirectoryName(reader.SourcePath));
                    if (chosen is { Length: > 0 })
                    {
                        if (!events.Gluapack.TryLoadFile(chosen))
                        {
                            events.Gluapack.MarkSkipped();
                        }
                        else
                        {
                            events.Gluapack.BindAfterLoad(events.Precache);
                        }
                    }
                    else
                    {
                        events.Gluapack.MarkSkipped();
                    }
                }

                LuaExporter.Write(Path.Combine(dir, stem + ".lua"), events.Precache, events.Gluapack);
            }
        }

        // The clothing wire format changed between server versions, independently of the
        // recording client's architecture. Resolve it before writing any clothing records.
        var pack = events.Gluapack;
        if (pack.Detected && !pack.HasPack
            && pack.SuggestPackPath(Path.GetDirectoryName(reader.SourcePath)) is { } localPack)
        {
            if (!pack.TryLoadFile(localPack))
            {
                Console.Error.WriteLine("CLOTHING_GLUAPACK_LOAD_FAILED: " + localPack);
            }
        }
        string? ClothingLua(string fileName)
        {
            var clothingPath = "gamemodes/darkrp/gamemode/modules/clothing/" + fileName;
            var text = pack.FileForPath(clothingPath);
            if (text is not null) return text;
            var record = events.Precache.LuaFileRecords.FirstOrDefault(r =>
                r.Path.Replace('\\', '/').EndsWith(clothingPath, StringComparison.OrdinalIgnoreCase));
            return record is null ? null : LuaExporter.FromUserData(record.UserData);
        }
        events.Perpheads.ResolveClothingLayout(ClothingLua("sh_clothes.lua"), ClothingLua("sh_clothing_ent.lua"));

        var playback = PlaybackDocument.Build(
            snapshot,
            events.Events,
            events.Precache,
            tickRate,
            clips);
        playback.Write(writer);
        WriteModels(writer, events.Precache);
        writer.WriteStartArray("lightStyles");
        foreach (var style in events.Precache.LightStyles)
        {
            writer.WriteStartObject();
            writer.WriteNumber("time", Math.Max(0, style.Tick) / tickRate);
            writer.WriteNumber("style", style.Style);
            writer.WriteString("pattern", style.Pattern);
            writer.WriteEndObject();
        }
        writer.WriteEndArray();
        events.TempEntities.Write(writer, events.Precache, tickRate, events.Perpheads.BloodImpacts,
            events.Perpheads.SurfaceImpacts);
        events.TempEntities.WritePlayerAnimations(writer, tickRate);
        events.Entities.Fires.Write(writer, events.Precache.NetworkName, tickRate);
        events.NetLog.WriteClothing(writer);
        events.Perpheads.WriteNwVars(writer, tickRate);
        events.Perpheads.WriteClothingData(writer, tickRate);
        events.Perpheads.WriteUiEvents(writer, tickRate);
        var npcLua = events.NetLog.Rows.Any(r => r.Name == "perpheads_npc_used")
            ? GmodDemo.Cli.PerpData.BuildTree(events, reader.SourcePath, null).Files
            : Array.Empty<(string Path, string Text)>();
        npcGui.Write(writer, npcLua, events.NetLog.Rows, tickRate, tick => {
            int entity=events.Entities.LocalPlayerEntity ?? events.Entities.RecorderEntity ?? -1;
            var team=events.Entities.TeamHistory.LastOrDefault(t=>t.Entity==entity && t.Tick<=tick);
            return new NpcGuiReplay.PlayerContext(team?.Team ?? 1,events.Perpheads.NpcPlayerStateAt(entity,tick));
        });
        writer.WriteStartArray("teams");
        foreach (var entry in events.Entities.TeamHistory)
        {
            writer.WriteStartObject();
            writer.WriteNumber("t", entry.Tick);
            writer.WriteNumber("time", tickRate > 0 ? entry.Tick / tickRate : 0);
            writer.WriteNumber("e", entry.Entity);
            writer.WriteNumber("team", entry.Team);
            writer.WriteEndObject();
        }

        writer.WriteEndArray();
        writer.WriteStartArray("weaponState");
        foreach (var entry in events.Entities.WeaponLog)
        {
            writer.WriteStartObject();
            writer.WriteNumber("t", entry.Tick);
            writer.WriteString("kind", entry.Kind);
            writer.WriteNumber("e", entry.Entity);
            writer.WriteNumber("serial", entry.Serial);
            // An active weapon is named by its exact handle (index and serial), never by
            // whatever entity later reused that index.
            var value = entry.Kind == "active"
                ? (events.Entities.HandleNames.TryGetValue((entry.Other, entry.Serial), out var named) ? named : null)
                : entry.Value;
            if (value is not null)
            {
                writer.WriteString("value", value);
            }

            if (entry.Kind == "active")
            {
                writer.WriteNumber("weapon", entry.Other);
            }
            else if (entry.Kind is "owner" or "gone"
                && events.Entities.HandleNames.TryGetValue((entry.Entity, entry.Serial), out var ownedClass))
            {
                writer.WriteString("cls", ownedClass);
            }

            writer.WriteEndObject();
        }

        writer.WriteEndArray();
        writer.WriteStartArray("ragdolls");
        foreach (var entry in events.Entities.RagdollLog)
        {
            writer.WriteStartObject();
            writer.WriteNumber("t", entry.Tick);
            writer.WriteNumber("e", entry.Entity);
            writer.WriteNumber("serial", entry.Serial);
            writer.WriteNumber("model", entry.Model);
            writer.WriteStartArray("pos");
            foreach (var p in entry.Pos)
            {
                writer.WriteNumberValue(p.X);
                writer.WriteNumberValue(p.Y);
                writer.WriteNumberValue(p.Z);
            }

            writer.WriteEndArray();
            writer.WriteStartArray("ang");
            foreach (var a in entry.Ang)
            {
                writer.WriteNumberValue(a.X);
                writer.WriteNumberValue(a.Y);
                writer.WriteNumberValue(a.Z);
            }

            writer.WriteEndArray();
            writer.WriteEndObject();
        }

        writer.WriteEndArray();
        writer.WriteStartArray("viewModels");
        foreach (var entry in events.Entities.ViewModelLog)
        {
            writer.WriteStartObject();
            writer.WriteNumber("t", entry.Tick);
            writer.WriteNumber("e", entry.Entity);
            writer.WriteNumber("vm", entry.VmIndex);
            writer.WriteNumber("owner", entry.Owner);
            writer.WriteNumber("model", entry.Model);
            writer.WriteNumber("seq", entry.Sequence);
            writer.WriteNumber("rate", entry.Rate);
            writer.WriteNumber("newSeq", entry.NewSequenceParity);
            writer.WriteNumber("animParity", entry.AnimationParity);
            writer.WriteNumber("weapon", entry.Weapon);
            writer.WriteNumber("effects", entry.Effects);
            writer.WriteEndObject();
        }

        writer.WriteEndArray();
        writer.WriteStartArray("poses");
        foreach (var entry in events.Entities.PoseLog)
        {
            writer.WriteStartObject();
            writer.WriteNumber("t", entry.Tick);
            writer.WriteNumber("e", entry.Entity);
            writer.WriteStartArray("i");
            foreach (var index in entry.Index)
            {
                writer.WriteNumberValue(index);
            }

            writer.WriteEndArray();
            writer.WriteStartArray("v");
            foreach (var value in entry.Value)
            {
                writer.WriteNumberValue(Math.Round(value, 4));
            }

            writer.WriteEndArray();
            writer.WriteEndObject();
        }

        writer.WriteEndArray();
        writer.WriteStartArray("sounds");
        var sounds = events.Sounds
            .Select(sound => (sound.Tick, Source: (SoundRow?)sound, Lua: (PerpheadsSoundPlay?)null))
            .Concat(events.Perpheads.SoundPlays.Select(sound =>
                (sound.Tick, Source: (SoundRow?)null, Lua: (PerpheadsSoundPlay?)sound)))
            .OrderBy(sound => sound.Tick);
        foreach (var soundEvent in sounds)
        {
            if (soundEvent.Source is { } sound)
            {
                writer.WriteStartObject();
                writer.WriteNumber("t", sound.Tick);
                writer.WriteNumber("e", sound.Entity);
                writer.WriteNumber("n", sound.SoundNum);
                if (events.Precache.Sounds.TryGetValue(sound.SoundNum, out var soundName))
                {
                    writer.WriteString("name", soundName);
                }

                writer.WriteNumber("f", sound.Flags);
                writer.WriteNumber("c", sound.Channel);
                if (!sound.IsStop)
                {
                    writer.WriteNumber("v", Math.Round(sound.Volume, 3));
                    writer.WriteNumber("lvl", sound.Level);
                    writer.WriteNumber("p", sound.Pitch);
                    writer.WriteStartArray("pos");
                    writer.WriteNumberValue(sound.X);
                    writer.WriteNumberValue(sound.Y);
                    writer.WriteNumberValue(sound.Z);
                    writer.WriteEndArray();
                    if (Math.Abs(sound.Delay) > 0.001f)
                    {
                        writer.WriteNumber("delay", Math.Round(sound.Delay, 3));
                    }
                }

                writer.WriteEndObject();
            }
            else if (soundEvent.Lua is { } luaSound)
            {
                writer.WriteStartObject();
                writer.WriteNumber("t", luaSound.Tick);
                writer.WriteString("name", luaSound.Name);
                writer.WriteNumber("v", Math.Round(luaSound.Volume, 3));
                writer.WriteNumber("lvl", luaSound.Level);
                writer.WriteNumber("p", luaSound.Pitch);
                writer.WriteStartArray("pos");
                writer.WriteNumberValue(luaSound.X);
                writer.WriteNumberValue(luaSound.Y);
                writer.WriteNumberValue(luaSound.Z);
                writer.WriteEndArray();
                writer.WriteEndObject();
            }

        }

        writer.WriteEndArray();
        WriteAudio(writer, clips, events.Voice.ResolvedInit);

        if (fileLength is { } knownLength)
        {
            progress?.Invoke(knownLength);
        }

        var summary = builder.Build(reader, fileLength);
        WriteSummary(writer, summary, events.EntityStats);

        writer.WriteEndObject();
        writer.Flush();
        afterWalk?.Invoke(events);

        return summary;
    }

    private static void WriteHeader(Utf8JsonWriter writer, DemoHeader header)
    {
        writer.WriteStartObject("header");
        writer.WriteString("magic", header.Magic);
        writer.WriteString("variant", header.Variant.ToString());
        writer.WriteNumber("demoProtocol", header.DemoProtocol);
        writer.WriteNumber("networkProtocol", header.NetworkProtocol);
        writer.WriteString("serverName", header.ServerName);
        writer.WriteString("clientName", header.ClientName);
        writer.WriteString("mapName", header.MapName);
        writer.WriteString("gameDirectory", header.GameDirectory);
        writer.WriteNumber("playbackTime", header.PlaybackTime);
        writer.WriteNumber("playbackTicks", header.PlaybackTicks);
        writer.WriteNumber("playbackFrames", header.PlaybackFrames);
        writer.WriteNumber("signonLength", header.SignonLength);

        if (header.TickRate is { } rate)
        {
            writer.WriteNumber("tickRate", rate);
        }
        else
        {
            writer.WriteNull("tickRate");
        }

        writer.WriteEndObject();
    }

    private static void WriteModels(Utf8JsonWriter writer, PrecacheTables precache)
    {
        if (precache.ModelUpdateFailures > 0)
        {
            Console.Error.WriteLine($"MODEL_PRECACHE_UPDATE_FAILED: {precache.ModelUpdateFailures} model-table updates could not be decoded; some late props may be unavailable.");
        }

        writer.WriteStartObject("models");
        foreach (var (index, path) in precache.Models.OrderBy(kv => kv.Key))
        {
            writer.WriteString(index.ToString(), path);
        }

        writer.WriteEndObject();
    }

    private void WriteCommand(Utf8JsonWriter writer, DemoCommand command)
    {
        writer.WriteStartObject();
        writer.WriteString("type", command.Type.ToString());
        writer.WriteNumber("tick", command.Tick);
        writer.WriteNumber("offset", command.Offset);

        switch (command)
        {
            case PacketCommand packet:
                writer.WriteNumber("sequenceIn", packet.SequenceIn);
                writer.WriteNumber("sequenceOut", packet.SequenceOut);
                WriteCommandInfo(writer, packet.Info);
                WritePayload(writer, packet.Payload);
                break;

            case ConsoleCommand console:
                writer.WriteString("command", console.Command);
                break;

            case UserCommand user:
                writer.WriteNumber("commandNumber", user.CommandNumber);
                WritePayload(writer, user.Payload);
                break;

            case DataTablesCommand dataTables:
                WritePayload(writer, dataTables.Payload);
                break;

            case StringTablesCommand stringTables:
                WritePayload(writer, stringTables.Payload);
                if (stringTables.Payload.Length == DemoSummary.StringTablesTruncationSize)
                {
                    writer.WriteBoolean("likelyTruncated", true);
                }

                break;
        }

        writer.WriteEndObject();
    }

    private static void WriteCommandInfo(Utf8JsonWriter writer, DemoCommandInfo info)
    {
        writer.WriteStartObject("cameraInfo");
        writer.WriteNumber("flags", (int)info.Flags);
        writer.WriteString("flagNames", info.Flags.ToString());
        WriteVec3(writer, "viewOrigin", info.EffectiveViewOrigin);
        WriteVec3(writer, "viewAngles", info.EffectiveViewAngles);
        WriteVec3(writer, "localViewAngles", info.EffectiveLocalViewAngles);
        writer.WriteEndObject();
    }

    private static void WriteVec3(Utf8JsonWriter writer, string name, Vec3 value)
    {
        writer.WriteStartObject(name);
        writer.WriteNumber("x", value.X);
        writer.WriteNumber("y", value.Y);
        writer.WriteNumber("z", value.Z);
        writer.WriteEndObject();
    }

    /// <summary>
    /// Undecoded payloads are reported by length always, and byte-for-byte only on request.
    /// Emitting the length unconditionally keeps the output honest about what has not been
    /// decoded yet without inflating every export.
    /// </summary>
    private void WritePayload(Utf8JsonWriter writer, byte[] payload)
    {
        writer.WriteNumber("payloadBytes", payload.Length);
        writer.WriteBoolean("decoded", false);

        if (includeRawPayloads)
        {
            writer.WriteBase64String("payloadBase64", payload);
        }
    }

    private static void WriteAudio(Utf8JsonWriter writer, IReadOnlyList<VoiceClip> clips, VoiceInitInfo? init)
    {
        writer.WriteStartObject("audio");
        var wav = clips.Count > 0 && clips.All(c => c.File.EndsWith(".wav", StringComparison.OrdinalIgnoreCase));
        if (init is { } voiceInit)
        {
            writer.WriteString("codec", voiceInit.Codec);
            writer.WriteNumber("quality", voiceInit.Quality);
        }

        writer.WriteNumber("sampleRate", wav ? VoiceDecoder.OutputSampleRate : (init is { SampleRate: > 0 } sr ? sr.SampleRate : VoiceDecoder.OutputSampleRate));

        writer.WriteBoolean("pcm", wav);
        writer.WriteString("format", wav ? "wav" : VoiceTrack.FileMagic.ToLowerInvariant());
        if (init is { Codec: "steam" } || wav)
        {
            writer.WriteString("payload", "steam-opus");
        }

        writer.WriteStartArray("clips");
        foreach (var clip in clips)
        {
            writer.WriteStartObject();
            writer.WriteNumber("t", clip.StartTick);
            writer.WriteNumber("tEnd", clip.EndTick);
            writer.WriteNumber("time", clip.Time);
            writer.WriteNumber("duration", clip.Duration);
            writer.WriteBoolean("proximity", clip.Proximity);
            WriteOptionalInt(writer, "p", clip.P);
            if (clip.Name is { Length: > 0 })
            {
                writer.WriteString("playerName", clip.Name);
            }

            if (clip.SteamId is { Length: > 0 })
            {
                writer.WriteString("steamId", clip.SteamId);
            }

            if (clip.SteamId64 is { Length: > 0 })
            {
                writer.WriteString("steamId64", clip.SteamId64);
            }

            writer.WriteNumber("slot", clip.Slot);
            writer.WriteString("file", clip.File);
            writer.WriteNumber("packets", clip.Packets);
            writer.WriteNumber("bytes", clip.Bytes);
            writer.WriteEndObject();
        }

        writer.WriteEndArray();
        writer.WriteEndObject();
    }

    internal static void WriteGmodString(Utf8JsonWriter writer, string name, string? value)
    {
        if (value is { Length: > 0 })
        {
            writer.WriteString(name, value);
        }
    }

    private static void WriteOptionalInt(Utf8JsonWriter writer, string name, int? value)
    {
        if (value is { } v)
        {
            writer.WriteNumber(name, v);
        }
    }

    private static void WriteSummary(Utf8JsonWriter writer, DemoSummary summary, EntityDecodeStats? entities = null)
    {
        writer.WriteStartObject("summary");
        writer.WriteNumber("totalCommands", summary.TotalCommands);

        if (summary.FirstTick is { } first)
        {
            writer.WriteNumber("firstTick", first);
        }

        if (summary.LastTick is { } last)
        {
            writer.WriteNumber("lastTick", last);
        }

        if (summary.LastContentTick is { } lastContent)
        {
            writer.WriteNumber("lastContentTick", lastContent);
        }

        if (summary.ContentTickCoverage is { } coverage)
        {
            writer.WriteNumber("contentTickCoverage", coverage);
        }

        writer.WriteBoolean("sawStopCommand", summary.SawStopCommand);
        writer.WriteNumber("endOffset", summary.EndOffset);

        if (summary.FileLength is { } fileLength)
        {
            writer.WriteNumber("fileLength", fileLength);
        }

        if (summary.TrailingBytes is { } trailing)
        {
            writer.WriteNumber("trailingBytes", trailing);
        }

        writer.WriteNumber("maxPacketPayloadBytes", summary.MaxPacketPayloadBytes);
        writer.WriteNumber("signonRegionLength", summary.SignonRegionLength);
        writer.WriteBoolean("parsedCleanly", summary.ParsedCleanly);
        writer.WriteBoolean("stringTablesLikelyTruncated", summary.StringTablesLikelyTruncated);
        if (summary.TailRecovery is { } recovery)
        {
            writer.WriteStartObject("tailRecovery");
            writer.WriteString("kind", recovery.Kind.ToString());
            writer.WriteNumber("offset", recovery.Offset);
            writer.WriteString("message", recovery.Message);
            writer.WriteEndObject();
        }

        writer.WriteStartObject("consistency");
        writer.WriteBoolean("playbackFramesMatchPacketCount", summary.PlaybackFramesMatchPacketCount);
        writer.WriteBoolean("playbackTicksMatchLastTick", summary.PlaybackTicksMatchLastTick);
        writer.WriteBoolean("signonLengthMatches", summary.SignonLengthMatches);
        writer.WriteEndObject();

        writer.WriteStartObject("commandCounts");
        foreach (var type in summary.CommandCounts.Keys.OrderBy(t => (byte)t))
        {
            writer.WriteNumber(type.ToString(), summary.CommandCounts[type]);
        }

        writer.WriteEndObject();

        writer.WriteStartObject("payloadBytes");
        foreach (var type in summary.PayloadBytes.Keys.OrderBy(t => (byte)t))
        {
            writer.WriteNumber(type.ToString(), summary.PayloadBytes[type]);
        }

        writer.WriteEndObject();

        writer.WriteStartArray("errors");
        foreach (var error in summary.Errors)
        {
            writer.WriteStartObject();
            writer.WriteString("kind", error.Kind.ToString());
            writer.WriteNumber("offset", error.Offset);
            writer.WriteString("message", error.Message);
            writer.WriteEndObject();
        }

        writer.WriteEndArray();

        if (entities is not null)
        {
            writer.WriteStartObject("entities");
            writer.WriteString("propIndexMode", entities.Mode.ToString());
            writer.WriteNumber("packets", entities.Packets);
            writer.WriteNumber("packetsClean", entities.PacketsClean);
            writer.WriteNumber("packetsFailed", entities.PacketsFailed);
            writer.WriteNumber("entered", entities.EntitiesEntered);
            writer.WriteNumber("left", entities.EntitiesLeft);
            writer.WriteNumber("originUpdates", entities.OriginUpdates);
            writer.WriteNumber("cameraMatches", entities.CameraMatches);
            writer.WriteBoolean("originsTrusted", entities.OriginsTrusted);
            writer.WriteEndObject();
        }

        writer.WriteEndObject();
    }
}
