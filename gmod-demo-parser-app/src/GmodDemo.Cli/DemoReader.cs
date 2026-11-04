using System.Buffers.Binary;
using System.Text;

namespace GmodDemo.Parser;

/// <summary>
/// Forward-only reader over the demo container. Yields one <see cref="DemoCommand"/> per record
/// and never buffers the whole file, so memory stays bounded by the largest single payload.
/// </summary>
/// <remarks>
/// This is the container layer only. Packet payloads are handed back as raw bytes; decoding the
/// net-message bitstream inside them is a separate concern.
/// </remarks>
public sealed class DemoReader : IDisposable
{
    private readonly Stream _stream;
    private readonly bool _leaveOpen;
    private readonly long _knownLength;
    private readonly List<DemoParseError> _errors = [];
    private long _position;
    private bool _enumerated;

    public DemoReader(Stream stream, bool leaveOpen = false, string? sourcePath = null)
    {
        ArgumentNullException.ThrowIfNull(stream);
        if (!stream.CanRead)
        {
            throw new ArgumentException("Stream must be readable.", nameof(stream));
        }

        _stream = stream;
        _leaveOpen = leaveOpen;
        _knownLength = stream.CanSeek ? stream.Length : -1;
        SourcePath = sourcePath;

        var headerBytes = new byte[DemoFormat.HeaderSize];
        if (!TryReadExact(headerBytes))
        {
            throw new DemoFormatException(
                _position,
                $"File ended while reading the demo header; only {_position} of {DemoFormat.HeaderSize} bytes available.");
        }

        Header = DemoHeader.Parse(headerBytes);
    }

    public static DemoReader Open(string path)
    {
        var stream = new FileStream(
            path,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            bufferSize: 1 << 16,
            FileOptions.SequentialScan);

        try
        {
            return new DemoReader(stream, sourcePath: path);
        }
        catch
        {
            stream.Dispose();
            throw;
        }
    }

    public DemoHeader Header { get; }

    public string? SourcePath { get; }

    /// <summary>Recoverable problems found so far. Populated as enumeration proceeds.</summary>
    public IReadOnlyList<DemoParseError> Errors => _errors;

    /// <summary>Incomplete end-of-file record discarded so preceding commands can still be used.</summary>
    public DemoParseError? TailRecovery { get; private set; }

    /// <summary>True once a dem_stop record has been read, indicating a cleanly terminated stream.</summary>
    public bool SawStopCommand { get; private set; }

    /// <summary>Byte offset reached. After a clean read this equals the file length.</summary>
    public long Position => _position;

    /// <summary>
    /// Walks the command stream. Enumeration halts at dem_stop, at end of input, or at the first
    /// unrecoverable desync; in the latter two cases an entry is appended to <see cref="Errors"/>.
    /// </summary>
    public IEnumerable<DemoCommand> ReadCommands()
    {
        if (_enumerated)
        {
            throw new InvalidOperationException("The command stream has already been enumerated.");
        }

        _enumerated = true;

        var commandInfoBuffer = new byte[DemoFormat.CommandInfoSize];

        while (true)
        {
            var commandOffset = _position;

            var commandByte = ReadByteOrEof();
            if (commandByte < 0)
            {
                RecoverTail(commandOffset, "Stream ended without a dem_stop record; complete commands before EOF were retained.");
                yield break;
            }

            if (commandByte is < 1 or > 8)
            {
                AddError(
                    DemoErrorKind.InvalidCommandByte,
                    commandOffset,
                    $"Command byte {commandByte} is outside the valid range 1-8; the stream is out of sync.");
                yield break;
            }

            if (!TryReadInt32(out var tick))
            {
                RecoverTail(_position, "Stream ended while reading the final command tick; the incomplete command was discarded.");
                yield break;
            }

            var type = (DemoCommandType)commandByte;

            switch (type)
            {
                case DemoCommandType.Signon:
                case DemoCommandType.Packet:
                {
                    if (!TryReadExact(commandInfoBuffer))
                    {
                        RecoverTail(_position, "Stream ended inside the final command info block; the incomplete command was discarded.");
                        yield break;
                    }

                    var info = DemoCommandInfo.Parse(commandInfoBuffer);

                    if (!TryReadInt32(out var sequenceIn) || !TryReadInt32(out var sequenceOut))
                    {
                        RecoverTail(_position, "Stream ended reading the final packet sequence numbers; the incomplete command was discarded.");
                        yield break;
                    }

                    if (!TryReadLengthPrefixedPayload(out var payload, out var payloadError))
                    {
                        RecordReadFailure(payloadError!);
                        yield break;
                    }

                    yield return new PacketCommand(type, tick, commandOffset, info, sequenceIn, sequenceOut, payload!);
                    break;
                }

                case DemoCommandType.SyncTick:
                    yield return new SyncTickCommand(tick, commandOffset);
                    break;

                case DemoCommandType.ConsoleCmd:
                {
                    if (!TryReadLengthPrefixedPayload(out var payload, out var payloadError))
                    {
                        RecordReadFailure(payloadError!);
                        yield break;
                    }

                    yield return new ConsoleCommand(
                        tick,
                        commandOffset,
                        DecodeConsoleCommand(payload!),
                        payload!.Length);
                    break;
                }

                case DemoCommandType.UserCmd:
                {
                    if (!TryReadInt32(out var commandNumber))
                    {
                        RecoverTail(_position, "Stream ended reading the final usercmd number; the incomplete command was discarded.");
                        yield break;
                    }

                    if (!TryReadLengthPrefixedPayload(out var payload, out var payloadError))
                    {
                        RecordReadFailure(payloadError!);
                        yield break;
                    }

                    yield return new UserCommand(tick, commandOffset, commandNumber, payload!);
                    break;
                }

                case DemoCommandType.DataTables:
                {
                    if (!TryReadLengthPrefixedPayload(out var payload, out var payloadError))
                    {
                        RecordReadFailure(payloadError!);
                        yield break;
                    }

                    yield return new DataTablesCommand(tick, commandOffset, payload!);
                    break;
                }

                case DemoCommandType.StringTables:
                {
                    if (!TryReadLengthPrefixedPayload(out var payload, out var payloadError))
                    {
                        RecordReadFailure(payloadError!);
                        yield break;
                    }

                    yield return new StringTablesCommand(tick, commandOffset, payload!);
                    break;
                }

                case DemoCommandType.Stop:
                    SawStopCommand = true;
                    yield return new StopCommand(tick, commandOffset);
                    yield break;
            }
        }
    }

    /// <summary>
    /// Console command payloads are NUL-terminated and the length includes the terminator.
    /// Encoding is unspecified by the format; player-authored text can be non-ASCII, so decode
    /// permissively rather than rejecting it.
    /// </summary>
    private static string DecodeConsoleCommand(byte[] payload)
    {
        var end = Array.IndexOf(payload, (byte)0);
        var length = end >= 0 ? end : payload.Length;
        return Encoding.UTF8.GetString(payload, 0, length);
    }

    private bool TryReadLengthPrefixedPayload(out byte[]? payload, out DemoParseError? error)
    {
        payload = null;
        error = null;

        var lengthOffset = _position;
        if (!TryReadInt32(out var length))
        {
            error = new DemoParseError(DemoErrorKind.TruncatedInput, lengthOffset, "Stream ended reading a payload length.");
            return false;
        }

        if (length < 0)
        {
            error = new DemoParseError(DemoErrorKind.InvalidLength, lengthOffset, $"Negative payload length {length}.");
            return false;
        }

        if (length > DemoFormat.MaxPayloadBytes)
        {
            error = new DemoParseError(
                DemoErrorKind.PayloadTooLarge,
                lengthOffset,
                $"Payload length {length} exceeds the {DemoFormat.MaxPayloadBytes} byte sanity ceiling.");
            return false;
        }

        if (_knownLength >= 0 && length > _knownLength - _position)
        {
            error = new DemoParseError(
                DemoErrorKind.TruncatedInput,
                lengthOffset,
                $"Final payload declares {length} bytes but only {_knownLength - _position} remain; the incomplete command was discarded.");
            return false;
        }

        var buffer = new byte[length];
        if (!TryReadExact(buffer))
        {
            error = new DemoParseError(
                DemoErrorKind.TruncatedInput,
                lengthOffset,
                $"Stream ended after {buffer.Length} of {length} declared payload bytes.");
            return false;
        }

        payload = buffer;
        return true;
    }

    private void RecordReadFailure(DemoParseError error)
    {
        if (error.Kind == DemoErrorKind.TruncatedInput)
        {
            TailRecovery = error;
        }
        else
        {
            _errors.Add(error);
        }
    }

    private void RecoverTail(long offset, string message) =>
        TailRecovery = new DemoParseError(DemoErrorKind.TruncatedInput, offset, message);

    private int ReadByteOrEof()
    {
        var value = _stream.ReadByte();
        if (value >= 0)
        {
            _position++;
        }

        return value;
    }

    private bool TryReadInt32(out int value)
    {
        Span<byte> buffer = stackalloc byte[4];
        if (!TryReadExact(buffer))
        {
            value = 0;
            return false;
        }

        value = BinaryPrimitives.ReadInt32LittleEndian(buffer);
        return true;
    }

    private bool TryReadExact(Span<byte> destination)
    {
        var total = 0;
        while (total < destination.Length)
        {
            var read = _stream.Read(destination[total..]);
            if (read == 0)
            {
                _position += total;
                return false;
            }

            total += read;
        }

        _position += total;
        return true;
    }

    private void AddError(DemoErrorKind kind, long offset, string message) =>
        _errors.Add(new DemoParseError(kind, offset, message));

    public void Dispose()
    {
        if (!_leaveOpen)
        {
            _stream.Dispose();
        }
    }
}
