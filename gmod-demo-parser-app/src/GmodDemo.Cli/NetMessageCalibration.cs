namespace GmodDemo.Parser;

/// <summary>One combination of the bit widths that published sources disagree about.</summary>
public readonly record struct LayoutCandidate(
    int MaxEdictBits,
    int DeltaSizeBits,
    int GModMessageLengthBits,
    int UserMessageLengthBits)
{
    public override string ToString() =>
        $"edict={MaxEdictBits} delta={DeltaSizeBits} gmod={GModMessageLengthBits} usermsg={UserMessageLengthBits}";
}

public sealed record LayoutScore(
    LayoutCandidate Candidate,
    int PacketsClean,
    int PacketsTotal,
    int SignonClean,
    int SignonTotal,
    IReadOnlyDictionary<NetMessageType, int> FailuresByType)
{
    public double PacketCleanRate => PacketsTotal == 0 ? 0 : (double)PacketsClean / PacketsTotal;

    public double SignonCleanRate => SignonTotal == 0 ? 0 : (double)SignonClean / SignonTotal;
}

/// <summary>
/// Determines which candidate bit widths actually decode a real demo.
/// </summary>
/// <remarks>
/// Several Garry's Mod field widths are reported inconsistently across community sources, and
/// the engine is closed so no authoritative reference exists. Because messages carry no framing
/// between them, a wrong width derails the rest of the packet — so the fraction of packets that
/// consume cleanly separates a correct guess from an incorrect one very sharply, usually
/// near 100% against near 0%.
/// </remarks>
public static class NetMessageCalibration
{
    public static IReadOnlyList<LayoutScore> Run(
        string demoPath,
        IEnumerable<LayoutCandidate>? candidates = null)
    {
        var (packets, signon) = LoadPayloads(demoPath);
        return Run(packets, signon, candidates);
    }

    public static IReadOnlyList<LayoutScore> Run(
        IReadOnlyList<byte[]> packetPayloads,
        IReadOnlyList<byte[]> signonPayloads,
        IEnumerable<LayoutCandidate>? candidates = null)
    {
        candidates ??= DefaultCandidates();

        var scores = new List<LayoutScore>();
        foreach (var candidate in candidates)
        {
            var walker = new NetMessageWalker
            {
                MaxEdictBits = candidate.MaxEdictBits,
                DeltaSizeBits = candidate.DeltaSizeBits,
                GModMessageLengthBits = candidate.GModMessageLengthBits,
                UserMessageLengthBits = candidate.UserMessageLengthBits,
            };

            var failures = new Dictionary<NetMessageType, int>();
            var packetsClean = Score(walker, packetPayloads, failures);
            var signonClean = Score(walker, signonPayloads, failures);

            scores.Add(new LayoutScore(
                candidate,
                packetsClean,
                packetPayloads.Count,
                signonClean,
                signonPayloads.Count,
                failures));
        }

        return scores;
    }

    private static int Score(
        NetMessageWalker walker,
        IReadOnlyList<byte[]> payloads,
        Dictionary<NetMessageType, int> failures)
    {
        var clean = 0;
        foreach (var payload in payloads)
        {
            var result = walker.Walk(payload);
            if (result.Status == PacketWalkStatus.Clean)
            {
                clean++;
            }
            else if (result.FailedAt is { } failedAt)
            {
                failures[failedAt] = failures.GetValueOrDefault(failedAt) + 1;
            }
        }

        return clean;
    }

    /// <summary>
    /// Scores candidate encodings for the svc_CreateStringTable payload length against the
    /// signon burst, where every string table is created. Reports how many string tables each
    /// candidate recovers, since a wrong width stops the walk at the first table.
    /// </summary>
    public static IReadOnlyList<(int LengthBits, int SignonClean, int TablesFound)> CalibrateStringTables(
        IReadOnlyList<byte[]> signonPayloads,
        IEnumerable<int>? lengthWidths = null)
    {
        // Zero stands for the varint encoding used by newer Valve branches.
        lengthWidths ??= [0, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 32];

        var results = new List<(int, int, int)>();
        foreach (var width in lengthWidths)
        {
            var walker = new NetMessageWalker { CreateStringTableLengthBits = width };
            var clean = 0;
            var tables = 0;

            foreach (var payload in signonPayloads)
            {
                var result = walker.Walk(payload);
                if (result.Status == PacketWalkStatus.Clean)
                {
                    clean++;
                }

                tables += result.Messages.Count(m => m.Type == NetMessageType.SvcCreateStringTable);
            }

            results.Add((width, clean, tables));
        }

        return results;
    }

    public static IEnumerable<LayoutCandidate> DefaultCandidates()
    {
        // Each axis pairs the stock Source value against the competing Garry's Mod claim.
        foreach (var edict in new[] { 11, 13 })
        {
            foreach (var delta in new[] { 20, 24 })
            {
                foreach (var gmod in new[] { 20, 32 })
                {
                    foreach (var userMessage in new[] { 11, 12 })
                    {
                        yield return new LayoutCandidate(edict, delta, gmod, userMessage);
                    }
                }
            }
        }
    }

    public static (List<byte[]> Packets, List<byte[]> Signon) LoadPayloads(
        string demoPath,
        Action<long>? progress = null)
    {
        var packets = new List<byte[]>();
        var signon = new List<byte[]>();

        using var reader = DemoReader.Open(demoPath);
        foreach (var command in reader.ReadCommands())
        {
            progress?.Invoke(reader.Position);
            if (command is PacketCommand packet)
            {
                (packet.Type == DemoCommandType.Signon ? signon : packets).Add(packet.Payload);
            }
        }

        return (packets, signon);
    }
}
