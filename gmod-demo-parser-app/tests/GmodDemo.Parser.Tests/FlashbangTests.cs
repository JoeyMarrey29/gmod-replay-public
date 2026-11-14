using System.Text;
using System.Text.Json;
using GmodDemo.Parser;

namespace GmodDemo.Parser.Tests;

public sealed class FlashbangTests
{
    private sealed class FlashbangDemoFactAttribute : FactAttribute
    {
        public FlashbangDemoFactAttribute()
        {
            if (!File.Exists(Environment.GetEnvironmentVariable("GMOD_FLASHBANG_DEMO")))
                Skip = "Set GMOD_FLASHBANG_DEMO to the November 14 12-39-14 recording.";
        }
    }

    [FlashbangDemoFact]
    public void Supplied_recording_preserves_both_victims_and_resolves_original_smoke_lua()
    {
        var path = Environment.GetEnvironmentVariable("GMOD_FLASHBANG_DEMO")!;
        using var reader = DemoReader.Open(path);
        var events = new EventCollector();
        foreach (var command in reader.ReadCommands())
        {
            if (command is PacketCommand packet) events.AddPacket(packet);
        }
        var flashes = Export(events.Perpheads).EnumerateArray()
            .Where(row => row.TryGetProperty("key", out var key) && key.GetString() == "flashed").ToArray();
        Assert.Equal(2, flashes.Length);
        Assert.Equal(347.89992, flashes[0].GetProperty("endTime").GetDouble(), 3);
        Assert.Equal(352.48391, flashes[1].GetProperty("endTime").GetDouble(), 3);
        var pack = events.Gluapack;
        var packPath = pack.SuggestPackPath(Path.GetDirectoryName(path));
        Assert.NotNull(packPath);
        Assert.True(pack.HasPack || pack.TryLoadFile(packPath));
        var smoke = pack.FileForPath("gamemodes/darkrp/entities/effects/flashbang_smoke/init.lua")
            ?? pack.FileForPath("lua/effects/flashbang_smoke/init.lua");
        Assert.NotNull(smoke);
        Assert.Contains("particle/smokestack", smoke);
        Assert.Contains("math.Rand(2, 3)", smoke);
        Assert.Contains("15 * self.Scale", smoke);
    }

    [Fact]
    public void Flashed_deadlines_follow_server_clock_changes_without_changing_other_floats()
    {
        var decoder = new PerpheadsMessageDecoder();
        decoder.SetServerTickInterval(0.02f);
        decoder.SetServerTick(100, 500100);
        Set(decoder, 100, "FLASHED", 10025);
        Set(decoder, 125, "flashed", 10030);
        Set(decoder, 125, "other", 10030);
        decoder.SetServerTick(200, 600200);
        Set(decoder, 200, "flashed", 12025);
        var rows = Export(decoder);
        Assert.Equal(25, rows[0].GetProperty("endTime").GetDouble());
        Assert.Equal(30, rows[1].GetProperty("endTime").GetDouble());
        Assert.False(rows[2].TryGetProperty("endTime", out _));
        Assert.Equal(25, rows[3].GetProperty("endTime").GetDouble());
        Assert.Equal(2, rows[0].GetProperty("time").GetDouble());
        Assert.Equal(10025, rows[0].GetProperty("value").GetDouble());
    }

    [Fact]
    public void Missing_clock_never_treats_server_uptime_as_a_demo_deadline()
    {
        var decoder = new PerpheadsMessageDecoder();
        Set(decoder, 100, "flashed", 10025);
        Assert.False(Export(decoder)[0].TryGetProperty("endTime", out _));
    }

    [Fact]
    public void Header_rate_correction_preserves_the_real_flash_duration()
    {
        var decoder = new PerpheadsMessageDecoder();
        decoder.SetServerTickInterval(0.04f);
        decoder.SetServerTick(100, 500100);
        Set(decoder, 100, "flashed", 20024);
        // This document is timed at 50 Hz, but the server actually ran at 25 Hz.
        var end = Export(decoder)[0].GetProperty("endTime").GetDouble();
        Assert.Equal(12, end);
        Assert.Equal(24, end * 50 / 25);
        Assert.Throws<InvalidDataException>(() => decoder.SetServerTickInterval(float.NaN));
    }

    private static JsonElement Export(PerpheadsMessageDecoder decoder)
    {
        using var output = new MemoryStream();
        using (var writer = new Utf8JsonWriter(output))
        {
            writer.WriteStartObject();
            decoder.WriteNwVars(writer, 50);
            writer.WriteEndObject();
        }
        using var document = JsonDocument.Parse(output.ToArray());
        return document.RootElement.GetProperty("nwvars").Clone();
    }

    private static void Set(PerpheadsMessageDecoder decoder, int tick, string key, float value)
    {
        var fields = new List<(uint Value, int Width)> { (12, 16), (0, 1) };
        fields.AddRange(Encoding.UTF8.GetBytes(key).Select(b => ((uint)b, 8)));
        fields.Add((0, 8));
        fields.Add((BitConverter.SingleToUInt32Bits(value), 32));
        var bytes = new byte[(fields.Sum(f => f.Width) + 7) / 8];
        var bit = 0;
        foreach (var field in fields)
            for (var i = 0; i < field.Width; ++i, ++bit)
                bytes[bit / 8] |= (byte)(((field.Value >> i) & 1) << (bit % 8));
        decoder.Process(tick, "perpheads_NWVars_setFloat", bytes);
    }
}
