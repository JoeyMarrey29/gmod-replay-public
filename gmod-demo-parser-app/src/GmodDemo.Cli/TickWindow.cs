using System.Text.Json;
using GmodDemo.Parser;

namespace GmodDemo.Cli;

internal static partial class Program
{
    /// <summary>
    /// Writes the human-facing slice of a demo: every player, then camera / chat / console
    /// events inside a tick window.
    /// </summary>
    private static int RunWindow(string[] args)
    {
        if (args.Length is < 1 or > 3)
        {
            return Fail("window requires a file path, optionally a start tick and a tick count.");
        }

        var startTick = args.Length >= 2 && int.TryParse(args[1], out var parsedStart) ? parsedStart : 0;
        var tickCount = args.Length >= 3 && int.TryParse(args[2], out var parsedCount) ? parsedCount : 30;
        var endTick = startTick + tickCount - 1;

        var players = new PlayerDirectory();
        var events = new EventCollector();
        double tickRate = 66.6666666667;
        using (var reader = DemoReader.Open(args[0]))
        {
            tickRate = reader.Header.TickRate ?? tickRate;
            events.SetRecorderName(reader.Header.ClientName);
            foreach (var command in reader.ReadCommands())
            {
                CliReport.Current?.Report(reader.Position);
                switch (command)
                {
                    case PacketCommand packet:
                        players.AddPacket(packet.Payload);
                        events.AddPacket(packet);
                        break;
                    case ConsoleCommand console:
                        events.AddConsole(console);
                        break;
                    case DataTablesCommand dataTables:
                        events.AddDataTables(dataTables);
                        break;
                    case StringTablesCommand stringTables:
                        players.AddStringTablesDump(stringTables.Payload);
                        events.Precache.AddPayload(stringTables.Payload);
                        break;
                }
            }
        }

        var snapshot = players.Snapshot();
        snapshot = events.BindPlayers(snapshot);

        var outputPath = CliReport.Current?.OutputPath
            ?? Path.ChangeExtension(Path.GetFullPath(args[0]), ".window.json");
        using var stream = new FileStream(outputPath, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 16);
        using var writer = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = true });
        writer.WriteStartObject();
        writer.WriteNumber("schemaVersion", PlaybackDocument.SchemaVersion);
        writer.WriteNumber("fromTick", startTick);
        writer.WriteNumber("toTick", endTick);
        PlaybackDocument.Build(
            snapshot,
            events.Events,
            events.Precache,
            tickRate,
            fromTick: startTick,
            toTick: endTick).Write(writer);
        writer.WriteEndObject();
        writer.Flush();
        return 0;
    }

    private static int RunDataTables(string[] args)
    {
        if (args.Length != 1)
        {
            return Fail("datatables requires a file path.");
        }

        DataTableSet? tables = null;
        var events = new EventCollector();
        using (var reader = DemoReader.Open(args[0]))
        {
            events.SetRecorderName(reader.Header.ClientName);
            foreach (var command in reader.ReadCommands())
            {
                CliReport.Current?.Report(reader.Position);
                switch (command)
                {
                    case DataTablesCommand dataTables:
                        tables = DataTableSet.Parse(dataTables.Payload);
                        events.AddDataTables(dataTables);
                        break;
                    case PacketCommand packet:
                        events.AddPacket(packet);
                        break;
                }
            }
        }

        if (tables is null)
        {
            Console.WriteLine(Term.Warn("  no dem_datatables command in this file"));
            return 2;
        }

        Term.Heading("Send tables");
        Term.Kv("overflowed", tables.Overflowed.ToString());
        Term.Kv("bits left", tables.BitsLeft.ToString());
        Term.Kv("send tables", tables.Tables.Count.ToString());
        Term.Kv("server classes", tables.Classes.Count.ToString());
        Term.Kv("DT_BaseEntity", tables.HasBaseEntity.ToString());
        Term.Kv("m_vecOrigin", tables.HasOrigin.ToString());

        var playerClasses = tables.Classes.Where(c => c.IsPlayer).ToList();
        Term.Kv("player classes", playerClasses.Count.ToString());
        foreach (var player in playerClasses.Take(8))
        {
            Console.WriteLine(
                $"    {Term.Muted($"id={player.ClassId}")}  {player.Name,-28} {Term.Muted("dt=" + player.DataTableName)}  props={player.Flattened.Count}");
        }

        var baseEntity = tables.Tables.FirstOrDefault(t => t.Name == "DT_BaseEntity");
        if (baseEntity is not null)
        {
            Term.Heading("DT_BaseEntity");
            foreach (var prop in baseEntity.Props.Take(20))
            {
                Console.WriteLine($"  {Term.Muted(prop.Type.ToString().PadRight(10))}  {prop.Name}");
            }
        }

        var stats = events.EntityStats;
        Term.Heading("Entities");
        Term.Kv("prop index mode", stats.Mode.ToString());
        Term.Kv("packets", $"{stats.PacketsClean}/{stats.Packets} clean, {stats.PacketsFailed} failed");
        Term.Kv("entered", stats.EntitiesEntered.ToString());
        Term.Kv("origin updates", stats.OriginUpdates.ToString());
        Term.Kv("camera matches", stats.CameraMatches.ToString());
        Term.Kv("origins trusted", stats.OriginsTrusted.ToString());
        if (events.Entities.RecorderEntity is { } recorder)
        {
            Term.Kv("recorder entity", recorder.ToString());
        }

        return 0;
    }
}
