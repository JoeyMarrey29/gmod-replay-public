using System.Buffers.Binary;
using System.Security;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32;

namespace GmodDemo.Parser;

/// <summary>
/// Some servers replace every client Lua body with <c>return gluapack()</c> and ship the real
/// files in a separate pack (RC4 then LZMA). Keys arrive as replicated convars in the demo.
/// The pack itself is used when it is in the demo userdata or sitting next to the <c>.dem</c>.
/// </summary>
public sealed class Gluapack
{
    public const int HeaderSkip = 256;

    private readonly Dictionary<string, byte[]> _byHash = new(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, string> _byPath = new(StringComparer.OrdinalIgnoreCase);
    private byte[]? _key;
    private byte[]? _salt;

    public string? FileName { get; private set; }

    public string? PackSource { get; private set; }

    public bool Detected { get; private set; }

    public int StubCount { get; private set; }

    public int UnpackedCount => _byPath.Count;

    public bool HasPack => _byHash.Count > 0;

    public IReadOnlyDictionary<string, string> FilesByPath => _byPath;

    public static bool IsStub(string? text)
    {
        if (text is not { Length: > 0 })
        {
            return false;
        }

        var t = text.Trim().Trim('\0');
        if (t.Length is 0 or > 200)
        {
            return false;
        }

        if (!t.StartsWith("return gluapack(", StringComparison.OrdinalIgnoreCase)
            && !t.StartsWith("return gluapack (", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        var lineBreak = t.AsSpan().IndexOfAny("\r\n");
        return lineBreak < 0 || t.AsSpan(lineBreak).Trim().IsEmpty;
    }

    public void ReadConVar(string name, string value)
    {
        if (name.Length == 0 || value.Length == 0)
        {
            return;
        }

        if (name.StartsWith("gluapack", StringComparison.OrdinalIgnoreCase))
        {
            Detected = true;
        }

        switch (name.ToLowerInvariant())
        {
            case "gluapack_file":
                FileName = value.Trim();
                break;
            case "gluapack_key":
                _key = TryFromHex(value);
                break;
            case "gluapack_md5":
                break;
            case "gluapack_salt":
                try
                {
                    _salt = Convert.FromBase64String(value.Trim());
                }
                catch (FormatException)
                {
                    _salt = null;
                }

                break;
        }
    }

    public void NoteLuaBody(string? text)
    {
        if (IsStub(text))
        {
            Detected = true;
            StubCount++;
        }
    }

    public bool CollectFromDemo(PrecacheTables precache)
    {
        foreach (var record in precache.LuaFileRecords)
        {
            NoteLuaBody(LuaExporter.FromUserData(record.UserData));
            if (record.UserData is { Length: >= 100_000 } && TryLoadBytes(record.UserData, "demo-userdata"))
            {
                BindPaths(precache.LuaFiles);
                return true;
            }
        }

        BindPaths(precache.LuaFiles);
        return HasPack;
    }

    public string? SuggestPackPath(string? demoDirectory)
    {
        var archive = ArchiveDirectory;
        try
        {
            var imported = ArchiveLocalPacks(GluapackDirectories(), archive);
            if (imported > 0)
                Console.Error.WriteLine($"GLUAPACK_ARCHIVED: {imported} local packs in {archive}");
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        {
            Console.Error.WriteLine($"GLUAPACK_ARCHIVE_FAILED: {error.Message}");
        }
        foreach (var path in CandidatePackFiles(demoDirectory))
        {
            if (File.Exists(path))
            {
                return Path.GetFullPath(path);
            }
        }

        return null;
    }

    public static string ArchiveDirectory => Environment.GetEnvironmentVariable("GMOD_GLUAPACK_ARCHIVE_DIR")
        is { Length: > 0 } configured ? Path.GetFullPath(configured)
        : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "GModReplay", "Gluapacks");

    public static int ArchiveLocalPacks(IEnumerable<string> directories, string archive)
    {
        var imported = 0;
        foreach (var directory in directories.Distinct(StringComparer.OrdinalIgnoreCase))
        {
            if (!Directory.Exists(directory)) continue;
            foreach (var source in Directory.EnumerateFiles(directory, "*.bsp"))
            {
                var id = Path.GetFileNameWithoutExtension(source);
                if (!IsPackId(id) || new FileInfo(source).Length < 64) continue;
                var destination = Path.Combine(archive, id.ToLowerInvariant() + ".bsp");
                if (Path.GetFullPath(source).Equals(Path.GetFullPath(destination), StringComparison.OrdinalIgnoreCase))
                    continue;
                if (File.Exists(destination)) continue;
                Directory.CreateDirectory(archive);
                var temporary = destination + "." + Guid.NewGuid().ToString("N") + ".tmp";
                try
                {
                    File.Copy(source, temporary);
                    try
                    {
                        File.Move(temporary, destination);
                        ++imported;
                    }
                    catch (IOException) when (File.Exists(destination))
                    {
                        // Another parser published the same pack while we were copying.
                    }
                }
                finally
                {
                    if (File.Exists(temporary)) File.Delete(temporary);
                }
            }
        }
        return imported;
    }

    private static bool IsPackId(string id) =>
        id.Length is 31 or 32 && id.All(char.IsAsciiHexDigit);

    public bool TryLoadFile(string path)
    {
        try
        {
            if (!File.Exists(path))
            {
                return false;
            }

            return TryLoadBytes(File.ReadAllBytes(path), "user");
        }
        catch (IOException)
        {
            return false;
        }
    }

    public void MarkSkipped()
    {
        if (PackSource is null or "none")
        {
            PackSource = "skipped";
        }
    }

    public bool TryLoadBytes(ReadOnlySpan<byte> data, string source)
    {
        if (HasPack || data.Length < 64)
        {
            return false;
        }

        if (_key is { Length: > 0 })
        {
            var decrypted = Rc4(_key, data);
            if (TryParseAny(decrypted))
            {
                PackSource = source;
                Detected = true;
                return true;
            }
        }

        if (TryParseAny(data))
        {
            PackSource = source;
            Detected = true;
            return true;
        }

        return false;
    }

    public string? FileForPath(string path)
    {
        if (_byPath.TryGetValue(path, out var cached))
        {
            return cached;
        }

        foreach (var candidate in PathKeys(path))
        {
            if (_byPath.TryGetValue(candidate, out cached))
            {
                _byPath[path] = cached;
                return cached;
            }

            var hash = HashPath(candidate);
            if (hash is not null && _byHash.TryGetValue(hash, out var body))
            {
                var text = Encoding.UTF8.GetString(body).Trim('\0');
                _byPath[path] = text;
                _byPath[candidate] = text;
                return text;
            }
        }

        return null;
    }

    public IEnumerable<(string Hash, string Text)> UnmappedFiles()
    {
        var mapped = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var path in _byPath.Keys)
        {
            foreach (var key in PathKeys(path))
            {
                var hash = HashPath(key);
                if (hash is not null)
                {
                    mapped.Add(hash);
                }
            }
        }

        var seenBody = new HashSet<string>(StringComparer.Ordinal);
        foreach (var (hash, body) in _byHash)
        {
            if (mapped.Contains(hash))
            {
                continue;
            }

            var text = Encoding.UTF8.GetString(body).Trim('\0');
            if (!seenBody.Add(text))
            {
                continue;
            }

            yield return (hash, text);
        }
    }

    public void WriteReport(string directory)
    {
        Directory.CreateDirectory(directory);
        var lines = new[]
        {
            $"detected={(Detected ? "true" : "false")}",
            $"stubs={StubCount}",
            $"unpacked={UnpackedCount}",
            $"hashes={_byHash.Count}",
            $"pack={PackSource ?? "none"}",
            $"file={FileName ?? ""}",
            $"key={(_key is { Length: > 0 } ? "yes" : "no")}",
            $"salt={(_salt is { Length: > 0 } ? "yes" : "no")}",
        };
        File.WriteAllLines(
            Path.Combine(directory, "_gluapack.txt"),
            lines,
            new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
    }

    private IEnumerable<string> CandidatePackFiles(string? demoDirectory)
    {
        var names = new List<string>();
        if (FileName is { Length: > 0 } name)
        {
            var trimmed = name.Trim().Trim('"');
            names.Add(trimmed);
            names.Add(trimmed + ".bsp");
            names.Add(Path.GetFileName(trimmed));
            names.Add(Path.GetFileName(trimmed) + ".bsp");
        }

        if (demoDirectory is { Length: > 0 } && names.Count > 0)
        {
            foreach (var n in names)
            {
                yield return Path.Combine(demoDirectory, n);
            }
        }

        foreach (var dir in GluapackDirectories())
        {
            if (names.Count == 0)
            {
                continue;
            }

            foreach (var n in names)
            {
                yield return Path.Combine(dir, Path.GetFileName(n));
            }
        }
        if (FileName is { Length: > 0 } archiveName)
        {
            var id = Path.GetFileNameWithoutExtension(archiveName.Trim().Trim('"'));
            if (IsPackId(id))
                yield return Path.Combine(ArchiveDirectory, id.ToLowerInvariant() + ".bsp");
        }
    }

    private static IEnumerable<string> GluapackDirectories()
    {
        var env = Environment.GetEnvironmentVariable("GMOD_GLUAPACK_DIR");
        if (env is { Length: > 0 })
        {
            yield return env;
        }

        foreach (var steam in SteamLibraries())
        {
            yield return Path.Combine(steam, "steamapps", "common", "GarrysMod", "garrysmod", "download", "data", "gluapack");
            yield return Path.Combine(steam, "steamapps", "common", "GarrysMod", "garrysmod", "data", "gluapack");
        }
    }

    private static IEnumerable<string> SteamLibraries()
    {
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var root in SteamRoots())
        {
            if (!seen.Add(root))
            {
                continue;
            }

            yield return root;
            var vdf = Path.Combine(root, "steamapps", "libraryfolders.vdf");
            if (!File.Exists(vdf))
            {
                continue;
            }

            string text;
            try
            {
                text = File.ReadAllText(vdf);
            }
            catch (IOException)
            {
                continue;
            }

            var from = 0;
            while (from < text.Length)
            {
                var key = text.IndexOf("\"path\"", from, StringComparison.OrdinalIgnoreCase);
                if (key < 0)
                {
                    break;
                }

                var open = text.IndexOf('"', key + 6);
                if (open < 0)
                {
                    break;
                }

                var close = text.IndexOf('"', open + 1);
                if (close < 0)
                {
                    break;
                }

                var path = text[(open + 1)..close].Replace(@"\\", @"\");
                from = close + 1;
                if (path.Length > 0 && seen.Add(path))
                {
                    yield return path;
                }
            }
        }
    }

    private static IEnumerable<string> SteamRoots()
    {
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var roots = new List<string>();
        void Add(string? path)
        {
            if (!string.IsNullOrWhiteSpace(path) && seen.Add(path))
            {
                roots.Add(path);
            }
        }

        Add(Environment.GetEnvironmentVariable("STEAM_DIR"));
        Add(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "Steam"));
        Add(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "Steam"));
        if (OperatingSystem.IsWindows())
        {
            foreach (var (key, value) in new[]
                     {
                         (@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath"),
                         (@"HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath"),
                         (@"HKEY_LOCAL_MACHINE\SOFTWARE\Valve\Steam", "InstallPath"),
                     })
            {
                try
                {
                    Add(Registry.GetValue(key, value, null) as string);
                }
                catch (SecurityException)
                {
                    Console.Error.WriteLine($"Could not read Steam library registry key {key}.");
                }
            }

            foreach (var drive in DriveInfo.GetDrives())
            {
                if (drive.DriveType != DriveType.Fixed)
                {
                    continue;
                }

                foreach (var folder in new[] { "Steam", Path.Combine("Program Files (x86)", "Steam"),
                                               Path.Combine("Program Files", "Steam"), "Games\\Steam",
                                               "Games\\SteamLibrary" })
                {
                    var candidate = Path.Combine(drive.RootDirectory.FullName, folder);
                    if (Directory.Exists(candidate))
                    {
                        Add(candidate);
                    }
                }
            }
        }

        foreach (var root in roots)
        {
            yield return root;
        }
    }

    private bool TryParseAny(ReadOnlySpan<byte> data)
    {
        if (TryParseCatalog(data))
        {
            return true;
        }

        if (data.Length > HeaderSkip && TryParseCatalog(data[HeaderSkip..]))
        {
            return true;
        }

        return TryDecompressCatalog(data)
            || (data.Length > HeaderSkip && TryDecompressCatalog(data[HeaderSkip..]));
    }

    private bool TryDecompressCatalog(ReadOnlySpan<byte> blob)
    {
        if (blob.Length < 14)
        {
            return false;
        }

        var unpacked = GmodLzma.UtilDecompress(blob) ?? GmodLzma.DecompressLzmaFile(blob);
        return unpacked is { Length: > 0 } && TryParseCatalog(unpacked);
    }

    private bool TryParseCatalog(ReadOnlySpan<byte> data)
    {
        Dictionary<string, byte[]>? best = null;
        foreach (var start in new[] { 0, 1, 4 })
        {
            var found = ParseCatalogFrom(data, start);
            if (found.Count > (best?.Count ?? 0))
            {
                best = found;
            }
        }

        if (best is not { Count: > 0 })
        {
            return false;
        }

        foreach (var (hash, body) in best)
        {
            _byHash[hash] = body;
        }

        return true;
    }

    private static Dictionary<string, byte[]> ParseCatalogFrom(ReadOnlySpan<byte> data, int start)
    {
        var found = new Dictionary<string, byte[]>(StringComparer.OrdinalIgnoreCase);
        if (start < 0 || start + 52 > data.Length)
        {
            return found;
        }

        var i = start;
        while (i + 52 <= data.Length)
        {
            var vfs = Convert.ToHexString(data.Slice(i, 16)).ToLowerInvariant();
            var lcl1 = Convert.ToHexString(data.Slice(i + 16, 16)).ToLowerInvariant();
            var lcl2 = Convert.ToHexString(data.Slice(i + 32, 16)).ToLowerInvariant();
            i += 48;
            var len = BinaryPrimitives.ReadUInt32BigEndian(data.Slice(i, 4));
            i += 4;
            if (len == 0 || len > 2_000_000 || i + (int)len > data.Length)
            {
                break;
            }

            var body = data.Slice(i, (int)len).ToArray();
            i += (int)len;

            // Entries are length-prefixed, so only the first body needs the text check to confirm
            // the key and start offset; later bodies may legitimately hold non-ASCII text.
            if (found.Count == 0 && !LooksLikeLua(body))
            {
                break;
            }

            found[vfs] = body;
            found[lcl1] = body;
            found[lcl2] = body;
        }

        return found;
    }

    public void BindAfterLoad(PrecacheTables precache) => BindPaths(precache.LuaFiles);

    private void BindPaths(IReadOnlyList<string> paths)
    {
        foreach (var path in paths)
        {
            _ = FileForPath(path);
        }
    }

    private string? HashPath(string path)
    {
        if (_salt is null)
        {
            return null;
        }

        var bytes = new byte[_salt.Length + Encoding.UTF8.GetByteCount(path)];
        _salt.CopyTo(bytes, 0);
        Encoding.UTF8.GetBytes(path, 0, path.Length, bytes, _salt.Length);
        return Convert.ToHexString(MD5.HashData(bytes)).ToLowerInvariant();
    }

    private static IEnumerable<string> PathKeys(string path)
    {
        var n = path.Replace('\\', '/').Trim().TrimStart('/', '@');
        if (n.Length == 0)
        {
            yield break;
        }

        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var withLua = n.StartsWith("lua/", StringComparison.OrdinalIgnoreCase) ? n : "lua/" + n;
        foreach (var candidate in new[]
                 {
                     n,
                     withLua,
                     StripLua(n),
                     StripLua(StripGamemodes(StripAddon(n))),
                     StripLua(StripGamemodeEntities(StripAddon(n))),
                     StripLua(StripAddon(n)),
                     StripAddon(n),
                 })
        {
            if (candidate.Length > 0 && seen.Add(candidate))
            {
                yield return candidate;
            }
        }
    }

    private static string StripLua(string path) =>
        path.StartsWith("lua/", StringComparison.OrdinalIgnoreCase) ? path[4..] : path;

    private static string StripAddon(string path) => StripFirstFolder(path, "addons/");

    private static string StripGamemodes(string path) =>
        path.StartsWith("gamemodes/", StringComparison.OrdinalIgnoreCase) ? path["gamemodes/".Length..] : path;

    private static string StripGamemodeEntities(string path)
    {
        const string prefix = "gamemodes/";
        if (!path.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
        {
            return path;
        }

        var rest = path[prefix.Length..];
        var slash = rest.IndexOf('/');
        if (slash < 0)
        {
            return path;
        }

        var afterName = rest[(slash + 1)..];
        return afterName.StartsWith("entities/", StringComparison.OrdinalIgnoreCase)
            ? afterName["entities/".Length..]
            : path;
    }

    private static string StripFirstFolder(string path, string requiredPrefix)
    {
        if (!path.StartsWith(requiredPrefix, StringComparison.OrdinalIgnoreCase))
        {
            return path;
        }

        var slash = path.IndexOf('/', requiredPrefix.Length);
        return slash < 0 ? path : path[(slash + 1)..];
    }

    private static bool LooksLikeLua(byte[] body)
    {
        if (body.Length == 0 || body.Length > 2_000_000)
        {
            return false;
        }

        var check = Math.Min(body.Length, 256);
        var nuls = 0;
        var printable = 0;
        for (var i = 0; i < check; i++)
        {
            var b = body[i];
            if (b == 0)
            {
                nuls++;
            }

            if (b is >= 32 and <= 126 or (byte)'\n' or (byte)'\r' or (byte)'\t')
            {
                printable++;
            }
        }

        return nuls <= 1 && printable * 10 >= check * 7;
    }

    private static byte[]? TryFromHex(string value)
    {
        var hex = value.Trim();
        if ((hex.Length & 1) != 0)
        {
            return null;
        }

        try
        {
            return Convert.FromHexString(hex);
        }
        catch (FormatException)
        {
            return null;
        }
    }

    internal static byte[] Rc4(ReadOnlySpan<byte> key, ReadOnlySpan<byte> data)
    {
        var s = new byte[256];
        for (var i = 0; i < 256; i++)
        {
            s[i] = (byte)i;
        }

        var j = 0;
        for (var i = 0; i < 256; i++)
        {
            j = (j + s[i] + key[i % key.Length]) & 255;
            (s[i], s[j]) = (s[j], s[i]);
        }

        var output = new byte[data.Length];
        var x = 0;
        var y = 0;
        for (var k = 0; k < data.Length; k++)
        {
            x = (x + 1) & 255;
            y = (y + s[x]) & 255;
            (s[x], s[y]) = (s[y], s[x]);
            output[k] = (byte)(data[k] ^ s[(s[x] + s[y]) & 255]);
        }

        return output;
    }
}
