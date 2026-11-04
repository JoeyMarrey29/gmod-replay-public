using System.Text.Json;
using GmodDemo.Parser;

namespace GmodDemo.Parser.Tests;

public sealed class NetLogTests
{
    [Fact]
    public void SafePath_stays_under_the_export_folder()
    {
        var root = Path.Combine(Path.GetTempPath(), "gmoddem-lua-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            var escaped = LuaExporter.SafePath(root, "../secret.lua");
            Assert.StartsWith(Path.GetFullPath(root), Path.GetFullPath(escaped), StringComparison.OrdinalIgnoreCase);
        }
        finally
        {
            Directory.Delete(root, true);
        }
    }

    [ShortDemoFact]
    public void Json_writes_net_log_and_lua_folder()
    {
        var jsonPath = Path.Combine(Path.GetTempPath(), $"gmoddem-net-{Guid.NewGuid():N}.json");
        var netPath = Path.ChangeExtension(jsonPath, ".net.jsonl");
        var luaDir = Path.ChangeExtension(jsonPath, ".lua");
        try
        {
            using (var output = File.Create(jsonPath))
            using (var reader = DemoReader.Open(RealDemo.ShortPath))
            {
                new JsonExporter(indent: false).Write(
                    output,
                    reader,
                    new FileInfo(RealDemo.ShortPath).Length,
                    outputPath: jsonPath);
            }

            Assert.True(File.Exists(netPath));
            var lines = File.ReadAllLines(netPath);
            Assert.True(lines.Length > 50, $"net rows={lines.Length}");
            Assert.Contains(lines, line => line.Contains("\"type\":\"lua\"", StringComparison.Ordinal));
            Assert.Contains(lines, line => line.Contains("\"type\":\"entities\"", StringComparison.Ordinal));
            Assert.Contains(lines, line => line.Contains("\"type\":\"game\"", StringComparison.Ordinal));

            var luaLine = lines.FirstOrDefault(l => l.Contains("\"type\":\"lua\"", StringComparison.Ordinal)
                && l.Contains("\"subType\":0", StringComparison.Ordinal));
            Assert.False(string.IsNullOrEmpty(luaLine));
            using var sample = JsonDocument.Parse(luaLine);
            Assert.True(sample.RootElement.TryGetProperty("id", out _));
            Assert.True(sample.RootElement.TryGetProperty("hex", out _) || sample.RootElement.TryGetProperty("ascii", out _));

            Assert.True(Directory.Exists(luaDir));
            Assert.True(File.Exists(Path.Combine(luaDir, "_paths.txt")));
        }
        finally
        {
            if (File.Exists(jsonPath))
            {
                File.Delete(jsonPath);
            }

            if (File.Exists(netPath))
            {
                File.Delete(netPath);
            }

            if (Directory.Exists(luaDir))
            {
                Directory.Delete(luaDir, true);
            }

            var audioDir = Path.ChangeExtension(jsonPath, ".audio");
            if (Directory.Exists(audioDir))
            {
                Directory.Delete(audioDir, true);
            }
        }
    }
}
