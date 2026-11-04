using System.Text;

namespace GmodDemo.Parser;

public sealed record VoiceInitInfo(string Codec, int Quality, int SampleRate);

public sealed record VoicePacket(int Tick, int Slot, bool Proximity, byte[] Payload);

/// <summary>
/// Collects <c>svc_VoiceInit</c> / <c>svc_VoiceData</c>. Current GMod uses Steam's compressed
/// codec (Opus PLC inside a Steam frame). Each speech line is decoded to a 16-bit mono WAV
/// so it can be opened in VLC; play that file at the line's first tick.
/// </summary>
public sealed class VoiceTrack
{
    public const string FileMagic = "GMODVOICE";

    public const int FileVersion = 1;

    /// <summary>Tick gap that starts a new speech line (~300 ms at 66.67 tick).</summary>
    public const int UtteranceGapTicks = 20;

    private readonly List<VoicePacket> _packets = [];

    public VoiceInitInfo? Init { get; private set; }

    public IReadOnlyList<VoicePacket> Packets => _packets;

    public VoiceInitInfo? ResolvedInit
    {
        get
        {
            if (Init is { } known)
            {
                return known;
            }

            foreach (var packet in _packets)
            {
                if (SteamVoice.TryParse(packet.Payload, out var frame) && frame.SampleRate > 0)
                {
                    return new VoiceInitInfo("steam", 255, frame.SampleRate);
                }
            }

            return _packets.Any(p => SteamVoice.HasMagic(p.Payload))
                ? new VoiceInitInfo("steam", 255, 0)
                : null;
        }
    }

    public void AddInit(string codec, int quality, int sampleRate) =>
        Init ??= new VoiceInitInfo(codec, quality, sampleRate);

    public void AddPacket(int tick, int slot, bool proximity, byte[] payload)
    {
        if (payload.Length == 0)
        {
            return;
        }

        _packets.Add(new VoicePacket(tick, slot, proximity, payload));
    }

    public void TryReadInitFromPayload(byte[] payload)
    {
        if (Init is not null)
        {
            return;
        }

        foreach (var needle in new[] { "steam"u8.ToArray(), "vaudio_celt"u8.ToArray(), "vaudio_speex"u8.ToArray() })
        {
            var at = payload.AsSpan().IndexOf(needle);
            if (at < 0)
            {
                continue;
            }

            for (var extra = 0; extra <= 48; extra++)
            {
                var bit = at * 8 - extra;
                if (bit < NetMessageLayout.TypeBits)
                {
                    continue;
                }

                var reader = new BitReader(payload);
                reader.Seek(bit - NetMessageLayout.TypeBits);
                if (reader.ReadUBitLong(NetMessageLayout.TypeBits) != (uint)NetMessageType.SvcVoiceInit)
                {
                    continue;
                }

                var codec = reader.ReadString(32);
                var quality = reader.ReadByte();
                var rate = quality == 255 ? (int)reader.ReadUBitLong(16) : 0;
                if (!reader.IsOverflowed && codec.Length > 0)
                {
                    AddInit(codec, quality, rate);
                    return;
                }
            }
        }
    }

    public static void ReadVoiceData(ref BitReader reader, int tick, VoiceTrack track)
    {
        var slot = reader.ReadByte();
        var proximity = reader.ReadByte() != 0;
        var lengthBits = (int)reader.ReadUBitLong(16);
        if (lengthBits <= 0 || reader.IsOverflowed)
        {
            return;
        }

        var payload = reader.ReadBitsAsBytes(Math.Min(lengthBits, reader.BitsLeft));
        track.AddPacket(tick, slot, proximity, payload);
    }

    /// <summary>
    /// Writes one file per speech line (same slot, packets closer than <see cref="UtteranceGapTicks"/>).
    /// Returns JSON-relative paths; play each file at <see cref="VoiceClip.Time"/>.
    /// </summary>
    public IReadOnlyList<VoiceClip> WriteSidecars(
        string jsonPath,
        IReadOnlyList<DemoPlayer> players,
        double tickRate,
        Func<int, int, DemoPlayer?>? playerAtSlot = null)
    {
        if (_packets.Count == 0)
        {
            return [];
        }

        var audioDir = Path.ChangeExtension(jsonPath, ".audio");
        Directory.CreateDirectory(audioDir);
        var jsonDir = Path.GetDirectoryName(jsonPath) ?? "";
        var clips = new List<VoiceClip>();
        var usedNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        foreach (var group in _packets.GroupBy(p => p.Slot).OrderBy(g => g.Key))
        {
            var slot = group.Key;
            var fallbackPlayer = players.FirstOrDefault(p => p.Slot == slot);
            var utterances = new List<(List<VoicePacket> Packets, DemoPlayer? Player)>();
            List<VoicePacket>? current = null;
            DemoPlayer? currentPlayer = null;
            foreach (var packet in group.OrderBy(p => p.Tick))
            {
                var packetPlayer = playerAtSlot is null ? fallbackPlayer : playerAtSlot(slot, packet.Tick);
                if (current is null || currentPlayer?.Id != packetPlayer?.Id
                    || packet.Proximity != current[^1].Proximity
                    || packet.Tick - current[^1].Tick > UtteranceGapTicks)
                {
                    current = [];
                    utterances.Add((current, packetPlayer));
                    currentPlayer = packetPlayer;
                }

                current.Add(packet);
            }

            foreach (var (ordered, player) in utterances)
            {
                var first = ordered[0].Tick;
                var last = ordered[^1].Tick;
                var id = player?.SteamId64 is { Length: > 0 } steam ? steam : $"slot{slot}";
                var wav = VoiceDecoder.TryDecode(ordered, out var pcm, out var sampleRate);
                var ext = wav ? ".wav" : ".gmodvoice";
                var fileName = UniqueName(usedNames, $"{id}_{first}{ext}");
                var fullPath = Path.Combine(audioDir, fileName);
                if (wav)
                {
                    WaveFile.Write(fullPath, sampleRate, pcm);
                }
                else
                {
                    WriteVoiceFile(fullPath, ordered);
                }

                var relative = Path.GetRelativePath(jsonDir, fullPath).Replace('\\', '/');
                var duration = wav && sampleRate > 0
                    ? pcm.Length / (double)sampleRate
                    : tickRate > 0 ? (last - first + 1) / tickRate : 0;
                clips.Add(new VoiceClip(
                    first,
                    last,
                    tickRate > 0 ? first / tickRate : 0,
                    duration,
                    player?.Id,
                    player?.Name,
                    player?.SteamId,
                    player?.SteamId64,
                    slot,
                    relative,
                    ordered.Count,
                    wav ? pcm.Length * 2 : ordered.Sum(p => p.Payload.Length))
                {
                    Proximity = ordered[0].Proximity,
                    HasPlayerAssignment = player is not null,
                });
            }
        }

        return clips;
    }

    public static IReadOnlyList<IReadOnlyList<VoicePacket>> SplitUtterances(
        IEnumerable<VoicePacket> packets,
        int gapTicks = UtteranceGapTicks)
    {
        var lines = new List<IReadOnlyList<VoicePacket>>();
        List<VoicePacket>? current = null;
        foreach (var packet in packets.OrderBy(p => p.Tick))
        {
            if (current is null || packet.Proximity != current[^1].Proximity
                || packet.Tick - current[^1].Tick > gapTicks)
            {
                current = [packet];
                lines.Add(current);
            }
            else
            {
                current.Add(packet);
            }
        }

        return lines;
    }

    private static string UniqueName(HashSet<string> used, string fileName)
    {
        if (used.Add(fileName))
        {
            return fileName;
        }

        var stem = Path.GetFileNameWithoutExtension(fileName);
        var ext = Path.GetExtension(fileName);
        for (var n = 2; ; n++)
        {
            var candidate = $"{stem}_{n}{ext}";
            if (used.Add(candidate))
            {
                return candidate;
            }
        }
    }

    private static void WriteVoiceFile(string path, IReadOnlyList<VoicePacket> packets)
    {
        using var stream = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.Read);
        using var writer = new BinaryWriter(stream, Encoding.ASCII, leaveOpen: false);
        writer.Write(Encoding.ASCII.GetBytes(FileMagic));
        writer.Write(FileVersion);
        writer.Write(packets.Count);
        foreach (var packet in packets)
        {
            writer.Write(packet.Tick);
            writer.Write(packet.Proximity ? (byte)1 : (byte)0);
            writer.Write(packet.Payload.Length);
            writer.Write(packet.Payload);
        }
    }
}

public sealed record VoiceClip(
    int StartTick,
    int EndTick,
    double Time,
    double Duration,
    int? P,
    string? Name,
    string? SteamId,
    string? SteamId64,
    int Slot,
    string File,
    int Packets,
    int Bytes)
{
    public bool Proximity { get; init; } = true;

    internal bool HasPlayerAssignment { get; init; }
}
