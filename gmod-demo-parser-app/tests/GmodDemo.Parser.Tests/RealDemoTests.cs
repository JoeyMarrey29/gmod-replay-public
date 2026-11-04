using GmodDemo.Parser;

namespace GmodDemo.Parser.Tests;

/// <summary>
/// Walks the captured demo once and records observations, so the assertions below can each
/// interrogate a different property of the format without re-reading the file.
/// </summary>
public sealed class RealDemoWalk
{
    public RealDemoWalk()
    {
        if (!RealDemo.Available)
        {
            return;
        }

        using var reader = DemoReader.Open(RealDemo.Path);
        var builder = new DemoSummaryBuilder();

        var ordinal = 0;
        var previousTick = int.MinValue;
        var seenSyncTick = false;
        TicksNonDecreasingAfterSyncTick = true;

        foreach (var command in reader.ReadCommands())
        {
            builder.Add(command, reader.Position);

            if (command.Tick < previousTick)
            {
                TickDecreaseCount++;
                TickDecreasesAtSyncTick += command.Type == DemoCommandType.SyncTick ? 1 : 0;

                if (seenSyncTick)
                {
                    TicksNonDecreasingAfterSyncTick = false;
                }
            }

            previousTick = command.Tick;

            switch (command)
            {
                case SyncTickCommand syncTick:
                    seenSyncTick = true;
                    SyncTickTicks.Add(syncTick.Tick);
                    break;

                case PacketCommand { Type: DemoCommandType.Signon }:
                    LastSignonOrdinal = ordinal;
                    break;

                case PacketCommand { Type: DemoCommandType.Packet } packet:
                    FirstPacketOrdinal ??= ordinal;
                    ObservedCameraFlags |= (int)packet.Info.Flags;
                    break;

                case DataTablesCommand:
                    DataTablesOrdinal = ordinal;
                    DataTablesOffset = command.Offset;
                    break;

                case StringTablesCommand:
                    StringTablesOffset = command.Offset;
                    break;
            }

            ordinal++;
        }

        Summary = builder.Build(reader, new FileInfo(RealDemo.Path).Length);
    }

    public DemoSummary? Summary { get; }

    /// <summary>Number of places where a command's tick was lower than its predecessor's.</summary>
    public int TickDecreaseCount { get; private set; }

    /// <summary>How many of those decreases landed on a dem_synctick record.</summary>
    public int TickDecreasesAtSyncTick { get; private set; }

    public bool TicksNonDecreasingAfterSyncTick { get; private set; }

    public List<int> SyncTickTicks { get; } = [];

    public int? LastSignonOrdinal { get; }

    public int? FirstPacketOrdinal { get; }

    public int? DataTablesOrdinal { get; }

    public long? DataTablesOffset { get; }

    public long? StringTablesOffset { get; }

    /// <summary>Bitwise union of every camera flags value seen, used to detect undocumented bits.</summary>
    public int ObservedCameraFlags { get; }
}

/// <summary>
/// Assertions about what a real Garry's Mod demo actually contains. These are statements about
/// the format, verified empirically, not descriptions of how the parser happens to be written.
/// </summary>
public sealed class RealDemoTests(RealDemoWalk walk) : IClassFixture<RealDemoWalk>
{
    private DemoSummary Summary => walk.Summary ?? throw new InvalidOperationException("Demo unavailable.");

    [RealDemoFact]
    public void GarrysMod_stamps_its_own_magic_not_HL2DEMO()
    {
        // GMod renames the 8-byte magic, which is why stock Source parsers reject its demos
        // outright. Everything after these 8 bytes follows the standard protocol 3 layout.
        Assert.Equal(DemoVariant.GarrysMod, Summary.Header.Variant);
        Assert.Equal("GMODEMO", Summary.Header.Magic);
    }

    [RealDemoFact]
    public void Uses_demo_protocol_3_and_network_protocol_24()
    {
        Assert.Equal(3, Summary.Header.DemoProtocol);
        Assert.Equal(24, Summary.Header.NetworkProtocol);
        Assert.Equal("garrysmod", Summary.Header.GameDirectory);
    }

    [RealDemoFact]
    public void Command_stream_consumes_the_file_exactly_and_ends_with_stop()
    {
        // The strongest available check on the container layout. If the per-command framing were
        // wrong by even one byte (for instance if protocol 3 carried protocol 4's player-slot
        // byte) the walk would desync and hit an invalid command byte long before the end.
        Assert.True(Summary.SawStopCommand);
        Assert.Empty(Summary.Errors);
        Assert.Equal(Summary.FileLength, Summary.EndOffset);
        Assert.Equal(0, Summary.TrailingBytes);
    }

    [RealDemoFact]
    public void Header_playback_frames_equals_the_dem_packet_count()
    {
        Assert.Equal(
            Summary.Header.PlaybackFrames,
            Summary.CommandCounts[DemoCommandType.Packet]);
    }

    [RealDemoFact]
    public void Header_playback_ticks_equals_the_final_command_tick()
    {
        Assert.Equal(Summary.Header.PlaybackTicks, Summary.LastTick);
    }

    [RealDemoFact]
    public void Signon_length_measures_bytes_to_the_end_of_the_last_signon_record()
    {
        // Note this is not the sum of signon payloads: it spans the whole region including the
        // intervening command headers and camera blocks.
        Assert.Equal(Summary.Header.SignonLength, Summary.SignonRegionLength);
        Assert.NotEqual(
            Summary.Header.SignonLength,
            Summary.PayloadBytes[DemoCommandType.Signon]);
    }

    [RealDemoFact]
    public void All_signon_records_precede_the_first_packet()
    {
        Assert.NotNull(walk.LastSignonOrdinal);
        Assert.NotNull(walk.FirstPacketOrdinal);
        Assert.True(walk.LastSignonOrdinal < walk.FirstPacketOrdinal);
    }

    [RealDemoFact]
    public void Data_tables_appear_once_and_inside_the_signon_region()
    {
        Assert.Equal(1, Summary.CommandCounts[DemoCommandType.DataTables]);
        Assert.NotNull(walk.DataTablesOffset);

        var signonEnd = DemoFormat.HeaderSize + Summary.Header.SignonLength;
        Assert.True(walk.DataTablesOffset < signonEnd);
    }

    [RealDemoFact]
    public void String_tables_appear_once_and_after_the_signon_region()
    {
        Assert.Equal(1, Summary.CommandCounts[DemoCommandType.StringTables]);
        Assert.NotNull(walk.StringTablesOffset);

        var signonEnd = DemoFormat.HeaderSize + Summary.Header.SignonLength;
        Assert.True(walk.StringTablesOffset > signonEnd);
    }

    [RealDemoFact]
    public void String_table_block_hits_the_engine_buffer_ceiling()
    {
        // A payload landing on exactly 512 KiB indicates the engine truncated the block rather
        // than that the data happened to be this size. Consumers must not assume completeness.
        Assert.Equal(DemoSummary.StringTablesTruncationSize, Summary.LargestStringTablesPayload);
        Assert.True(Summary.StringTablesLikelyTruncated);
    }

    [RealDemoFact]
    public void Sync_tick_is_always_tick_zero()
    {
        // The engine writes dem_synctick with a hardcoded tick of 0 to mark the boundary between
        // the signon burst and live playback, where the client resets its demo clock.
        Assert.NotEmpty(walk.SyncTickTicks);
        Assert.All(walk.SyncTickTicks, tick => Assert.Equal(0, tick));
    }

    [RealDemoFact]
    public void Sync_tick_is_the_only_place_ticks_move_backwards()
    {
        // Signon records carry real recording ticks, so the tick-0 synctick that follows them is
        // a genuine backwards step. It is the only one: a consumer treating ticks as monotonic
        // must special-case this single record rather than assume the stream is sorted.
        Assert.Equal(1, walk.TickDecreaseCount);
        Assert.Equal(walk.TickDecreaseCount, walk.TickDecreasesAtSyncTick);
        Assert.True(walk.TicksNonDecreasingAfterSyncTick);
    }

    [RealDemoFact]
    public void Camera_info_uses_only_the_three_documented_flag_bits()
    {
        // An undocumented bit here would mean the camera block carries state we are not modelling.
        const int documented = (int)(DemoCommandInfoFlags.UseOrigin2
            | DemoCommandInfoFlags.UseAngles2
            | DemoCommandInfoFlags.NoInterp);

        Assert.Equal(0, walk.ObservedCameraFlags & ~documented);
    }

    [RealDemoFact]
    public void Stop_record_can_sit_far_beyond_the_last_real_content()
    {
        // The header's playback totals describe the recording session, not the data. Here the
        // stop record lands at a tick roughly four times the last tick carrying content, so any
        // consumer sizing a timeline from PlaybackTicks would produce mostly empty output.
        Assert.NotNull(Summary.LastContentTick);
        Assert.True(Summary.LastContentTick < Summary.LastTick);
        Assert.Equal(DemoCommandType.Stop, Summary.CommandCounts.Keys.Single(t => t == DemoCommandType.Stop));
        Assert.True(Summary.ContentTickCoverage < 0.5);
    }

    [RealDemoFact]
    public void Packet_payloads_exceed_the_stock_Source_maximum()
    {
        // GMod raises NET_MAX_PAYLOAD well above stock Source, so any validation threshold
        // borrowed from the Valve SDK would reject legitimate packets.
        Assert.True(Summary.MaxPacketPayloadBytes > 100_000);
    }
}
