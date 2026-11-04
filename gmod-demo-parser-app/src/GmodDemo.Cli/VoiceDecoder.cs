using Concentus;
using Concentus.Structs;

namespace GmodDemo.Parser;

/// <summary>
/// Unwraps Steam voice frames and decodes the inner Opus PLC packets to 16-bit mono PCM.
/// Steam's sample-rate opcode is often 24000 (capture / requested rate); the packets are
/// standard Opus and must be decoded at 48 kHz. Decoding at 24 kHz aliases into a hiss.
/// </summary>
public static class VoiceDecoder
{
    public const int OutputSampleRate = 48000;

    public static bool TryDecode(IReadOnlyList<VoicePacket> packets, out short[] pcm, out int sampleRate)
    {
        pcm = [];
        sampleRate = OutputSampleRate;
        IOpusDecoder? decoder = null;
        var samples = new List<short>(packets.Count * 960);
        var buffer = new short[5760];
        int? nextSeq = null;

        try
        {
            decoder = OpusCodecFactory.CreateDecoder(OutputSampleRate, 1);
            foreach (var packet in packets)
            {
                if (!SteamVoice.TryEnumerate(packet.Payload, out var ops))
                {
                    continue;
                }

                foreach (var op in ops)
                {
                    switch (op.Kind)
                    {
                        case SteamVoiceOpKind.Silence when op.Value > 0:
                            // Opcode 0 counts samples at the Steam capture rate (typically 24 kHz).
                            samples.AddRange(new short[op.Value * OutputSampleRate / 24000]);
                            break;
                        case SteamVoiceOpKind.Reset:
                            decoder.ResetState();
                            nextSeq = null;
                            break;
                        case SteamVoiceOpKind.Opus when op.Packet is { Length: > 0 }:
                            if (nextSeq is { } expected)
                            {
                                if (op.Value < expected)
                                {
                                    decoder.ResetState();
                                }
                                else
                                {
                                    var lost = Math.Min(op.Value - expected, 10);
                                    try
                                    {
                                        AppendPlc(decoder, samples, buffer, lost);
                                    }
                                    catch (Exception)
                                    {
                                    }
                                }
                            }

                            nextSeq = op.Value + 1;
                            try
                            {
                                AppendOpus(decoder, samples, buffer, op.Packet);
                            }
                            catch (Exception)
                            {
                            }

                            break;
                    }
                }
            }
        }
        catch (Exception)
        {
            decoder?.Dispose();
            return false;
        }

        decoder?.Dispose();
        if (samples.Count == 0)
        {
            return false;
        }

        pcm = [.. samples];
        return true;
    }

    private static void AppendOpus(IOpusDecoder decoder, List<short> samples, short[] buffer, byte[] packet)
    {
        var frame = 960;
        try
        {
            var hinted = OpusPacketInfo.GetNumSamples(packet, OutputSampleRate);
            if (hinted > 0)
            {
                frame = hinted;
            }
        }
        catch (Exception)
        {
        }

        var count = decoder.Decode(packet, buffer, Math.Max(frame, 960), false);
        if (count > 0)
        {
            samples.AddRange(buffer.AsSpan(0, count).ToArray());
        }
    }

    private static void AppendPlc(IOpusDecoder decoder, List<short> samples, short[] buffer, int lost)
    {
        if (lost <= 0)
        {
            return;
        }

        var frame = decoder.LastPacketDuration > 0 ? decoder.LastPacketDuration : 960;
        for (var i = 0; i < lost; i++)
        {
            var count = decoder.Decode([], buffer.AsSpan(0, frame), frame, false);
            if (count > 0)
            {
                samples.AddRange(buffer.AsSpan(0, count).ToArray());
            }
        }
    }
}
