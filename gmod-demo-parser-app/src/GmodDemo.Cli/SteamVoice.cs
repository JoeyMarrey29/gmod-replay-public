using System.Buffers.Binary;

namespace GmodDemo.Parser;

/// <summary>
/// Steam voice framing used by current GMod (<c>sv_voicecodec steam</c>). Payloads wrap Opus PLC
/// frames (type 6) plus an optional sample-rate opcode (type 11, typically 24000).
/// </summary>
public readonly record struct SteamVoiceFrame(int SampleRate, int OpusBytes, int SilenceSamples);

public enum SteamVoiceOpKind
{
    SampleRate,
    Silence,
    Opus,
    Reset,
}

public readonly record struct SteamVoiceOp(SteamVoiceOpKind Kind, int Value, byte[]? Packet);

public static class SteamVoice
{
    public static ReadOnlySpan<byte> Magic => [0xFF, 0x39, 0x32, 0x46];

    public static bool HasMagic(ReadOnlySpan<byte> payload) =>
        payload.Length >= 4 && payload.StartsWith(Magic);

    public static bool TryParse(ReadOnlySpan<byte> data, out SteamVoiceFrame frame)
    {
        if (!TryEnumerate(data, out _, out var sampleRate, out var opusBytes, out var silence))
        {
            frame = default;
            return false;
        }

        frame = new SteamVoiceFrame(sampleRate, opusBytes, silence);
        return true;
    }

    public static bool TryEnumerate(ReadOnlySpan<byte> data, out List<SteamVoiceOp> ops) =>
        TryEnumerate(data, out ops, out _, out _, out _);

    public static bool TryEnumerate(
        ReadOnlySpan<byte> data,
        out List<SteamVoiceOp> ops,
        out int sampleRate,
        out int opusBytes,
        out int silence)
    {
        ops = [];
        sampleRate = 0;
        opusBytes = 0;
        silence = 0;
        if (data.Length < 12)
        {
            return false;
        }

        var crcStored = BinaryPrimitives.ReadUInt32LittleEndian(data[^4..]);
        var body = data[..^4];
        if (Crc32(body) != crcStored || body.Length < 8)
        {
            return false;
        }

        var payload = body[8..];
        var i = 0;
        while (i < payload.Length)
        {
            var type = payload[i++];
            if (i + 2 > payload.Length)
            {
                return false;
            }

            var n = BinaryPrimitives.ReadUInt16LittleEndian(payload[i..]);
            i += 2;
            switch (type)
            {
                case 0:
                    silence += n;
                    ops.Add(new SteamVoiceOp(SteamVoiceOpKind.Silence, n, null));
                    break;
                case 11:
                    sampleRate = n;
                    ops.Add(new SteamVoiceOp(SteamVoiceOpKind.SampleRate, n, null));
                    break;
                case 1:
                case 2:
                case 3:
                case 4:
                case 5:
                case 6:
                    if (i + n > payload.Length)
                    {
                        return false;
                    }

                    var blob = payload.Slice(i, n);
                    i += n;
                    if (type == 6)
                    {
                        opusBytes += n;
                        ReadOpusPlc(blob, ops);
                    }

                    break;
                default:
                    return false;
            }
        }

        return true;
    }

    private static void ReadOpusPlc(ReadOnlySpan<byte> data, List<SteamVoiceOp> ops)
    {
        var i = 0;
        var any = false;
        while (i + 2 <= data.Length)
        {
            var len = BinaryPrimitives.ReadUInt16LittleEndian(data[i..]);
            i += 2;
            if (len == 0xFFFF)
            {
                ops.Add(new SteamVoiceOp(SteamVoiceOpKind.Reset, 0, null));
                any = true;
                continue;
            }

            if (i + 2 > data.Length)
            {
                break;
            }

            var seq = BinaryPrimitives.ReadUInt16LittleEndian(data[i..]);
            i += 2;
            if (i + len > data.Length)
            {
                break;
            }

            ops.Add(new SteamVoiceOp(SteamVoiceOpKind.Opus, seq, data.Slice(i, (int)len).ToArray()));
            i += len;
            any = true;
        }

        if (!any && data.Length > 0)
        {
            ops.Add(new SteamVoiceOp(SteamVoiceOpKind.Opus, 0, data.ToArray()));
        }
    }

    public static uint Crc32(ReadOnlySpan<byte> data)
    {
        uint crc = 0xFFFFFFFF;
        foreach (var b in data)
        {
            crc ^= b;
            for (var bit = 0; bit < 8; bit++)
            {
                var mask = (uint)-(int)(crc & 1);
                crc = (crc >> 1) ^ (0xEDB88320u & mask);
            }
        }

        return ~crc;
    }
}
