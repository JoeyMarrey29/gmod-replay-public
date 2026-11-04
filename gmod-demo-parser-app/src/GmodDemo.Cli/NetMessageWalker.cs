namespace GmodDemo.Parser;

/// <summary>Where a message sits within a packet payload, and how many bits it occupied.</summary>
public readonly record struct NetMessageSpan(NetMessageType Type, int StartBit, int LengthBits);

public enum PacketWalkStatus
{
    /// <summary>Every message parsed and only sub-byte padding remained.</summary>
    Clean,

    /// <summary>A message type outside the known table was encountered.</summary>
    UnknownMessageType,

    /// <summary>A message ran past the end of the payload, meaning a preceding width was wrong.</summary>
    Overflow,
}

public sealed record PacketWalkResult(
    PacketWalkStatus Status,
    IReadOnlyList<NetMessageSpan> Messages,
    int BitsConsumed,
    int BitsRemaining,
    NetMessageType? FailedAt);

/// <summary>
/// Splits a packet payload into its constituent network messages.
/// </summary>
/// <remarks>
/// Messages are packed back to back with no length prefix or alignment between them, so there is
/// no way to skip an unrecognised message and resynchronise. Every message's header must be read
/// correctly to find where the next one begins. This makes "did the payload consume to within a
/// byte of its end?" a strong correctness signal: a single wrong bit width derails the remainder
/// of the packet, so a high clean rate across thousands of packets is meaningful evidence that
/// the widths in <see cref="NetMessageLayout"/> are right.
/// </remarks>
public sealed class NetMessageWalker
{
    /// <summary>Overridable widths, used by calibration to test competing published values.</summary>
    internal int MaxEdictBits { get; init; } = NetMessageLayout.MaxEdictBits;

    internal int DeltaSizeBits { get; init; } = NetMessageLayout.DeltaSizeBits;

    internal int GModMessageLengthBits { get; init; } = NetMessageLayout.GModMessageLengthBits;

    internal int UserMessageLengthBits { get; init; } = NetMessageLayout.NetMessageLengthBits;

    /// <summary>
    /// Payload-length override for calibration: zero reads a varint, a positive value
    /// reads that many bits. By default the shared parser validates both layouts.
    /// </summary>
    internal int? CreateStringTableLengthBits { get; init; }

    public PacketWalkResult Walk(ReadOnlySpan<byte> payload)
    {
        var reader = new BitReader(payload);
        var messages = new List<NetMessageSpan>();

        // Stop with under a byte left: the recorder pads the final byte, so a stray sub-byte
        // remainder is expected rather than a parse failure.
        while (reader.BitsLeft >= 8)
        {
            var start = reader.BitsRead;
            var type = (NetMessageType)reader.ReadUBitLong(NetMessageLayout.TypeBits);

            if (!TrySkip(ref reader, type))
            {
                return new PacketWalkResult(
                    reader.IsOverflowed ? PacketWalkStatus.Overflow : PacketWalkStatus.UnknownMessageType,
                    messages,
                    reader.BitsRead,
                    reader.BitsLeft,
                    type);
            }

            if (reader.IsOverflowed)
            {
                return new PacketWalkResult(
                    PacketWalkStatus.Overflow, messages, reader.BitsRead, reader.BitsLeft, type);
            }

            messages.Add(new NetMessageSpan(type, start, reader.BitsRead - start));
        }

        return new PacketWalkResult(
            PacketWalkStatus.Clean, messages, reader.BitsRead, reader.BitsLeft, null);
    }

    /// <summary>
    /// Advances past one message whose type tag has already been consumed. Returns false if the
    /// type is unknown or the message runs off the end.
    /// </summary>
    public bool TrySkip(ref BitReader r, NetMessageType type)
    {
        switch (type)
        {
            case NetMessageType.NetNop:
                return true;

            case NetMessageType.NetDisconnect:
                r.ReadString();
                return true;

            case NetMessageType.NetFile:
                r.ReadUBitLong(32);
                if (r.ReadOneBit() != 0)
                {
                    r.ReadOneBit();
                    r.ReadUBitLong(32);
                }

                return true;

            case NetMessageType.NetTick:
                r.ReadUBitLong(32);
                r.ReadUBitLong(16);
                r.ReadUBitLong(16);
                return true;

            case NetMessageType.NetStringCmd:
            case NetMessageType.SvcPrint:
                r.ReadString();
                return true;

            case NetMessageType.NetSetConVar:
            {
                var count = r.ReadByte();
                for (var i = 0; i < count; i++)
                {
                    r.ReadString();
                    r.ReadString();
                }

                return true;
            }

            case NetMessageType.NetSignonState:
                r.ReadByte();
                r.ReadUBitLong(32);
                return true;

            case NetMessageType.SvcServerInfo:
                return TrySkipServerInfo(ref r);

            case NetMessageType.SvcSendTable:
            {
                r.ReadOneBit();
                var lengthBits = (int)r.ReadUBitLong(16);
                return r.SeekRelative(lengthBits);
            }

            case NetMessageType.SvcClassInfo:
            {
                var classCount = (int)r.ReadUBitLong(16);
                var createOnClient = r.ReadOneBit() != 0;
                if (createOnClient)
                {
                    return true;
                }

                var idBits = BitMath.Log2(classCount) + 1;
                for (var i = 0; i < classCount; i++)
                {
                    r.ReadUBitLong(idBits);
                    r.ReadString();
                    r.ReadString();
                }

                return true;
            }

            case NetMessageType.SvcSetPause:
                r.ReadOneBit();
                return true;

            case NetMessageType.SvcCreateStringTable:
                return StringTableParser.SkipCreate(ref r, CreateStringTableLengthBits);

            case NetMessageType.SvcUpdateStringTable:
                return StringTableParser.SkipUpdate(ref r);

            case NetMessageType.SvcVoiceInit:
            {
                r.ReadString();
                var quality = r.ReadByte();

                // The 2017 revision signals its presence with a sentinel quality of 255 followed
                // by an explicit sample rate; older builds infer the rate from the codec name.
                if (quality == 255)
                {
                    r.ReadUBitLong(16);
                }

                return true;
            }

            case NetMessageType.SvcVoiceData:
            {
                r.ReadByte();
                r.ReadByte();
                var lengthBits = (int)r.ReadUBitLong(16);
                return r.SeekRelative(lengthBits);
            }

            case NetMessageType.SvcSounds:
            {
                var reliable = r.ReadOneBit() != 0;
                int lengthBits;
                if (reliable)
                {
                    lengthBits = (int)r.ReadUBitLong(8);
                }
                else
                {
                    r.ReadUBitLong(8);
                    lengthBits = (int)r.ReadUBitLong(16);
                }

                return r.SeekRelative(lengthBits);
            }

            case NetMessageType.SvcSetView:
                r.ReadUBitLong(MaxEdictBits);
                return true;

            case NetMessageType.SvcFixAngle:
                r.ReadOneBit();
                r.ReadUBitLong(16);
                r.ReadUBitLong(16);
                r.ReadUBitLong(16);
                return true;

            case NetMessageType.SvcCrosshairAngle:
                r.ReadUBitLong(16);
                r.ReadUBitLong(16);
                r.ReadUBitLong(16);
                return true;

            case NetMessageType.SvcBspDecal:
            {
                r.ReadBitVec3Coord();
                r.ReadUBitLong(NetMessageLayout.MaxDecalIndexBits);
                if (r.ReadOneBit() != 0)
                {
                    r.ReadUBitLong(MaxEdictBits);
                    r.ReadUBitLong(NetMessageLayout.SpModelIndexBits);
                }

                r.ReadOneBit();
                return true;
            }

            case NetMessageType.SvcUserMessage:
            {
                r.ReadByte();
                var lengthBits = (int)r.ReadUBitLong(UserMessageLengthBits);
                return r.SeekRelative(lengthBits);
            }

            case NetMessageType.SvcEntityMessage:
            {
                r.ReadUBitLong(MaxEdictBits);
                r.ReadUBitLong(NetMessageLayout.MaxServerClassBits);
                var lengthBits = (int)r.ReadUBitLong(NetMessageLayout.NetMessageLengthBits);
                return r.SeekRelative(lengthBits);
            }

            case NetMessageType.SvcGameEvent:
            {
                var lengthBits = (int)r.ReadUBitLong(NetMessageLayout.NetMessageLengthBits);
                return r.SeekRelative(lengthBits);
            }

            case NetMessageType.SvcPacketEntities:
            {
                r.ReadUBitLong(MaxEdictBits);
                var isDelta = r.ReadOneBit() != 0;
                if (isDelta)
                {
                    r.ReadUBitLong(32);
                }

                r.ReadUBitLong(1);
                r.ReadUBitLong(MaxEdictBits);
                var lengthBits = (int)r.ReadUBitLong(DeltaSizeBits);
                r.ReadOneBit();
                return r.SeekRelative(lengthBits);
            }

            case NetMessageType.SvcTempEntities:
            {
                r.ReadUBitLong(8);

                // Network protocol 24 is above the threshold where this became a varint.
                var lengthBits = (int)r.ReadVarInt32();
                return r.SeekRelative(lengthBits);
            }

            case NetMessageType.SvcPrefetch:
                r.ReadUBitLong(NetMessageLayout.SoundIndexBits);
                return true;

            case NetMessageType.SvcMenu:
            {
                r.ReadUBitLong(16);
                var lengthBytes = (int)r.ReadUBitLong(16);
                return r.SeekRelative(lengthBytes * 8);
            }

            case NetMessageType.SvcGameEventList:
            {
                r.ReadUBitLong(NetMessageLayout.MaxEventBits);
                var lengthBits = (int)r.ReadUBitLong(20);
                return r.SeekRelative(lengthBits);
            }

            case NetMessageType.SvcGetCvarValue:
                r.ReadUBitLong(32);
                r.ReadString();
                return true;

            case NetMessageType.SvcCmdKeyValues:
            {
                var lengthBytes = (int)r.ReadUBitLong(32);
                if (lengthBytes < 0)
                {
                    return false;
                }

                return r.SeekRelative(lengthBytes * 8);
            }

            case NetMessageType.SvcGModServerToClient:
            {
                // The declared length covers everything after itself, including the submessage
                // type byte, so the whole remainder can be skipped in one step.
                var lengthBits = (int)r.ReadUBitLong(GModMessageLengthBits);
                return r.SeekRelative(lengthBits);
            }

            default:
                return false;
        }
    }

    private static bool TrySkipServerInfo(ref BitReader r)
    {
        r.ReadUBitLong(16);
        r.ReadUBitLong(32);
        r.ReadOneBit();
        r.ReadOneBit();
        r.ReadUBitLong(32);
        r.ReadUBitLong(16);

        // Protocol 24 is past the point where the map checksum became a 16-byte MD5.
        r.SeekRelative(128);

        r.ReadByte();
        r.ReadByte();
        r.ReadUBitLong(32);
        r.ReadByte();
        r.ReadString();
        r.ReadString();
        r.ReadString();
        r.ReadString();

        // Garry's Mod appends the loading screen URL and the gamemode name.
        r.ReadString();
        r.ReadString();

        return !r.IsOverflowed;
    }
}
