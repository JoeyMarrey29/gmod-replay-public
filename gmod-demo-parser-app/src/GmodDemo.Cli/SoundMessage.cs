namespace GmodDemo.Parser;

/// <summary>One sound the server told the client to start, change or stop (svc_Sounds).</summary>
public sealed record SoundRow(
    int Tick,
    int Entity,
    int SoundNum,
    int Flags,
    int Channel,
    bool Ambient,
    bool Sentence,
    float Volume,
    int Level,
    int Pitch,
    float Delay,
    float X,
    float Y,
    float Z,
    int Speaker)
{
    /// <summary>SND_STOP: the sound on this entity and channel is cut.</summary>
    public bool IsStop => Flags == 4;
}

/// <summary>
/// Decodes svc_Sounds. The layout is SoundInfo_t::ReadDelta from the Source SDK
/// (<c>public/soundinfo.h</c>, network protocol 24): every field is delta-coded against the
/// previous sound of the same message, starting from the engine defaults. Garry's Mod sends
/// the flags in 12 bits rather than the SDK's 11; with that, every message of a recording
/// decodes to exactly its length.
/// </summary>
internal static class SoundMessage
{
    private const int SoundFlagBits = 12;
    private const int MaxSoundLevelBits = 9;
    private const int DelayBits = 13;
    private const int CoordBits = 12;
    private const float DelayOffset = 0.100f;

    public static List<SoundRow> Read(ref BitReader reader, int tick, int count, int totalBits)
    {
        var rows = new List<SoundRow>(count);
        var end = reader.BitsRead + totalBits;

        var entity = 0;
        var soundNum = 0;
        var flags = 0;
        var channel = 6;
        var volume = 1.0f;
        var level = 75;
        var pitch = 100;
        var delay = 0.0f;
        float x = 0, y = 0, z = 0;
        var speaker = -1;

        for (var i = 0; i < count && reader.BitsRead < end && !reader.IsOverflowed; i++)
        {
            if (reader.ReadOneBit() != 0)
            {
                entity = reader.ReadOneBit() != 0
                    ? (int)reader.ReadUBitLong(5)
                    : (int)reader.ReadUBitLong(NetMessageLayout.MaxEdictBits);
            }

            if (reader.ReadOneBit() != 0)
            {
                soundNum = (int)reader.ReadUBitLong(NetMessageLayout.SoundIndexBits);
            }

            if (reader.ReadOneBit() != 0)
            {
                flags = (int)reader.ReadUBitLong(SoundFlagBits);
            }

            if (reader.ReadOneBit() != 0)
            {
                channel = (int)reader.ReadUBitLong(3);
            }

            var ambient = reader.ReadOneBit() != 0;
            var sentence = reader.ReadOneBit() != 0;

            if (flags != 4)
            {
                if (reader.ReadOneBit() == 0 && reader.ReadOneBit() == 0)
                {
                    reader.ReadUBitLong(10);
                }

                if (reader.ReadOneBit() != 0)
                {
                    volume = reader.ReadUBitLong(7) / 127.0f;
                }

                if (reader.ReadOneBit() != 0)
                {
                    level = (int)reader.ReadUBitLong(MaxSoundLevelBits);
                }

                if (reader.ReadOneBit() != 0)
                {
                    pitch = (int)reader.ReadUBitLong(8);
                }

                if (reader.ReadOneBit() != 0)
                {
                    reader.ReadUBitLong(8);
                }

                if (reader.ReadOneBit() != 0)
                {
                    delay = reader.ReadSBitLong(DelayBits) / 1000.0f;
                    if (delay < 0)
                    {
                        delay *= 10.0f;
                    }

                    delay -= DelayOffset;
                }

                if (reader.ReadOneBit() != 0)
                {
                    x = 8.0f * reader.ReadSBitLong(CoordBits);
                }

                if (reader.ReadOneBit() != 0)
                {
                    y = 8.0f * reader.ReadSBitLong(CoordBits);
                }

                if (reader.ReadOneBit() != 0)
                {
                    z = 8.0f * reader.ReadSBitLong(CoordBits);
                }

                if (reader.ReadOneBit() != 0)
                {
                    speaker = reader.ReadSBitLong(NetMessageLayout.MaxEdictBits + 1);
                }
            }
            else
            {
                // ClearStopFields
                volume = 0;
                level = 0;
                pitch = 100;
                delay = 0;
                x = y = z = 0;
                speaker = -1;
            }

            if (reader.IsOverflowed)
            {
                break;
            }

            rows.Add(new SoundRow(
                tick, entity, soundNum, flags, channel, ambient, sentence,
                volume, level, pitch, delay, x, y, z, speaker));
        }

        // Only messages that decode to exactly their length are trusted.
        return reader.BitsRead == end ? rows : [];
    }
}
