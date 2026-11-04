using System.Text;

namespace GmodDemo.Parser;

/// <summary>
/// Writes client Lua recovered from the demo itself (string-table userdata) next to the JSON.
/// Paths with no body still appear in <c>_paths.txt</c>.
/// </summary>
public static class LuaExporter
{
    public static int Write(string directory, PrecacheTables precache, Gluapack? gluapack = null)
    {
        Directory.CreateDirectory(directory);
        var written = 0;
        var seenPath = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        foreach (var record in precache.LuaFileRecords)
        {
            var packed = gluapack?.FileForPath(record.Path);
            var text = packed ?? FromUserData(record.UserData);
            if (text is null || !seenPath.Add(record.Path))
            {
                continue;
            }

            if (WriteFile(directory, record.Path, text))
            {
                written++;
            }
        }

        if (gluapack is { HasPack: true })
        {
            foreach (var path in precache.LuaFiles)
            {
                if (!seenPath.Add(path))
                {
                    continue;
                }

                var packed = gluapack.FileForPath(path);
                if (packed is null)
                {
                    continue;
                }

                if (WriteFile(directory, path, packed))
                {
                    written++;
                }
            }

            foreach (var (hash, text) in gluapack.UnmappedFiles())
            {
                if (WriteFile(directory, Path.Combine("_unmapped", hash + ".lua"), text))
                {
                    written++;
                }
            }
        }

        File.WriteAllLines(
            Path.Combine(directory, "_paths.txt"),
            precache.LuaFiles,
            new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));

        if (gluapack is { Detected: true })
        {
            gluapack.WriteReport(directory);
        }

        return written;
    }

    internal static string? FromUserData(byte[]? userdata)
    {
        if (userdata is not { Length: > 0 })
        {
            return null;
        }

        var decoded = GmodLzma.DecompressLuaCache(userdata) ?? GmodLzma.DecompressLzmaFile(userdata);
        return decoded is { Length: > 0 } ? Encoding.UTF8.GetString(decoded).Trim('\0') : null;
    }

    private static bool WriteFile(string root, string relative, string text)
    {
        var full = SafePath(root, relative);
        Directory.CreateDirectory(Path.GetDirectoryName(full)!);
        File.WriteAllText(full, text, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
        return true;
    }

    public static string SafePath(string root, string relative)
    {
        var normalized = relative.Replace('\\', '/').Trim().TrimStart('/');
        if (normalized.Length == 0 || normalized.Contains("..", StringComparison.Ordinal))
        {
            normalized = Path.GetFileName(normalized);
            if (normalized.Length == 0)
            {
                normalized = "unnamed.lua";
            }
        }

        if (!normalized.EndsWith(".lua", StringComparison.OrdinalIgnoreCase)
            && !normalized.EndsWith(".txt", StringComparison.OrdinalIgnoreCase))
        {
            normalized += ".lua";
        }

        var full = Path.GetFullPath(Path.Combine(root, normalized.Replace('/', Path.DirectorySeparatorChar)));
        var rootFull = Path.GetFullPath(root);
        if (!full.StartsWith(rootFull, StringComparison.OrdinalIgnoreCase))
        {
            return Path.Combine(rootFull, Path.GetFileName(normalized));
        }

        return full;
    }
}
