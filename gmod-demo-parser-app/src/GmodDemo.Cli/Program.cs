using System.Text.Json;
using GmodDemo.Parser;

namespace GmodDemo.Cli;

internal static partial class Program
{
    private static readonly HashSet<string> Commands = new(StringComparer.OrdinalIgnoreCase)
    {
        "info", "json", "calibrate", "messages", "serverinfo", "dump-signon", "find", "bits",
        "solve-tables", "search", "gmod-at", "gmod-ids", "tables", "players", "window",
        "probe-tables", "datatables", "perp-data",
    };

    private static int Main(string[] args)
    {
        Term.Init();
        var (command, rest) = SplitArgs(args);
        try
        {
            if (command is "" or "help")
            {
                if (rest.Length > 0 && rest[0] is "more" or "diagnostics")
                {
                    PrintDiagnosticsHelp();
                }
                else
                {
                    PrintUsage();
                }

                return command == "" ? 1 : 0;
            }

            using var report = CliReport.Attach(command, rest);
            return command switch
            {
                "info" => RunInfo(rest),
                "json" => RunJson(rest),
                "calibrate" => RunCalibrate(rest),
                "messages" => RunMessages(rest),
                "serverinfo" => RunServerInfo(rest),
                "dump-signon" => RunDumpSignon(rest),
                "find" => RunFind(rest),
                "bits" => RunBits(rest),
                "solve-tables" => RunSolveTables(rest),
                "search" => RunSearch(rest),
                "gmod-at" => RunGmodAt(rest),
                "gmod-ids" => RunGmodIds(rest),
                "tables" => RunTables(rest),
                "players" => RunPlayers(rest),
                "window" => RunWindow(rest),
                "probe-tables" => RunProbeTables(rest),
                "datatables" => RunDataTables(rest),
                "perp-data" => RunPerpData(rest),
                _ => Fail($"Unknown command '{command}'."),
            };
        }
        catch (DemoFormatException ex)
        {
            Console.Error.WriteLine(Term.Fail("Not a parsable demo") + Term.Muted("  " + ex.Message));
            return 2;
        }
        catch (FileNotFoundException ex)
        {
            Console.Error.WriteLine(Term.Fail("File not found") + Term.Muted("  " + (ex.FileName ?? "")));
            return 2;
        }
        catch (IOException ex)
        {
            Console.Error.WriteLine(Term.Fail("I/O error") + Term.Muted("  " + ex.Message));
            return 2;
        }
        finally
        {
            Term.PauseIfLoneConsole();
        }
    }

    private static (string Command, string[] Tail) SplitArgs(string[] args)
    {
        if (args.Length == 0)
        {
            return ("", []);
        }

        if (args[0] is "-h" or "--help" or "help")
        {
            return ("help", args[1..]);
        }

        if (Commands.Contains(args[0]))
        {
            return (args[0], args[1..]);
        }

        if (!args[0].StartsWith('-') && LooksLikeDemo(args[0]))
        {
            return ("json", args);
        }

        return (args[0], args[1..]);
    }

    private static bool LooksLikeDemo(string value) =>
        value.EndsWith(".dem", StringComparison.OrdinalIgnoreCase) || File.Exists(value);

    private static void PrintUsage()
    {
        Console.WriteLine();
        Console.WriteLine("  " + Term.Title("gmoddem"));
        Console.WriteLine("  " + Term.Muted("Parse Garry's Mod demo files"));
        Console.WriteLine();
        Console.WriteLine("  " + Term.Accent("Usage"));
        Console.WriteLine("    gmoddem <file.dem>              " + Term.Muted("export JSON next to the demo"));
        Console.WriteLine("    gmoddem <command> <file.dem>");
        Console.WriteLine();
        Console.WriteLine("  " + Term.Accent("Commands"));
        Console.WriteLine("    json       " + Term.Muted("timeline, net log, lua paths, voice  (default)"));
        Console.WriteLine("    info       " + Term.Muted("header, checks, command counts"));
        Console.WriteLine("    players    " + Term.Muted("who was in the demo"));
        Console.WriteLine("    window     " + Term.Muted("JSON for a tick range  (start 0, count 30)"));
        Console.WriteLine("    datatables " + Term.Muted("send tables and entity decode stats"));
        Console.WriteLine("    perp-data  " + Term.Muted("Perpheads jobs/weapons/clothing JSON from the demo's Lua  (-o <dir>)"));
        Console.WriteLine();
        Console.WriteLine("  " + Term.Accent("json options"));
        Console.WriteLine("    -o, --out <path>   " + Term.Muted("output JSON path"));
        Console.WriteLine("    --compact          " + Term.Muted("minified JSON (pretty is the default)"));
        Console.WriteLine("    --commands         " + Term.Muted("also dump every raw demo command"));
        Console.WriteLine("    --include-raw      " + Term.Muted("Base64 on undecoded payloads"));
        Console.WriteLine("    --max-commands <n> " + Term.Muted("cap raw commands (with --commands)"));
        Console.WriteLine("    --gluapack <bsp>   " + Term.Muted("unpack client Lua from this .bsp"));
        Console.WriteLine("    --no-gluapack      " + Term.Muted("leave packed Lua as placeholders"));
        Console.WriteLine("    --perp-data <dir>  " + Term.Muted("use/build the PERP data for the demo's server Lua under <dir>"));
        Console.WriteLine();
        Console.WriteLine("  " + Term.Muted("Drag a .dem onto gmoddem.exe, or pass the path with no command."));
        Console.WriteLine("  " + Term.Muted("gmoddem --help more") + "  lists format-research commands.");
        Console.WriteLine();
    }

    private static void PrintDiagnosticsHelp()
    {
        Console.WriteLine();
        Console.WriteLine("  " + Term.Title("gmoddem") + "  " + Term.Muted("format-research commands"));
        Console.WriteLine();
        Console.WriteLine("    messages / calibrate / serverinfo / tables / probe-tables <file.dem>");
        Console.WriteLine("    dump-signon <file.dem> <signonIndex>");
        Console.WriteLine("    find <file.dem> <signonIndex> <text>");
        Console.WriteLine("    bits <file.dem> <signonIndex> <bitOffset>");
        Console.WriteLine("    solve-tables <file.dem> <signonIndex>");
        Console.WriteLine("    search <file.dem> <text>");
        Console.WriteLine("    gmod-at <file.dem> <tick>");
        Console.WriteLine("    gmod-ids <file.dem>");
        Console.WriteLine();
        Console.WriteLine("  " + Term.Muted("These are for bitstream layout work. json / info / players are enough for normal use."));
        Console.WriteLine();
    }

    private static int Fail(string message)
    {
        Console.Error.WriteLine(Term.Fail(message));
        Console.Error.WriteLine(Term.Muted("  gmoddem --help"));
        return 1;
    }

    private static (List<byte[]> Packets, List<byte[]> Signon) LoadDemoPayloads(string path) =>
        NetMessageCalibration.LoadPayloads(path, CliReport.ReportProgress);

    private static int RunInfo(string[] args)
    {
        if (args.Length != 1)
        {
            return Fail("info requires exactly one file path.");
        }

        var summary = DemoSummary.FromFile(args[0], CliReport.ReportProgress);

        var header = summary.Header;
        Term.Heading("Header");
        Term.Kv("file", Path.GetFullPath(args[0]));
        Term.Kv("magic", $"{header.Magic} ({header.Variant})");
        Term.Kv("demo protocol", header.DemoProtocol.ToString());
        Term.Kv("network protocol", header.NetworkProtocol.ToString());
        Term.Kv("server", header.ServerName);
        Term.Kv("client", header.ClientName);
        Term.Kv("map", header.MapName);
        Term.Kv("game directory", header.GameDirectory);
        Term.Kv("playback time", $"{header.PlaybackTime:F3} s");
        Term.Kv("playback ticks", header.PlaybackTicks.ToString());
        Term.Kv("playback frames", header.PlaybackFrames.ToString());
        Term.Kv("signon length", header.SignonLength.ToString());
        Term.Kv("tick rate", header.TickRate is { } rate ? $"{rate:F2}/s" : "unknown");

        Term.Heading("Summary");
        Term.Kv("commands", summary.TotalCommands.ToString());
        Term.Kv("tick range", $"{summary.FirstTick} .. {summary.LastTick}");
        Term.Kv(
            "last content tick",
            summary.LastContentTick.ToString()
            + (summary.ContentTickCoverage is { } coverage ? $" ({coverage:P1} of declared range)" : ""));
        Term.Kv("saw dem_stop", summary.SawStopCommand.ToString());
        Term.Kv("end offset", summary.EndOffset.ToString());
        Term.Kv("file length", summary.FileLength.ToString());
        Term.Kv("trailing bytes", summary.TrailingBytes.ToString());
        Term.Kv("max packet payload", summary.MaxPacketPayloadBytes.ToString());
        Term.Kv("parsed cleanly", summary.ParsedCleanly.ToString());

        Term.Heading("Checks");
        WriteCheck("playback_frames == dem_packet count", summary.PlaybackFramesMatchPacketCount);
        WriteCheck("playback_ticks == final tick", summary.PlaybackTicksMatchLastTick);
        WriteCheck($"signon_length == signon region ({summary.SignonRegionLength})", summary.SignonLengthMatches);

        Term.Heading("Commands");
        foreach (var type in summary.CommandCounts.Keys.OrderBy(t => (byte)t))
        {
            var count = summary.CommandCounts[type];
            var bytes = summary.PayloadBytes.GetValueOrDefault(type);
            Console.WriteLine($"  {Term.Muted(type.ToString().PadRight(13))}  {count,-8}  {Term.Muted(bytes + " B")}");
        }

        if (summary.StringTablesLikelyTruncated)
        {
            Console.WriteLine();
            Console.WriteLine(Term.Warn($"  string tables are exactly {DemoSummary.StringTablesTruncationSize} bytes — treat as truncated"));
        }

        if (summary.TailRecovery is { } recovery)
        {
            Console.WriteLine();
            Console.WriteLine(Term.Warn("  recovered incomplete demo tail"));
            Console.WriteLine("  " + Term.Muted(recovery.Message));
        }

        if (summary.Errors.Count > 0)
        {
            Console.WriteLine();
            Console.WriteLine(Term.Fail($"  errors ({summary.Errors.Count})"));
            foreach (var error in summary.Errors)
            {
                Console.WriteLine($"  {Term.Muted(error.ToString())}");
            }

            return 3;
        }

        return 0;
    }

    private static void WriteCheck(string label, bool ok)
    {
        var mark = ok
            ? Term.Ok(Term.Unicode ? "✓" : "ok")
            : Term.Fail(Term.Unicode ? "✗" : "FAIL");
        Console.WriteLine($"  {mark}  {label}");
    }

    /// <summary>
    /// Recovers the svc_CreateStringTable header layout from observed message boundaries.
    /// </summary>
    /// <remarks>
    /// String tables appear back to back in the signon burst, so locating each table's name
    /// gives the exact bit length of the preceding message. That turns the header layout into a
    /// solvable constraint: the declared payload length plus the header size must equal the
    /// measured span, simultaneously for every table in the demo.
    /// </remarks>
    private static int RunSolveTables(string[] args)
    {
        if (args.Length != 2 || !int.TryParse(args[1], out var index))
        {
            return Fail("solve-tables requires a file path and a signon index.");
        }

        var (_, signon) = LoadDemoPayloads(args[0]);
        var payload = signon[index];
        // Table data contains arbitrary strings (workshop IDs, model names) that can look like a
        // tag followed by a name, so restrict to tables the engine and Garry's Mod actually create.
        HashSet<string> known =
        [
            "downloadables", "modelprecache", "genericprecache", "soundprecache", "decalprecache",
            "instancebaseline", "lightstyles", "userinfo", "server_query_info", "ParticleEffectNames",
            "EffectDispatch", "VguiScreen", "Materials", "Scenes", "ServerMapCycle", "DynamicModels",
            "GameRulesCreation", "networkstring", "networkvars", "client_lua_files",
        ];

        var anchors = FindTableAnchors(payload).Where(a => known.Contains(a.Name)).ToList();

        Console.WriteLine($"Found {anchors.Count} string tables in signon[{index}]:");
        foreach (var (name, nameEnd, msgStart) in anchors)
        {
            Console.WriteLine($"  {name,-24} msgStart={msgStart,-8} nameEnd={nameEnd}");
        }

        // Pair each table with the start of the next to get its exact extent.
        var spans = new List<(string Name, int NameEnd, int Span)>();
        for (var i = 0; i + 1 < anchors.Count; i++)
        {
            spans.Add((anchors[i].Name, anchors[i].NameEnd, anchors[i + 1].MsgStart - anchors[i].NameEnd));
        }

        Console.WriteLine();
        Console.WriteLine("Searching for a header layout consistent with every table...");

        var matches = new List<(int Prefix, int LengthBits, int Suffix, int Solved)>();
        for (var prefix = 0; prefix <= 48; prefix++)
        {
            for (var lengthBits = 8; lengthBits <= 32; lengthBits++)
            {
                for (var suffix = 0; suffix <= 24; suffix++)
                {
                    var header = prefix + lengthBits + suffix;
                    var solved = 0;

                    foreach (var (_, nameEnd, span) in spans)
                    {
                        if (span < header)
                        {
                            continue;
                        }

                        var probe = new BitReader(payload);
                        probe.Seek(nameEnd + prefix);
                        var declared = (long)probe.ReadUBitLong(lengthBits);

                        if (declared == span - header)
                        {
                            solved++;
                        }
                    }

                    // A span is wrong wherever an unknown table sits between two known ones, so
                    // accept the layout that explains the most tables rather than demanding all.
                    if (solved >= 3)
                    {
                        matches.Add((prefix, lengthBits, suffix, solved));
                    }
                }
            }
        }

        if (matches.Count == 0)
        {
            Console.WriteLine("  No fixed layout fits all tables; the header must vary per table.");
            return 1;
        }

        foreach (var (prefix, lengthBits, suffix, solved) in matches
            .OrderByDescending(m => m.Solved)
            .ThenBy(m => m.Prefix)
            .Take(15))
        {
            Console.WriteLine(
                $"  prefix={prefix,-3} lengthBits={lengthBits,-3} suffix={suffix,-3} "
                + $"header={prefix + lengthBits + suffix,-3} fits {solved}/{spans.Count}");
        }

        return 0;
    }

    /// <summary>
    /// Scans a payload for svc_CreateStringTable tags followed by a printable table name.
    /// </summary>
    private static List<(string Name, int NameEnd, int MsgStart)> FindTableAnchors(byte[] payload)
    {
        var anchors = new List<(string, int, int)>();
        var totalBits = payload.Length * 8;

        for (var bit = 0; bit + 6 + 32 < totalBits; bit++)
        {
            var probe = new BitReader(payload);
            probe.Seek(bit);
            if (probe.ReadUBitLong(NetMessageLayout.TypeBits) != (uint)NetMessageType.SvcCreateStringTable)
            {
                continue;
            }

            var name = probe.ReadString(64);
            if (name.Length is < 4 or > 40 || probe.IsOverflowed)
            {
                continue;
            }

            if (!name.All(c => char.IsAsciiLetterOrDigit(c) || c is '_' or '-'))
            {
                continue;
            }

            anchors.Add((name, probe.BitsRead, bit));
            bit += 6 + (name.Length * 8);
        }

        return anchors;
    }

    /// <summary>
    /// Reads candidate field values at a given bit offset, so a header layout can be checked
    /// against plausible values rather than assumed.
    /// </summary>
    private static int RunBits(string[] args)
    {
        if (args.Length != 3
            || !int.TryParse(args[1], out var index)
            || !int.TryParse(args[2], out var bitOffset))
        {
            return Fail("bits requires a file path, a signon index, and a bit offset.");
        }

        var (_, signon) = LoadDemoPayloads(args[0]);
        var payload = signon[index];

        var r = new BitReader(payload);
        r.Seek(bitOffset);
        Console.Write($"bit {bitOffset} onward: ");
        for (var i = 0; i < 56; i++)
        {
            if (i % 8 == 0)
            {
                Console.Write(' ');
            }

            Console.Write(r.ReadOneBit());
        }

        Console.WriteLine();

        foreach (var width in new[] { 8, 12, 16, 20, 24 })
        {
            r.Seek(bitOffset);
            Console.WriteLine($"  first {width,2} bits as unsigned : {r.ReadUBitLong(width)}");
        }

        r.Seek(bitOffset);
        var maxEntries = (int)r.ReadUBitLong(16);
        var entryBits = Log2(maxEntries) + 1;
        var numEntries = (int)r.ReadUBitLong(entryBits);
        Console.WriteLine($"  as maxEntries={maxEntries} entryBits={entryBits} numEntries={numEntries}");
        foreach (var lengthWidth in new[] { 17, 18, 19, 20, 21, 22 })
        {
            r.Seek(bitOffset + 16 + entryBits);
            Console.WriteLine($"    then {lengthWidth}-bit length = {r.ReadUBitLong(lengthWidth)}");
        }

        r.Seek(bitOffset + 16 + entryBits);
        Console.WriteLine($"    then varint length = {r.ReadVarInt32()}");

        r.Seek(bitOffset);
        var text = r.ReadBytes(Math.Min(400, r.BitsLeft / 8))
            .Select(b => b is >= 32 and < 127 ? (char)b : '.')
            .ToArray();
        Console.WriteLine();
        Console.WriteLine($"  as text: {new string(text)}");

        return 0;
    }

    /// <summary>
    /// Locates literal text inside a signon payload at every bit alignment. String tables are
    /// created in a fixed order, so finding the next table's name gives the exact extent of the
    /// previous one without having to guess its header layout.
    /// </summary>
    private static int RunFind(string[] args)
    {
        if (args.Length != 3 || !int.TryParse(args[1], out var index))
        {
            return Fail("find requires a file path, a signon index, and text to search for.");
        }

        var (_, signon) = LoadDemoPayloads(args[0]);
        if (index < 0 || index >= signon.Count)
        {
            return Fail($"Signon index out of range; the demo has {signon.Count}.");
        }

        var payload = signon[index];
        var needle = System.Text.Encoding.ASCII.GetBytes(args[2]);
        var hits = new List<int>();

        for (var shift = 0; shift < 8; shift++)
        {
            var reader = new BitReader(payload);
            reader.Seek(shift);
            var shifted = reader.ReadBytes((payload.Length * 8 - shift) / 8);

            var from = 0;
            while (true)
            {
                var at = shifted.AsSpan(from).IndexOf(needle);
                if (at < 0)
                {
                    break;
                }

                hits.Add(shift + ((from + at) * 8));
                from += at + 1;
            }
        }

        hits.Sort();
        Console.WriteLine($"signon[{index}]: {hits.Count} occurrence(s) of \"{args[2]}\"");
        foreach (var bit in hits)
        {
            // A genuine table name is preceded by the 6-bit svc_CreateStringTable tag; a match
            // inside table data will not be.
            var tag = -1;
            if (bit >= NetMessageLayout.TypeBits)
            {
                var probe = new BitReader(payload);
                probe.Seek(bit - NetMessageLayout.TypeBits);
                tag = (int)probe.ReadUBitLong(NetMessageLayout.TypeBits);
            }

            var verdict = tag == (int)NetMessageType.SvcCreateStringTable ? "TABLE" : $"tag={tag}";
            Console.WriteLine($"  bit {bit,-10} (byte {bit / 8}.{bit % 8})  {verdict}");
        }

        return 0;
    }

    /// <summary>
    /// Walks one signon packet, expanding string-table headers so a desync can be located.
    /// </summary>
    private static int RunDumpSignon(string[] args)
    {
        if (args.Length != 2 || !int.TryParse(args[1], out var index))
        {
            return Fail("dump-signon requires a file path and a signon index.");
        }

        var (_, signon) = LoadDemoPayloads(args[0]);
        if (index < 0 || index >= signon.Count)
        {
            return Fail($"Signon index out of range; the demo has {signon.Count}.");
        }

        var payload = signon[index];
        var r = new BitReader(payload);
        Console.WriteLine($"signon[{index}]: {payload.Length} bytes = {r.TotalBits} bits");

        var walker = new NetMessageWalker();
        var shown = 0;

        while (r.BitsLeft >= 8 && shown < 40)
        {
            var startBit = r.BitsRead;
            var type = (NetMessageType)r.ReadUBitLong(NetMessageLayout.TypeBits);

            if (type == NetMessageType.SvcCreateStringTable)
            {
                var isFilenames = r.PeekUBitLong(8) == ':';
                if (isFilenames)
                {
                    r.ReadByte();
                }

                var name = r.ReadString();

                if (shown < 3)
                {
                    var mark = r.BitsRead;
                    Console.WriteLine($"    raw after name '{name}' (bit {mark}):");

                    // Show the same region at each of the eight possible bit alignments: the
                    // entry data is bit-packed, so the offset where readable text appears
                    // reveals how many header bits precede it.
                    for (var shift = 0; shift < 8; shift++)
                    {
                        r.Seek(mark + shift);
                        var bytes = r.ReadBytes(24);
                        var ascii = new string(bytes
                            .Select(b => b is >= 32 and < 127 ? (char)b : '.')
                            .ToArray());
                        Console.WriteLine($"      +{shift}: {Convert.ToHexString(bytes)}  {ascii}");
                    }

                    r.Seek(mark);
                }

                var maxEntries = (int)r.ReadUBitLong(16);
                var entryBits = Log2(maxEntries) + 1;
                var numEntries = (int)r.ReadUBitLong(entryBits);
                var lengthBits = (int)r.ReadVarInt32();
                var fixedSize = r.ReadOneBit() != 0;
                var userDataSize = 0;
                var userDataSizeBits = 0;
                if (fixedSize)
                {
                    userDataSize = (int)r.ReadUBitLong(12);
                    userDataSizeBits = (int)r.ReadUBitLong(4);
                }

                var compressed = r.ReadOneBit() != 0;

                Console.WriteLine(
                    $"  bit {startBit,-9} CreateStringTable '{name}' filenames={isFilenames} "
                    + $"max={maxEntries} entries={numEntries} entryBits={entryBits} "
                    + $"lenBits={lengthBits} fixed={fixedSize}({userDataSize},{userDataSizeBits}) "
                    + $"compressed={compressed} left={r.BitsLeft}");

                if (!r.SeekRelative(lengthBits))
                {
                    Console.WriteLine("    -> length overruns the payload");
                    break;
                }
            }
            else
            {
                Console.WriteLine($"  bit {startBit,-9} {type}");
                if (!walker.TrySkip(ref r, type) || r.IsOverflowed)
                {
                    Console.WriteLine($"    -> failed to skip ({r.BitsLeft} bits left)");
                    break;
                }
            }

            shown++;
        }

        Console.WriteLine($"  ... stopped with {r.BitsLeft} bits left");
        return 0;
    }

    private static int Log2(int value)
    {
        var result = 0;
        while (value > 1)
        {
            value >>= 1;
            result++;
        }

        return result;
    }

    /// <summary>
    /// Decodes the first signon packet field by field. The string fields act as their own check:
    /// if the map and game directory come out matching the demo header, the layout is right.
    /// </summary>
    private static int RunServerInfo(string[] args)
    {
        if (args.Length != 1)
        {
            return Fail("serverinfo requires exactly one file path.");
        }

        var (_, signon) = LoadDemoPayloads(args[0]);
        if (signon.Count == 0)
        {
            return Fail("No signon packets found.");
        }

        var payload = signon[0];
        var r = new BitReader(payload);
        Console.WriteLine($"signon[0]: {payload.Length} bytes = {r.TotalBits} bits");
        Console.WriteLine();

        while (r.BitsLeft >= 8)
        {
            var type = (NetMessageType)r.ReadUBitLong(NetMessageLayout.TypeBits);
            Console.WriteLine($"--- {type} (at bit {r.BitsRead - 6})");

            if (type == NetMessageType.SvcPrint)
            {
                Console.WriteLine($"    text: {r.ReadString().TrimEnd('\n')}");
                continue;
            }

            if (type != NetMessageType.SvcServerInfo)
            {
                Console.WriteLine("    (stopping: expected ServerInfo here)");
                break;
            }

            Console.WriteLine($"    protocol      : {r.ReadUBitLong(16)}");
            Console.WriteLine($"    serverCount   : {r.ReadUBitLong(32)}");
            Console.WriteLine($"    isHLTV        : {r.ReadOneBit()}");
            Console.WriteLine($"    isDedicated   : {r.ReadOneBit()}");
            Console.WriteLine($"    clientCRC     : 0x{r.ReadUBitLong(32):X8}");
            Console.WriteLine($"    maxClasses    : {r.ReadUBitLong(16)}");
            Console.WriteLine($"    mapMD5        : {Convert.ToHexString(r.ReadBytes(16))}");
            Console.WriteLine($"    playerSlot    : {r.ReadByte()}");
            Console.WriteLine($"    maxClients    : {r.ReadByte()}");
            Console.WriteLine($"    tickInterval  : {r.ReadFloat()}");
            Console.WriteLine($"    os            : {(char)r.ReadByte()}");
            Console.WriteLine($"    gameDir       : {r.ReadString()}");
            Console.WriteLine($"    mapName       : {r.ReadString()}");
            Console.WriteLine($"    skyName       : {r.ReadString()}");
            Console.WriteLine($"    hostName      : {r.ReadString()}");
            Console.WriteLine($"    [bits left after stock layout: {r.BitsLeft}]");

            // Anything beyond this point is a Garry's Mod addition; show what follows so the
            // extra fields can be identified rather than guessed.
            Console.WriteLine($"    next string   : {r.ReadString()}");
            Console.WriteLine($"    [bits left: {r.BitsLeft}]");
            Console.WriteLine($"    next string   : {r.ReadString()}");
            Console.WriteLine($"    [bits left: {r.BitsLeft}]");

            var tail = r.BitsLeft;
            Console.Write("    trailing bits : ");
            for (var i = 0; i < tail; i++)
            {
                Console.Write(r.ReadOneBit());
            }

            Console.WriteLine();
            break;
        }

        return 0;
    }

    /// <summary>
    /// Locates ASCII text in console commands and in packet/usercmd/datatable payloads.
    /// Chat and Lua net strings are often bit-packed, so payloads are scanned at all eight
    /// bit alignments rather than only on byte boundaries.
    /// </summary>
    private static int RunSearch(string[] args)
    {
        if (args.Length != 2)
        {
            return Fail("search requires a file path and the text to look for.");
        }

        var needle = System.Text.Encoding.UTF8.GetBytes(args[1]);
        if (needle.Length == 0)
        {
            return Fail("Search text must not be empty.");
        }

        using var reader = DemoReader.Open(args[0]);
        var walker = new NetMessageWalker();
        var hits = 0;

        Console.WriteLine($"Searching for \"{args[1]}\" ({needle.Length} bytes, all bit alignments)");
        Console.WriteLine();

        foreach (var command in reader.ReadCommands())
        {
            CliReport.Current?.Report(reader.Position);
            switch (command)
            {
                case ConsoleCommand console when console.Command.Contains(args[1], StringComparison.Ordinal):
                    hits++;
                    Console.WriteLine($"console  tick={console.Tick,-8} offset={console.Offset}  {console.Command}");
                    break;

                case PacketCommand packet:
                    foreach (var (bit, _) in FindBitAligned(packet.Payload, needle))
                    {
                        hits++;
                        var walk = walker.Walk(packet.Payload);
                        var containing = walk.Messages.FirstOrDefault(m =>
                            bit >= m.StartBit && bit < m.StartBit + m.LengthBits);

                        var messageLabel = containing.LengthBits > 0
                            ? $"{containing.Type} @ bit {containing.StartBit}+{containing.LengthBits}"
                            : $"unmapped (walk {walk.Status})";

                        var context = ExtractContext(packet.Payload, bit, needle.Length);
                        Console.WriteLine(
                            $"{packet.Type,-8} tick={packet.Tick,-8} fileOffset={packet.Offset}  " +
                            $"payloadBit={bit}  {messageLabel}");
                        Console.WriteLine($"           ctx: {context}");
                    }

                    break;

                case UserCommand user:
                    foreach (var (bit, _) in FindBitAligned(user.Payload, needle))
                    {
                        hits++;
                        Console.WriteLine($"usercmd   tick={user.Tick,-8} offset={user.Offset}  payloadBit={bit}");
                    }

                    break;

                case DataTablesCommand dataTables:
                    foreach (var (bit, _) in FindBitAligned(dataTables.Payload, needle))
                    {
                        hits++;
                        Console.WriteLine($"datatables tick={dataTables.Tick} offset={dataTables.Offset} payloadBit={bit}");
                    }

                    break;

                case StringTablesCommand stringTables:
                    foreach (var (bit, _) in FindBitAligned(stringTables.Payload, needle))
                    {
                        hits++;
                        var context = ExtractContext(stringTables.Payload, bit, needle.Length);
                        Console.WriteLine($"stringtables tick={stringTables.Tick} offset={stringTables.Offset} payloadBit={bit}");
                        Console.WriteLine($"           ctx: {context}");
                    }

                    break;
            }
        }

        Console.WriteLine();
        Console.WriteLine(hits == 0 ? "No matches." : $"{hits} match(es).");
        return hits == 0 ? 1 : 0;
    }

    /// <summary>
    /// Dumps every Garry's Mod svc_GMod_ServerToClient message in packets of a given tick,
    /// field by field, so a known string (chat, etc.) can be placed in the payload.
    /// </summary>
    private static int RunGmodAt(string[] args)
    {
        if (args.Length != 2 || !int.TryParse(args[1], out var targetTick))
        {
            return Fail("gmod-at requires a file path and a tick number.");
        }

        using var reader = DemoReader.Open(args[0]);
        var found = false;

        foreach (var command in reader.ReadCommands())
        {
            CliReport.Current?.Report(reader.Position);
            if (command is not PacketCommand packet || packet.Tick != targetTick)
            {
                continue;
            }

            found = true;
            Console.WriteLine($"{packet.Type} tick={packet.Tick} offset={packet.Offset} payload={packet.Payload.Length} bytes");

            var walker = new NetMessageWalker();
            var walk = walker.Walk(packet.Payload);
            Console.WriteLine($"  walk: {walk.Status}, {walk.Messages.Count} messages, {walk.BitsRemaining} bits left");

            foreach (var span in walk.Messages.Where(m => m.Type == NetMessageType.SvcGModServerToClient))
            {
                var r = new BitReader(packet.Payload);
                r.Seek(span.StartBit);
                var type = (NetMessageType)r.ReadUBitLong(NetMessageLayout.TypeBits);
                var lengthBits = (int)r.ReadUBitLong(NetMessageLayout.GModMessageLengthBits);
                var subType = r.ReadByte();
                var bodyBits = lengthBits - 8;

                Console.WriteLine(
                    $"  {type} startBit={span.StartBit} declaredLength={lengthBits} bits  subType={subType}");

                if (subType == 0)
                {
                    var networkId = r.ReadWord();
                    bodyBits -= 16;
                    Console.WriteLine($"    lua net name id = {networkId}");
                }

                var bodyStart = r.BitsRead;
                Console.WriteLine($"    body starts at bit {bodyStart}, {bodyBits} bits remaining in declaration");

                var printable = new System.Text.StringBuilder();
                var remaining = Math.Min(bodyBits, r.BitsLeft);
                var saved = r.BitsRead;
                while (r.BitsRead - saved + 8 <= remaining)
                {
                    var b = r.ReadByte();
                    printable.Append(b is >= 32 and < 127 ? (char)b : '.');
                }

                Console.WriteLine($"    body as 8-bit chars from body start: {printable}");

                r.Seek(bodyStart);
                Console.Write("    first 80 body bits: ");
                for (var i = 0; i < Math.Min(80, remaining); i++)
                {
                    if (i > 0 && i % 8 == 0)
                    {
                        Console.Write(' ');
                    }

                    Console.Write(r.ReadOneBit());
                }

                Console.WriteLine();
            }
        }

        if (!found)
        {
            Console.WriteLine($"No packet at tick {targetTick}.");
            return 1;
        }

        return 0;
    }

    /// <summary>
    /// Histograms Lua net-message name IDs (svc_GMod_ServerToClient subtype 0) so a known
    /// payload can be tied to a stable ID even before the networkstring table is decoded.
    /// </summary>
    private static int RunGmodIds(string[] args)
    {
        if (args.Length != 1)
        {
            return Fail("gmod-ids requires a file path.");
        }

        using var reader = DemoReader.Open(args[0]);
        var walker = new NetMessageWalker();
        var counts = new Dictionary<int, int>();
        var subtypes = new Dictionary<int, int>();
        var sampleTicks = new Dictionary<int, List<int>>();

        foreach (var command in reader.ReadCommands())
        {
            CliReport.Current?.Report(reader.Position);
            if (command is not PacketCommand packet)
            {
                continue;
            }

            var walk = walker.Walk(packet.Payload);
            foreach (var span in walk.Messages.Where(m => m.Type == NetMessageType.SvcGModServerToClient))
            {
                var r = new BitReader(packet.Payload);
                r.Seek(span.StartBit);
                r.ReadUBitLong(NetMessageLayout.TypeBits);
                var lengthBits = (int)r.ReadUBitLong(NetMessageLayout.GModMessageLengthBits);
                var subType = r.ReadByte();
                subtypes[subType] = subtypes.GetValueOrDefault(subType) + 1;

                if (subType != 0)
                {
                    continue;
                }

                var networkId = r.ReadWord();
                counts[networkId] = counts.GetValueOrDefault(networkId) + 1;
                if (!sampleTicks.TryGetValue(networkId, out var ticks))
                {
                    ticks = [];
                    sampleTicks[networkId] = ticks;
                }

                if (ticks.Count < 3)
                {
                    ticks.Add(packet.Tick);
                }
            }
        }

        Console.WriteLine("GMod submessage types:");
        foreach (var (sub, count) in subtypes.OrderByDescending(kv => kv.Value))
        {
            Console.WriteLine($"  subtype {sub,-4} count={count}");
        }

        Console.WriteLine();
        Console.WriteLine($"{"id",6} {"count",8}  sample ticks");
        foreach (var (id, count) in counts.OrderByDescending(kv => kv.Value))
        {
            Console.WriteLine($"{id,6} {count,8}  {string.Join(", ", sampleTicks[id])}");
        }

        Console.WriteLine();
        Console.WriteLine($"{counts.Count} distinct Lua net IDs, {counts.Values.Sum()} subtype-0 messages");
        return 0;
    }

    private static int RunTables(string[] args)
    {
        if (args.Length != 1)
        {
            return Fail("tables requires a file path.");
        }

        var directory = PlayerDirectory.FromDemo(args[0], CliReport.ReportProgress);
        Console.WriteLine($"{"id",4} {"name",-22} {"max",6} {"decl",6} {"got",6} varint compressed");
        for (var i = 0; i < directory.Tables.Count; i++)
        {
            var table = directory.Tables[i];
            Console.WriteLine(
                $"{i,4} {table.Name,-22} {table.MaxEntries,6} {table.DeclaredEntries,6} {table.Entries.Count,6} " +
                $"{table.LengthIsVarInt,-6} {table.Compressed}");
        }

        return directory.Tables.Count == 0 ? 1 : 0;
    }

    /// <summary>
    /// Tries CreateStringTable length encodings against the first signon packet that contains
    /// one, and reports what the next message type would be after skipping the payload.
    /// </summary>
    private static int RunProbeTables(string[] args)
    {
        if (args.Length != 1)
        {
            return Fail("probe-tables requires a file path.");
        }

        var (_, signon) = LoadDemoPayloads(args[0]);
        if (signon.Count < 2)
        {
            return Fail("Expected a signon packet containing string tables.");
        }

        var payload = signon[1];
        var start = FindFirstCreate(payload);
        if (start < 0)
        {
            return Fail("No svc_CreateStringTable in signon[1].");
        }

        Console.WriteLine($"First CreateStringTable at bit {start}");
        DumpCreateHeader(payload, start);
        Console.WriteLine();
        Console.WriteLine("Known table names in this packet:");
        var tables = DumpKnownTableHeaders(payload);
        Console.WriteLine();
        Console.WriteLine("Power-of-two maxEntries if extra bits sit between name and max:");
        ProbeMaxShift(payload, tables);
        Console.WriteLine();
        Console.WriteLine("userinfo slice before the next table:");
        DumpUserInfoSlice(payload, tables);
        Console.WriteLine();
        Console.WriteLine("player_info_t candidates around STEAM_ (name at -36 bytes):");
        DumpPlayerInfoCandidates(payload);
        return 0;
    }

    private static int FindFirstCreate(byte[] payload)
    {
        var r = new BitReader(payload);
        var walker = new NetMessageWalker();
        while (r.BitsLeft >= 8)
        {
            var start = r.BitsRead;
            var type = (NetMessageType)r.ReadUBitLong(NetMessageLayout.TypeBits);
            if (type == NetMessageType.SvcCreateStringTable)
            {
                return start;
            }

            if (!walker.TrySkip(ref r, type) || r.IsOverflowed)
            {
                break;
            }
        }

        return -1;
    }

    private static void DumpCreateHeader(byte[] payload, int startBit)
    {
        var r = new BitReader(payload);
        r.Seek(startBit);
        r.ReadUBitLong(NetMessageLayout.TypeBits);
        if (r.PeekUBitLong(8) == ':')
        {
            r.ReadByte();
            Console.WriteLine("  filenames colon prefix");
        }

        var name = r.ReadString();
        var maxEntries = (int)r.ReadUBitLong(16);
        var numEntries = (int)r.ReadUBitLong(BitMath.Log2(maxEntries) + 1);
        var afterCounts = r.BitsRead;
        Console.WriteLine($"  name='{name}' max={maxEntries} declared={numEntries} afterCounts={afterCounts}");

        var peek = new BitReader(payload);
        peek.Seek(afterCounts);
        var bits = new char[48];
        for (var i = 0; i < bits.Length; i++)
        {
            bits[i] = peek.ReadOneBit() != 0 ? '1' : '0';
        }

        Console.WriteLine($"  next 48 bits: {new string(bits)}");

        peek.Seek(afterCounts);
        Console.WriteLine($"  as 20-bit length : {peek.ReadUBitLong(20)}");
        peek.Seek(afterCounts);
        Console.WriteLine($"  as varint length : {peek.ReadVarInt32()}");
        peek.Seek(afterCounts);
        Console.WriteLine($"  as ubitvar length: {peek.ReadUBitVar()}");
    }

    private static readonly string[] KnownTableNames =
    [
        "downloadables", "modelprecache", "genericprecache", "soundprecache", "decalprecache",
        "instancebaseline", "lightstyles", "userinfo", "server_query_info", "ParticleEffectNames",
        "EffectDispatch", "VguiScreen", "ExtraUserData", "InfoPanel", "Scenes", "Materials",
        "DynamicModels", "ServerMapCycle", "ServerMapCycleEx", "Soundscape", "LuaNetworkedVars",
        "GameRulesCreation",
    ];

    private static List<(string Name, int TypeBit, int NameBit)> DumpKnownTableHeaders(byte[] payload)
    {
        var found = new List<(string Name, int TypeBit, int NameBit)>();
        foreach (var tableName in KnownTableNames)
        {
            var needle = System.Text.Encoding.ASCII.GetBytes(tableName);
            foreach (var (bit, _) in FindBitAligned(payload, needle).Take(3))
            {
                var typeBit = bit - NetMessageLayout.TypeBits;
                if (typeBit < 0)
                {
                    continue;
                }

                var r = new BitReader(payload);
                r.Seek(typeBit);
                var type = r.ReadUBitLong(6);
                if (type != (uint)NetMessageType.SvcCreateStringTable)
                {
                    continue;
                }

                if (r.PeekUBitLong(8) == ':')
                {
                    r.ReadByte();
                }

                var name = r.ReadString(64);
                if (name != tableName)
                {
                    continue;
                }

                var max = r.ReadUBitLong(16);
                var num = max > 0 ? r.ReadUBitLong(BitMath.Log2((int)max) + 1) : 0;
                Console.WriteLine(
                    $"  '{tableName}' ascii@{bit} type@{typeBit} max={max} declared={num}");
                found.Add((tableName, typeBit, bit));
            }
        }

        return found.OrderBy(t => t.TypeBit).ToList();
    }

    private static void ProbeMaxShift(byte[] payload, List<(string Name, int TypeBit, int NameBit)> tables)
    {
        foreach (var table in tables)
        {
            var r = new BitReader(payload);
            r.Seek(table.NameBit);
            r.ReadString();
            var afterName = r.BitsRead;
            var hits = new List<string>();
            for (var extra = 0; extra <= 24; extra++)
            {
                var probe = new BitReader(payload);
                probe.Seek(afterName + extra);
                var max = (int)probe.ReadUBitLong(16);
                if (max is 32 or 64 or 128 or 256 or 512 or 1024 or 2048 or 4096 or 8192 or 16384)
                {
                    var declared = (int)probe.ReadUBitLong(BitMath.Log2(max) + 1);
                    hits.Add($"extra={extra} max={max} declared={declared}");
                }
            }

            Console.WriteLine($"  {table.Name,-22} {(hits.Count == 0 ? "none" : string.Join("; ", hits))}");
        }
    }

    private static void DumpUserInfoSlice(byte[] payload, List<(string Name, int TypeBit, int NameBit)> tables)
    {
        var user = tables.FirstOrDefault(t => t.Name == "userinfo");
        if (user.Name is null)
        {
            Console.WriteLine("  userinfo not found");
            return;
        }

        var next = tables.FirstOrDefault(t => t.TypeBit > user.TypeBit);
        var end = next.Name is null ? payload.Length * 8 : next.TypeBit;
        Console.WriteLine($"  range {user.TypeBit} .. {end} ({end - user.TypeBit} bits) next={next.Name}");

        var r = new BitReader(payload);
        r.Seek(user.NameBit);
        r.ReadString();
        var afterName = r.BitsRead;
        var sliceBits = end - afterName;
        var slice = r.ReadBitsAsBytes(sliceBits);
        Console.WriteLine($"  after-name hex ({slice.Length} bytes): {Convert.ToHexString(slice[..Math.Min(64, slice.Length)])}");

        foreach (var extra in new[] { 0, 1, 2, 8, 16 })
        foreach (var maxGuess in new[] { 64, 128, 256, 4104 })
        {
            TryParseUserInfo(slice, extra, maxGuess, declaredHint: 96);
        }

        var lzOffset = IndexOf(slice, "LZSS"u8.ToArray());
        if (lzOffset >= 0)
        {
            var decompressed = Lzss.Decompress(slice.AsSpan(lzOffset));
            Console.WriteLine($"  LZSS at byte {lzOffset} of after-name slice: {(decompressed is null ? "fail" : decompressed.Length + " bytes")}");
            if (decompressed is not null)
            {
                var ascii = new string(decompressed.Select(b => b is >= 32 and < 127 ? (char)b : '.').ToArray());
                Console.WriteLine($"  decompressed ascii: {ascii[..Math.Min(400, ascii.Length)]}");
                DumpSteamFromBytes("decompressed", decompressed);
                TryParseEntriesFromBytes(decompressed, 128, 96);
                TryParseEntriesFromBytes(decompressed, 256, 96);
                TryParseEntriesFromBytes(decompressed, 4104, 96);
            }
        }
    }

    private static void TryParseUserInfo(byte[] afterName, int extraBits, int maxEntries, int declaredHint)
    {
        var r = new BitReader(afterName);
        r.SeekRelative(extraBits);
        var max = (int)r.ReadUBitLong(16);
        var declared = (int)r.ReadUBitLong(BitMath.Log2(maxEntries) + 1);
        _ = max;
        _ = declared;
        _ = declaredHint;
    }

    private static void TryParseEntriesFromBytes(byte[] data, int maxEntries, int declared)
    {
        foreach (var fixedSize in new[] { false, true })
        {
            var table = new StringTable
            {
                Name = "userinfo",
                MaxEntries = maxEntries,
                DeclaredEntries = declared,
                LengthIsVarInt = false,
                UserDataFixedSize = fixedSize,
                UserDataSize = fixedSize ? 256 : 0,
                UserDataSizeBits = fixedSize ? 8 * 256 : 0,
                Compressed = false,
            };
            var reader = new BitReader(data);
            var entries = StringTableParser.ReadEntries(ref reader, table, declared);
            if (entries is null)
            {
                continue;
            }

            var withData = entries.Count(e => e.UserData is { Length: > 0 } || !string.IsNullOrEmpty(e.Text));
            Console.WriteLine(
                $"  entries max={maxEntries} fixed={fixedSize} parsed={entries.Count} nonempty={withData} leftover={reader.BitsLeft}");
            foreach (var entry in entries.Take(8))
            {
                var text = entry.Text ?? "";
                var ud = entry.UserData ?? [];
                var ascii = new string(ud.Select(b => b is >= 32 and < 127 ? (char)b : '.').ToArray());
                Console.WriteLine($"    #{entry.Index} text='{text}' ud={ud.Length} '{ascii}'");
            }

            var last = entries.LastOrDefault();
            if (last is not null && entries.Count > 8)
            {
                var ud = last.UserData ?? [];
                var ascii = new string(ud.Select(b => b is >= 32 and < 127 ? (char)b : '.').ToArray());
                Console.WriteLine($"    ... #{last.Index} text='{last.Text}' ud={ud.Length} '{ascii}'");
            }
        }
    }

    private static void DumpPlayerInfoCandidates(byte[] payload)
    {
        var hits = FindBitAligned(payload, "STEAM_"u8.ToArray()).ToList();
        Console.WriteLine($"  {hits.Count} STEAM_ hits");
        var parsed = 0;
        var seen = new HashSet<string>();
        foreach (var (bit, _) in hits)
        {
            if (bit < 36 * 8)
            {
                continue;
            }

            var r = new BitReader(payload);
            r.Seek(bit - 36 * 8);
            var blob = r.ReadBytes(120);
            var player = PlayerDirectory.ParseUserInfo(new StringTableEntry(
                Index: parsed,
                Text: null,
                UserData: blob));
            if (player?.SteamId is null)
            {
                continue;
            }

            if (!seen.Add(player.SteamId))
            {
                continue;
            }

            parsed++;
            if (parsed <= 8)
            {
                Console.WriteLine(
                    $"  bit={bit} slotGuess={player.Slot} userid={player.UserId} " +
                    $"name='{player.Name}' {player.SteamId} {player.SteamId64}");
            }
        }

        Console.WriteLine($"  unique SteamIDs with player_info_t@-36: {seen.Count}");
        if (hits.Count > 0)
        {
            var nameBit = Math.Max(0, hits[0].Bit - 36 * 8);
            MeasureNameToGuid(payload, nameBit, "first STEAM_");
            DumpAsciiWindow(payload, hits[0].Bit, 240, "first STEAM_");
        }
    }

    private static void MeasureNameToGuid(byte[] payload, int nameBit, string name)
    {
        var r = new BitReader(payload);
        r.Seek(nameBit);
        var window = r.ReadBytes(200);
        var guidAt = IndexOf(window, "STEAM_"u8.ToArray());
        Console.WriteLine($"  {name} @bit {nameBit}: STEAM_ is {guidAt} bytes after name start");
        if (guidAt >= 4)
        {
            var userId = System.Buffers.Binary.BinaryPrimitives.ReadInt32LittleEndian(window.AsSpan(guidAt - 4, 4));
            Console.WriteLine($"    userID@{guidAt - 4}={userId}");
            if (nameBit >= 32)
            {
                var bits = new BitReader(payload);
                bits.Seek(nameBit - 32);
                var buf = new char[32];
                for (var i = 0; i < 32; i++)
                {
                    buf[i] = bits.ReadOneBit() != 0 ? '1' : '0';
                }

                Console.WriteLine($"    32 bits before name: {new string(buf)}");
                bits.Seek(nameBit - 14);
                Console.WriteLine($"    14-bit field immediately before name: {bits.ReadUBitLong(14)}");
            }
        }
    }

    private static void DumpAsciiWindow(byte[] payload, int centerBit, int bytesEachSide, string label)
    {
        var start = Math.Max(0, centerBit - bytesEachSide * 8);
        var r = new BitReader(payload);
        r.Seek(start);
        var data = r.ReadBytes(bytesEachSide * 2);
        var ascii = new string(data.Select(b => b is >= 32 and < 127 ? (char)b : '.').ToArray());
        Console.WriteLine($"  {label} bit {centerBit}:");
        Console.WriteLine($"    {ascii}");
    }

    private static void DumpSteamFromBytes(string label, byte[] data)
    {
        var text = System.Text.Encoding.ASCII.GetString(data);
        var matches = System.Text.RegularExpressions.Regex.Matches(text, @"STEAM_[0-5]:[0-1]:\d+");
        Console.WriteLine($"  {label} STEAM_={matches.Count}");
        foreach (System.Text.RegularExpressions.Match match in matches.Take(5))
        {
            Console.WriteLine($"    {match.Value}");
        }
    }

    private static int IndexOf(byte[] haystack, byte[] needle)
    {
        var span = haystack.AsSpan();
        for (var i = 0; i <= haystack.Length - needle.Length; i++)
        {
            if (span[i..].StartsWith(needle))
            {
                return i;
            }
        }

        return -1;
    }

    private static void TryFraming(byte[] payload, int startBit, int targetEnd)
    {
        var rows = new List<(int Dist, string Line)>();
        foreach (var lengthKind in new[] { "16", "17", "18", "19", "20", "21", "22", "24", "32", "varint", "ubitvar" })
        foreach (var flagsBeforeLength in new[] { false, true })
        foreach (var extraBits in new[] { 0, 1, 2, 3, 8 })
        {
            if (!TryReadCreateFrame(
                    payload,
                    startBit,
                    lengthKind,
                    flagsBeforeLength,
                    extraBits,
                    out _,
                    out var lengthBits,
                    out var endBit,
                    out var nextType,
                    out var nextName))
            {
                continue;
            }

            var dist = Math.Abs(endBit - targetEnd);
            var namePreview = nextName.Length > 24 ? nextName[..24] : nextName;
            rows.Add((dist,
                $"  dist={dist,-8} {lengthKind,-8} flagsBefore={flagsBeforeLength,-5} extra={extraBits} " +
                $"len={lengthBits,-8} end={endBit} next={nextType} '{namePreview}'"));
        }

        foreach (var row in rows.OrderBy(r => r.Dist).Take(12))
        {
            Console.WriteLine(row.Line);
        }
    }

    private static void DumpLzssBlobs(byte[] payload)
    {
        foreach (var (bit, _) in FindBitAligned(payload, "LZSS"u8.ToArray()))
        {
            var lz = new BitReader(payload);
            lz.Seek(bit);
            var blob = lz.ReadBytes(Math.Min(lz.BitsLeft / 8, 512 * 1024));
            var decompressed = Lzss.Decompress(blob);
            if (decompressed is null)
            {
                Console.WriteLine($"  @{bit} decomp-fail");
                continue;
            }

            var text = System.Text.Encoding.ASCII.GetString(decompressed);
            var steam = System.Text.RegularExpressions.Regex.Matches(text, @"STEAM_[0-5]:[0-1]:\d+");
            var markerHits = new List<string>();
            foreach (var marker in new[] { "\\name\\", "\\guid\\" })
            {
                if (text.Contains(marker, StringComparison.Ordinal))
                {
                    markerHits.Add(marker);
                }
            }

            Console.WriteLine(
                $"  @{bit} uncompressed={decompressed.Length,-7} STEAM_={steam.Count} markers={string.Join(",", markerHits)}");
            foreach (System.Text.RegularExpressions.Match match in steam.Take(2))
            {
                Console.WriteLine($"      {match.Value}");
            }
        }
    }

    private static bool TryReadCreateFrame(
        byte[] payload,
        int startBit,
        string lengthKind,
        bool flagsBeforeLength,
        int extraBits,
        out string name,
        out int lengthBits,
        out int endBit,
        out NetMessageType nextType,
        out string nextName)
    {
        name = "";
        lengthBits = 0;
        endBit = 0;
        nextType = NetMessageType.NetNop;
        nextName = "";

        var r = new BitReader(payload);
        r.Seek(startBit);
        if (r.ReadUBitLong(NetMessageLayout.TypeBits) != (uint)NetMessageType.SvcCreateStringTable)
        {
            return false;
        }

        if (r.PeekUBitLong(8) == ':')
        {
            r.ReadByte();
        }

        name = r.ReadString();
        var maxEntries = (int)r.ReadUBitLong(16);
        if (maxEntries <= 0 || r.IsOverflowed)
        {
            return false;
        }

        r.ReadUBitLong(BitMath.Log2(maxEntries) + 1);

        if (flagsBeforeLength && !ReadCreateFlags(ref r))
        {
            return false;
        }

        lengthBits = lengthKind switch
        {
            "varint" => (int)r.ReadVarInt32(),
            "ubitvar" => (int)r.ReadUBitVar(),
            _ => (int)r.ReadUBitLong(int.Parse(lengthKind)),
        };

        if (!flagsBeforeLength && !ReadCreateFlags(ref r))
        {
            return false;
        }

        for (var i = 0; i < extraBits; i++)
        {
            r.ReadOneBit();
        }

        if (r.IsOverflowed || lengthBits < 0 || lengthBits > r.BitsLeft)
        {
            return false;
        }

        r.SeekRelative(lengthBits);
        endBit = r.BitsRead;
        if (r.BitsLeft < 8)
        {
            return false;
        }

        nextType = (NetMessageType)r.ReadUBitLong(NetMessageLayout.TypeBits);
        if (nextType != NetMessageType.SvcCreateStringTable)
        {
            return true;
        }

        if (r.PeekUBitLong(8) == ':')
        {
            r.ReadByte();
        }

        nextName = r.ReadString(64);
        return !r.IsOverflowed;
    }

    private static bool ReadCreateFlags(ref BitReader r)
    {
        if (r.ReadOneBit() != 0)
        {
            r.ReadUBitLong(12);
            r.ReadUBitLong(4);
        }

        r.ReadOneBit();
        return !r.IsOverflowed;
    }

    private static bool LooksLikeTableName(string name) =>
        name.Length is >= 3 and <= 64 && name.All(c => char.IsAsciiLetterOrDigit(c) || c is '_' or '-');

    private static void DumpUserInfoNeighbourhood(byte[] payload)
    {
        var needle = "userinfo"u8.ToArray();
        foreach (var (bit, _) in FindBitAligned(payload, needle).Take(4))
        {
            var r = new BitReader(payload);
            r.Seek(bit);
            var raw = r.ReadBytes(48);
            var hex = Convert.ToHexString(raw);
            var ascii = new string(raw.Select(b => b is >= 32 and < 127 ? (char)b : '.').ToArray());
            Console.WriteLine($"  at bit {bit}");
            Console.WriteLine($"    hex   {hex}");
            Console.WriteLine($"    ascii {ascii}");

            var typeBit = bit - NetMessageLayout.TypeBits;
            if (typeBit >= 0)
            {
                var header = new BitReader(payload);
                header.Seek(typeBit);
                var type = header.ReadUBitLong(6);
                var name = header.ReadString(32);
                var max = header.ReadUBitLong(16);
                Console.WriteLine($"    if type@-{NetMessageLayout.TypeBits}: type={type} name='{name}' max={max}");
            }

            foreach (var lzssBit in new[] { bit + 168, 228299 })
            {
                if (lzssBit < 0 || lzssBit >= payload.Length * 8)
                {
                    continue;
                }

                var lz = new BitReader(payload);
                lz.Seek(lzssBit);
                if (lz.BitsLeft < 8 * 8)
                {
                    continue;
                }

                var magic = lz.ReadBytes(4);
                if (magic[0] != (byte)'L' || magic[1] != (byte)'Z')
                {
                    continue;
                }

                lz.Seek(lzssBit);
                var blob = lz.ReadBytes(Math.Min(lz.BitsLeft / 8, 256 * 1024));
                var decompressed = Lzss.Decompress(blob);
                Console.WriteLine(
                    $"    LZSS@{lzssBit}: {(decompressed is null ? "decomp-fail" : $"{decompressed.Length} bytes")}");
                if (decompressed is not null)
                {
                    var text = System.Text.Encoding.ASCII.GetString(decompressed);
                    var steam = System.Text.RegularExpressions.Regex.Matches(text, @"STEAM_[0-5]:[0-1]:\d+");
                    Console.WriteLine($"      STEAM_ strings: {steam.Count}");
                    foreach (System.Text.RegularExpressions.Match match in steam.Cast<System.Text.RegularExpressions.Match>().Take(3))
                    {
                        Console.WriteLine($"        {match.Value}");
                    }
                }

                break;
            }
        }
    }

    private static int RunPlayers(string[] args)
    {
        if (args.Length != 1)
        {
            return Fail("players requires a file path.");
        }

        var players = PlayerDirectory.FromDemo(args[0], CliReport.ReportProgress).Snapshot();
        Term.Heading("Players");
        Console.WriteLine("  " + Term.Muted($"{"id",4} {"slot",6} {"ent",6} {"userid",8} {"steamid64",20}  name"));
        using var reader = DemoReader.Open(args[0]);
        var recorderName = reader.Header.ClientName;
        foreach (var player in players)
        {
            var line =
                $"{player.Id,4} {(player.Slot >= 0 ? player.Slot.ToString() : "-"),6} " +
                $"{(player.Slot >= 0 ? player.Entity.ToString() : "-"),6} {player.UserId?.ToString() ?? "-",8} " +
                $"{player.SteamId64 ?? "-",20}  {player.Name ?? ""}";
            var isRecorder = recorderName.Length > 0
                && string.Equals(player.Name, recorderName, StringComparison.Ordinal);
            Console.WriteLine(isRecorder ? Term.Accent("  " + line) : "  " + line);
        }

        Console.WriteLine();
        Term.Kv("identified", $"{players.Count} in userinfo");
        var recorder = players.FirstOrDefault(p =>
            recorderName.Length > 0
            && string.Equals(p.Name, recorderName, StringComparison.Ordinal));
        if (recorder is not null)
        {
            Term.Kv(
                "recorder",
                $"{recorderName}  steamid={recorder.SteamId} steamid64={recorder.SteamId64}"
                + (recorder.Slot >= 0 ? $"  slot={recorder.Slot} entity={recorder.Entity}" : ""));
        }

        return players.Count == 0 ? 1 : 0;
    }

    private static IEnumerable<(int Bit, int Shift)> FindBitAligned(byte[] payload, byte[] needle)
    {
        var totalBits = payload.Length * 8;
        if (totalBits < needle.Length * 8)
        {
            yield break;
        }

        for (var shift = 0; shift < 8; shift++)
        {
            var availableBytes = (totalBits - shift) / 8;
            if (availableBytes < needle.Length)
            {
                continue;
            }

            var reader = new BitReader(payload);
            reader.Seek(shift);
            var shifted = reader.ReadBytes(availableBytes);

            var from = 0;
            while (from <= shifted.Length - needle.Length)
            {
                var at = shifted.AsSpan(from).IndexOf(needle);
                if (at < 0)
                {
                    break;
                }

                yield return (shift + ((from + at) * 8), shift);
                from += at + 1;
            }
        }
    }

    private static string ExtractContext(byte[] payload, int bit, int needleBytes)
    {
        var startBit = Math.Max(0, bit - 24 * 8);
        var endBit = Math.Min(payload.Length * 8, bit + (needleBytes + 24) * 8);
        var reader = new BitReader(payload);
        reader.Seek(startBit);
        var bytes = reader.ReadBytes((endBit - startBit) / 8);
        var chars = bytes.Select(b => b is >= 32 and < 127 ? (char)b : '.').ToArray();
        return new string(chars);
    }

    /// <summary>
    /// Inventories the network messages present in a demo without interpreting their contents.
    /// </summary>
    private static int RunMessages(string[] args)
    {
        if (args.Length != 1)
        {
            return Fail("messages requires exactly one file path.");
        }

        var (packets, signon) = LoadDemoPayloads(args[0]);
        var walker = new NetMessageWalker();

        var counts = new Dictionary<NetMessageType, int>();
        var bits = new Dictionary<NetMessageType, long>();
        var failed = 0;

        foreach (var payload in packets)
        {
            var result = walker.Walk(payload);
            if (result.Status != PacketWalkStatus.Clean)
            {
                failed++;
            }

            foreach (var message in result.Messages)
            {
                counts[message.Type] = counts.GetValueOrDefault(message.Type) + 1;
                bits[message.Type] = bits.GetValueOrDefault(message.Type) + message.LengthBits;
            }
        }

        Console.WriteLine($"Gameplay packets   : {packets.Count} ({failed} failed to parse)");
        Console.WriteLine();
        Console.WriteLine($"{"message",-28} {"count",10} {"payload KiB",14}");
        foreach (var (type, count) in counts.OrderByDescending(kv => kv.Value))
        {
            Console.WriteLine($"{type,-28} {count,10} {bits[type] / 8192.0,14:F1}");
        }

        Console.WriteLine();
        Console.WriteLine("Signon packets (message sequence until first failure):");
        for (var i = 0; i < signon.Count; i++)
        {
            var result = walker.Walk(signon[i]);
            var summary = string.Join(", ", result.Messages
                .GroupBy(m => m.Type)
                .Select(g => g.Count() > 1 ? $"{g.Key}x{g.Count()}" : g.Key.ToString()));

            Console.WriteLine($"  [{i}] {signon[i].Length,8} bytes  {result.Status}"
                + (result.FailedAt is { } f ? $" at {f}" : string.Empty));
            Console.WriteLine($"      {summary}");
        }

        return 0;
    }

    /// <summary>
    /// Scores competing bit-width candidates against a real demo. Used to settle field widths
    /// that community sources report inconsistently.
    /// </summary>
    private static int RunCalibrate(string[] args)
    {
        if (args.Length != 1)
        {
            return Fail("calibrate requires exactly one file path.");
        }

        Console.WriteLine("Loading packet payloads...");
        var (packets, signon) = LoadDemoPayloads(args[0]);
        Console.WriteLine($"  {packets.Count} gameplay packets, {signon.Count} signon packets");
        Console.WriteLine();

        var scores = NetMessageCalibration.Run(packets, signon)
            .OrderByDescending(s => s.PacketCleanRate)
            .ThenByDescending(s => s.SignonCleanRate)
            .ToList();

        Console.WriteLine($"{"candidate",-52} {"packets",10} {"signon",11}");
        foreach (var score in scores)
        {
            Console.WriteLine(
                $"{score.Candidate,-52} {score.PacketCleanRate,10:P2} {score.SignonCleanRate,11:P2}");
        }

        Console.WriteLine();
        Console.WriteLine("svc_CreateStringTable payload length encoding (scored on signon):");
        Console.WriteLine($"  {"encoding",-12} {"signon clean",14} {"tables found",14}");
        foreach (var (width, clean, tables) in NetMessageCalibration.CalibrateStringTables(signon))
        {
            var label = width == 0 ? "varint" : $"{width} bits";
            Console.WriteLine($"  {label,-12} {clean + "/" + signon.Count,14} {tables,14}");
        }

        Console.WriteLine();
        var best = scores[0];
        Console.WriteLine($"Best: {best.Candidate}");
        Console.WriteLine($"  gameplay packets clean: {best.PacketsClean}/{best.PacketsTotal} ({best.PacketCleanRate:P3})");
        Console.WriteLine($"  signon packets clean  : {best.SignonClean}/{best.SignonTotal}");

        if (best.FailuresByType.Count > 0)
        {
            Console.WriteLine("  first-failure message types:");
            foreach (var (type, count) in best.FailuresByType.OrderByDescending(kv => kv.Value))
            {
                Console.WriteLine($"    {type,-28} {count}");
            }
        }

        return 0;
    }

    private static int RunJson(string[] args)
    {
        if (args.Length == 0)
        {
            return Fail("json requires a file path.");
        }

        var path = args[0];
        string? outputPath = null;
        var includeCommands = false;
        var includeRaw = false;
        var indent = true;
        var maxCommands = int.MaxValue;
        string? gluapackPath = null;
        var skipGluapack = false;
        string? perpDataRoot = null;

        for (var i = 1; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "-o" or "--out":
                    if (++i >= args.Length)
                    {
                        return Fail("--out requires a path.");
                    }

                    outputPath = args[i];
                    break;

                case "--commands":
                    includeCommands = true;
                    break;

                case "--no-commands":
                    includeCommands = false;
                    break;

                case "--include-raw":
                    includeRaw = true;
                    break;

                case "--indent":
                    indent = true;
                    break;

                case "--compact":
                    indent = false;
                    break;

                case "--max-commands":
                    if (++i >= args.Length || !int.TryParse(args[i], out maxCommands) || maxCommands < 0)
                    {
                        return Fail("--max-commands requires a non-negative integer.");
                    }

                    break;

                case "--gluapack":
                    if (++i >= args.Length)
                    {
                        return Fail("--gluapack requires a .bsp path.");
                    }

                    gluapackPath = args[i];
                    break;

                case "--no-gluapack":
                    skipGluapack = true;
                    break;

                case "--perp-data":
                    if (++i >= args.Length)
                    {
                        return Fail("--perp-data requires a cache directory.");
                    }

                    perpDataRoot = args[i];
                    break;

                case "--full-bodies":
                    NetMessageLog.MaxBodyBytes = int.MaxValue;
                    break;

                default:
                    return Fail($"Unknown option '{args[i]}'.");
            }
        }

        outputPath ??= CliReport.Current?.OutputPath ?? Path.ChangeExtension(Path.GetFullPath(path), ".json");

        using var output = new FileStream(outputPath, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 16);
        using var reader = DemoReader.Open(path);
        var exporter = new JsonExporter(includeCommands, includeRaw, maxCommands, indent);
        var summary = exporter.Write(
            output,
            reader,
            new FileInfo(path).Length,
            CliReport.ReportProgress,
            outputPath,
            choosePackFile: (pack, demoDir) => ChooseGluapack(pack, demoDir, gluapackPath, skipGluapack),
            afterWalk: perpDataRoot is null ? null : events => WritePerpDataPointer(events, path, outputPath, perpDataRoot));
        if (summary.TailRecovery is { } recovery)
        {
            Console.Error.WriteLine("DEMO_TAIL_RECOVERED: " + recovery.Message);
        }

        RegisterJsonSidecars(outputPath);

        return summary.Errors.Count > 0 ? 3 : 0;
    }

    // Writes <output>.perp.txt: line 1 is the data folder matching the demo's server Lua (empty
    // when there is none), line 2 says why. A failure here never fails the demo export.
    private static void WritePerpDataPointer(EventCollector events, string demo, string outputPath, string cacheRoot)
    {
        string? dir;
        string note;
        try
        {
            dir = PerpData.EnsureCached(events, demo, Path.GetFullPath(cacheRoot), out note);
        }
        catch (Exception error)
        {
            dir = null;
            note = "failed: " + error.Message;
        }

        Console.WriteLine("perp-data: " + note);
        File.WriteAllText(
            Path.ChangeExtension(outputPath, ".perp.txt"),
            (dir ?? "") + "\n" + note + "\n",
            new System.Text.UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
    }

    private static string? ChooseGluapack(Gluapack pack, string? demoDirectory, string? forcedPath, bool skip)
    {
        CliReport.Current?.EndBar();

        if (skip)
        {
            Console.WriteLine();
            Console.WriteLine("  " + Term.Warn("gluapack") + "  " + Term.Muted("skipped. Lua files stay as placeholders (return gluapack())."));
            pack.MarkSkipped();
            return null;
        }

        if (forcedPath is { Length: > 0 })
        {
            if (File.Exists(forcedPath))
            {
                return Path.GetFullPath(forcedPath);
            }

            Console.WriteLine();
            Console.WriteLine("  " + Term.Fail("gluapack file not found") + Term.Muted("  " + forcedPath));
            Console.WriteLine("  " + Term.Muted("Lua files stay as placeholders (return gluapack())."));
            pack.MarkSkipped();
            return null;
        }

        if (Console.IsInputRedirected)
        {
            Console.WriteLine();
            Console.WriteLine("  " + Term.Warn("gluapack") + "  " + Term.Muted("no prompt (input redirected). Lua files stay as placeholders."));
            pack.MarkSkipped();
            return null;
        }

        var suggested = pack.SuggestPackPath(demoDirectory);
        Console.WriteLine();
        Console.WriteLine("  " + Term.Title("gluapack"));
        Console.WriteLine("  " + Term.Muted("This demo packed client Lua into a CDN .bsp. It is not inside the .dem."));
        Console.WriteLine();
        if (suggested is not null)
        {
            Console.WriteLine("  " + Term.Muted("found in the usual Steam GMod download folder:"));
            Console.WriteLine("    " + suggested);
            Console.WriteLine();
            Console.WriteLine("    y       use that file");
            Console.WriteLine("    n       skip; lua stays as return gluapack() placeholders");
            Console.WriteLine("    <path>  a different .bsp");
        }
        else
        {
            Console.WriteLine("  " + Term.Muted("no pack in the usual Steam GMod download folder."));
            Console.WriteLine();
            Console.WriteLine("    n       skip; lua stays as return gluapack() placeholders");
            Console.WriteLine("    <path>  path to the .bsp");
        }

        Console.WriteLine();
        while (true)
        {
            Console.Write("  > ");
            var line = Console.ReadLine();
            if (line is null)
            {
                pack.MarkSkipped();
                return null;
            }

            var text = line.Trim().Trim('"');
            if (text is "n" or "N" or "no" or "skip")
            {
                Console.WriteLine("  " + Term.Muted("skipped. Lua files stay as placeholders (return gluapack())."));
                pack.MarkSkipped();
                return null;
            }

            if (suggested is not null && text is "y" or "Y" or "yes")
            {
                return suggested;
            }

            if (text.Length > 0 && File.Exists(text))
            {
                return Path.GetFullPath(text);
            }

            if (text.Length > 0)
            {
                Console.WriteLine("  " + Term.Fail("not a file") + Term.Muted("  " + text));
            }
        }
    }

    private static void RegisterJsonSidecars(string jsonPath)
    {
        var report = CliReport.Current;
        if (report is null)
        {
            return;
        }

        report.AddOutput(jsonPath);
        var dir = Path.GetDirectoryName(Path.GetFullPath(jsonPath));
        var stem = Path.GetFileNameWithoutExtension(jsonPath);
        if (dir is not { Length: > 0 } || stem is not { Length: > 0 })
        {
            return;
        }

        var net = Path.Combine(dir, stem + ".net.jsonl");
        var lua = Path.Combine(dir, stem + ".lua");
        var audio = Path.Combine(dir, stem + ".audio");
        if (File.Exists(net))
        {
            report.AddOutput(net);
        }

        if (Directory.Exists(lua))
        {
            report.AddOutput(lua);
        }

        if (Directory.Exists(audio))
        {
            report.AddOutput(audio);
        }
    }
}
