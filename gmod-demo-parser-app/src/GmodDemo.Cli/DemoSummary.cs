namespace GmodDemo.Parser;

/// <summary>
/// Aggregate view of a demo's container structure, plus consistency checks between the header's
/// self-reported totals and what the command stream actually contains.
/// </summary>
public sealed record DemoSummary
{
    /// <summary>
    /// The engine writes the string table block into a fixed buffer and truncates on overflow,
    /// so a payload of exactly this size means the data is almost certainly incomplete.
    /// </summary>
    public const int StringTablesTruncationSize = 512 * 1024;

    public required DemoHeader Header { get; init; }
    public required IReadOnlyDictionary<DemoCommandType, int> CommandCounts { get; init; }
    public required IReadOnlyDictionary<DemoCommandType, long> PayloadBytes { get; init; }
    public required int TotalCommands { get; init; }
    public required int? FirstTick { get; init; }
    public required int? LastTick { get; init; }

    /// <summary>
    /// The highest tick carrying actual content, ignoring the terminating dem_stop record.
    /// A recording that lost its connection, or was left open after disconnect, can carry a
    /// dem_stop tick far beyond the last real data, making the header's playback totals a poor
    /// guide to how much of the timeline is populated.
    /// </summary>
    public required int? LastContentTick { get; init; }
    public required bool SawStopCommand { get; init; }
    public required long EndOffset { get; init; }
    public required long? FileLength { get; init; }
    public required int MaxPacketPayloadBytes { get; init; }
    public required int LargestStringTablesPayload { get; init; }

    /// <summary>
    /// Bytes from the end of the file header to the end of the last dem_signon record. This is
    /// what the header's SignonLength field measures, and it includes the intervening command
    /// headers and command info blocks rather than just summed payloads.
    /// </summary>
    public required long SignonRegionLength { get; init; }

    public required IReadOnlyList<DemoParseError> Errors { get; init; }
    public required DemoParseError? TailRecovery { get; init; }

    public long? TrailingBytes => FileLength - EndOffset;

    /// <summary>True when the stream terminated cleanly and consumed the file exactly.</summary>
    public bool ParsedCleanly => SawStopCommand && Errors.Count == 0 && TrailingBytes is null or 0;

    public bool StringTablesLikelyTruncated => LargestStringTablesPayload == StringTablesTruncationSize;

    /// <summary>The header's frame count should equal the number of dem_packet records.</summary>
    public bool PlaybackFramesMatchPacketCount =>
        Header.PlaybackFrames == CommandCounts.GetValueOrDefault(DemoCommandType.Packet);

    /// <summary>The header's tick count should equal the final record's tick.</summary>
    public bool PlaybackTicksMatchLastTick => Header.PlaybackTicks == LastTick;

    public bool SignonLengthMatches => Header.SignonLength == SignonRegionLength;

    /// <summary>
    /// Fraction of the header's declared tick range that actually carries content. Well below 1.0
    /// means the tail of the demo is empty and no data can be extracted from it.
    /// </summary>
    public double? ContentTickCoverage =>
        Header.PlaybackTicks > 0 && LastContentTick is { } last ? (double)last / Header.PlaybackTicks : null;

    public static DemoSummary Build(DemoReader reader)
    {
        ArgumentNullException.ThrowIfNull(reader);

        var builder = new DemoSummaryBuilder();
        foreach (var command in reader.ReadCommands())
        {
            builder.Add(command, reader.Position);
        }

        return builder.Build(reader, fileLength: null);
    }

    public static DemoSummary FromFile(string path, Action<long>? progress = null)
    {
        var fileLength = new FileInfo(path).Length;
        using var reader = DemoReader.Open(path);
        var builder = new DemoSummaryBuilder();
        foreach (var command in reader.ReadCommands())
        {
            builder.Add(command, reader.Position);
            progress?.Invoke(reader.Position);
        }

        progress?.Invoke(fileLength);
        return builder.Build(reader, fileLength);
    }
}

/// <summary>
/// Accumulates summary statistics one command at a time, so a caller already streaming the
/// command list (to write JSON, for example) can produce a summary without a second pass.
/// </summary>
public sealed class DemoSummaryBuilder
{
    private readonly Dictionary<DemoCommandType, int> _counts = [];
    private readonly Dictionary<DemoCommandType, long> _payloadBytes = [];
    private int? _firstTick;
    private int? _lastTick;
    private int? _lastContentTick;
    private int _total;
    private int _maxPacketPayload;
    private int _largestStringTables;
    private long _signonRegionEnd;

    /// <param name="positionAfter">
    /// The reader's byte offset immediately after this command, used to locate the end of the
    /// signon region.
    /// </param>
    public void Add(DemoCommand command, long positionAfter)
    {
        ArgumentNullException.ThrowIfNull(command);

        _total++;
        _firstTick ??= command.Tick;
        _lastTick = command.Tick;
        _counts[command.Type] = _counts.GetValueOrDefault(command.Type) + 1;

        if (command.Type != DemoCommandType.Stop)
        {
            _lastContentTick = _lastContentTick is { } previous
                ? Math.Max(previous, command.Tick)
                : command.Tick;
        }

        switch (command)
        {
            case PacketCommand packet:
                Accumulate(command.Type, packet.Payload.Length);
                if (packet.Payload.Length > _maxPacketPayload)
                {
                    _maxPacketPayload = packet.Payload.Length;
                }

                if (packet.Type == DemoCommandType.Signon)
                {
                    _signonRegionEnd = positionAfter;
                }

                break;

            case ConsoleCommand console:
                Accumulate(command.Type, console.PayloadBytes);
                break;

            case UserCommand user:
                Accumulate(command.Type, user.Payload.Length);
                break;

            case DataTablesCommand dataTables:
                Accumulate(command.Type, dataTables.Payload.Length);
                break;

            case StringTablesCommand stringTables:
                Accumulate(command.Type, stringTables.Payload.Length);
                if (stringTables.Payload.Length > _largestStringTables)
                {
                    _largestStringTables = stringTables.Payload.Length;
                }

                break;
        }
    }

    public DemoSummary Build(DemoReader reader, long? fileLength)
    {
        ArgumentNullException.ThrowIfNull(reader);

        return new DemoSummary
        {
            Header = reader.Header,
            CommandCounts = _counts,
            PayloadBytes = _payloadBytes,
            TotalCommands = _total,
            FirstTick = _firstTick,
            LastTick = _lastTick,
            LastContentTick = _lastContentTick,
            SawStopCommand = reader.SawStopCommand,
            EndOffset = reader.Position,
            FileLength = fileLength,
            MaxPacketPayloadBytes = _maxPacketPayload,
            LargestStringTablesPayload = _largestStringTables,
            SignonRegionLength = _signonRegionEnd > 0 ? _signonRegionEnd - DemoFormat.HeaderSize : 0,
            Errors = reader.Errors.ToArray(),
            TailRecovery = reader.TailRecovery,
        };
    }

    private void Accumulate(DemoCommandType type, int bytes) =>
        _payloadBytes[type] = _payloadBytes.GetValueOrDefault(type) + bytes;
}
