using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;
using GmodDemo.Cli;
using GmodDemo.Parser;

namespace GmodDemo.Parser.Tests;

public sealed class GluapackTests
{
    [Theory]
    [InlineData("0123456789abcdef0123456789abcde")]
    [InlineData("0123456789abcdef0123456789abcdef")]
    public void Historical_archive_restores_only_the_exact_pack_after_original_is_removed(string id)
    {
        var root = Path.Combine(Path.GetTempPath(), "gmoddem-history-" + Guid.NewGuid().ToString("N"));
        var downloads = Path.Combine(root, "downloads");
        var archive = Path.Combine(root, "archive");
        Directory.CreateDirectory(downloads);
        var previous = Environment.GetEnvironmentVariable("GMOD_GLUAPACK_ARCHIVE_DIR");
        var previousDownloads = Environment.GetEnvironmentVariable("GMOD_GLUAPACK_DIR");
        try
        {
            Environment.SetEnvironmentVariable("GMOD_GLUAPACK_ARCHIVE_DIR", archive);
            Environment.SetEnvironmentVariable("GMOD_GLUAPACK_DIR", downloads);
            var salt = Encoding.UTF8.GetBytes("historic-test-salt");
            const string path = "lua/historical.lua";
            const string source = "print('exact historical source, never substitute a different version')\n";
            var bytes = BuildCatalog(salt, path, source);
            var original = Path.Combine(downloads, id + ".bsp");
            File.WriteAllBytes(original, bytes);
            File.WriteAllBytes(Path.Combine(downloads, "not-a-pack.bsp"), bytes);

            var pack = new Gluapack();
            pack.ReadConVar("gluapack_file", id);
            pack.ReadConVar("gluapack_salt", Convert.ToBase64String(salt));
            Assert.Equal(Path.GetFullPath(original), pack.SuggestPackPath(null));
            Assert.Equal(bytes, File.ReadAllBytes(Path.Combine(archive, id + ".bsp")));
            Assert.False(File.Exists(Path.Combine(archive, "not-a-pack.bsp")));
            Assert.False(pack.HasPack);
            File.Delete(original);
            var fallback = pack.SuggestPackPath(null);
            Assert.Equal(Path.Combine(archive, id + ".bsp"), fallback);
            Assert.True(pack.TryLoadFile(fallback!));
            Assert.Equal(source, pack.FileForPath(path));

            var missing = new Gluapack();
            missing.ReadConVar("gluapack_file", "ffffffffffffffffffffffffffffffff");
            Assert.Null(missing.SuggestPackPath(null));
            Assert.False(missing.HasPack);
            Assert.Empty(Directory.GetFiles(archive, "*.tmp"));
        }
        finally
        {
            Environment.SetEnvironmentVariable("GMOD_GLUAPACK_ARCHIVE_DIR", previous);
            Environment.SetEnvironmentVariable("GMOD_GLUAPACK_DIR", previousDownloads);
            Directory.Delete(root, true);
        }
    }

    [Fact]
    public void Archive_is_idempotent_and_does_not_replace_existing_packs()
    {
        var root = Path.Combine(Path.GetTempPath(), "gmoddem-history-copy-" + Guid.NewGuid().ToString("N"));
        var downloads = Path.Combine(root, "downloads");
        var archive = Path.Combine(root, "archive");
        Directory.CreateDirectory(downloads);
        try
        {
            var file = Path.Combine(downloads, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.bsp");
            var bytes = new byte[128];
            File.WriteAllBytes(file, bytes);
            Assert.Equal(1, Gluapack.ArchiveLocalPacks([downloads, downloads], archive));
            Assert.Equal(0, Gluapack.ArchiveLocalPacks([downloads, archive], archive));
            File.WriteAllBytes(file, Enumerable.Repeat((byte)1, 128).ToArray());
            Assert.Equal(0, Gluapack.ArchiveLocalPacks([downloads], archive));
            Assert.Equal(bytes, File.ReadAllBytes(Path.Combine(archive, Path.GetFileName(file))));
            Assert.True(File.Exists(file));
            Assert.Empty(Directory.GetFiles(archive, "*.tmp"));
        }
        finally
        {
            Directory.Delete(root, true);
        }
    }

    [Theory]
    [InlineData("return gluapack()")]
    [InlineData("return gluapack()()")]
    [InlineData("  return gluapack(\"x\")\r\n")]
    public void Stub_text_is_detected(string text)
    {
        Assert.True(Gluapack.IsStub(text));
    }

    [Theory]
    [InlineData("")]
    [InlineData("print('hi')")]
    [InlineData("return gluapack()\nENT.Model = 'x'")]
    public void Real_lua_is_not_a_stub(string text)
    {
        Assert.False(Gluapack.IsStub(text));
    }

    [Fact]
    public void Catalog_maps_hashed_paths()
    {
        var salt = Encoding.UTF8.GetBytes("gluapack-test-salt-bytes!!");
        const string path = "lua/weapons/weapon_test.lua";
        const string source = "ENT.Model = \"models/weapons/w_test.mdl\"\n";
        var catalog = BuildCatalog(salt, path, source);

        var pack = new Gluapack();
        pack.ReadConVar("gluapack_salt", Convert.ToBase64String(salt));
        pack.ReadConVar("gluapack_file", "packid");
        Assert.True(pack.TryLoadBytes(catalog, "test"));
        Assert.Equal(source.Trim('\0'), pack.FileForPath(path));
        Assert.Equal(source.Trim('\0'), pack.FileForPath("weapons/weapon_test.lua"));
    }

    [Fact]
    public void Truncated_lua_path_lists_still_bind_hash_verified_attachment_module_files()
    {
        var salt = Encoding.UTF8.GetBytes("truncated-lua-manifest-test");
        var events = new EventCollector();
        events.Gluapack.ReadConVar("gluapack_salt", Convert.ToBase64String(salt));
        var catalog = new List<byte> { 1 };
        foreach (var name in PerpData.AttachmentFiles)
        {
            var path = PerpData.AttachmentFolder + name;
            catalog.AddRange(BuildCatalog(salt, path, $"-- exact packed source: {name}\n").Skip(1));
        }
        const string unrelated = "gamemodes/darkrp/gamemode/modules/other/unknown.lua";
        catalog.AddRange(BuildCatalog(salt, unrelated, "-- unrelated packed source\n").Skip(1));
        Assert.True(events.Gluapack.TryLoadBytes(catalog.ToArray(), "test"));
        Assert.Empty(events.Precache.LuaFiles);
        var tree = PerpData.BuildTree(events, "truncated.dem", null);
        foreach (var name in PerpData.AttachmentFiles)
            Assert.Contains(tree.Files, file => file.Path == PerpData.AttachmentFolder + name
                && file.Text == $"-- exact packed source: {name}\n");
        Assert.Single(tree.Files.Where(file => file.Path.StartsWith("_unmapped/")));
    }

    [Fact]
    public void Rc4_pack_roundtrip()
    {
        var salt = Encoding.UTF8.GetBytes("gluapack-test-salt-bytes!!");
        var key = Convert.FromHexString("00112233445566778899aabbccddeeff");
        const string path = "lua/autorun/client/cl_pack.lua";
        const string source = "hook.Add(\"Think\", \"x\", function() end)\n";
        var catalog = BuildCatalog(salt, path, source);
        var encrypted = Rc4(key, catalog);

        var pack = new Gluapack();
        pack.ReadConVar("gluapack_key", Convert.ToHexString(key));
        pack.ReadConVar("gluapack_salt", Convert.ToBase64String(salt));
        Assert.True(pack.TryLoadBytes(encrypted, "test"));
        Assert.Contains("hook.Add", pack.FileForPath(path), StringComparison.Ordinal);
    }

    [Fact]
    public void Exporter_replaces_stubs_from_the_pack()
    {
        var salt = Encoding.UTF8.GetBytes("gluapack-test-salt-bytes!!");
        const string path = "lua/entities/sent_box.lua";
        const string source = "ENT.Type = \"anim\"\nENT.Model = \"models/props_junk/wood_crate001a.mdl\"\n";
        var catalog = BuildCatalog(salt, path, source);

        var pack = new Gluapack();
        pack.ReadConVar("gluapack_salt", Convert.ToBase64String(salt));
        Assert.True(pack.TryLoadBytes(catalog, "test"));
        pack.NoteLuaBody("return gluapack()");

        var precache = new PrecacheTables();
        precache.SetLuaFileRecords([new LuaFileRecord(path, "return gluapack()"u8.ToArray())]);

        var dir = Path.Combine(Path.GetTempPath(), "gmoddem-gluapack-" + Guid.NewGuid().ToString("N"));
        try
        {
            LuaExporter.Write(dir, precache, pack);
            var written = File.ReadAllText(LuaExporter.SafePath(dir, path));
            Assert.Contains("ENT.Model", written, StringComparison.Ordinal);
            Assert.DoesNotContain("return gluapack()", written, StringComparison.Ordinal);
            Assert.True(File.Exists(Path.Combine(dir, "_gluapack.txt")));
        }
        finally
        {
            if (Directory.Exists(dir))
            {
                Directory.Delete(dir, true);
            }
        }
    }

    [Fact]
    public void SuggestPackPath_does_not_load_until_asked()
    {
        var dir = Path.Combine(Path.GetTempPath(), "gmoddem-gluapack-suggest-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        var packFile = Path.Combine(dir, "packid.bsp");
        try
        {
            File.WriteAllBytes(packFile, [1, 2, 3, 4]);
            var pack = new Gluapack();
            pack.ReadConVar("gluapack_file", "packid");
            Assert.Equal(Path.GetFullPath(packFile), pack.SuggestPackPath(dir));
            Assert.False(pack.HasPack);
        }
        finally
        {
            Directory.Delete(dir, true);
        }
    }

    [Fact]
    public void SuggestPackPath_searches_the_configured_gmod_pack_directory()
    {
        var dir = Path.Combine(Path.GetTempPath(), "gmoddem-gluapack-env-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        var packFile = Path.Combine(dir, "custom-pack.bsp");
        var previous = Environment.GetEnvironmentVariable("GMOD_GLUAPACK_DIR");
        try
        {
            File.WriteAllBytes(packFile, [1, 2, 3, 4]);
            Environment.SetEnvironmentVariable("GMOD_GLUAPACK_DIR", dir);
            var pack = new Gluapack();
            pack.ReadConVar("gluapack_file", "custom-pack");
            Assert.Equal(Path.GetFullPath(packFile), pack.SuggestPackPath(null));
        }
        finally
        {
            Environment.SetEnvironmentVariable("GMOD_GLUAPACK_DIR", previous);
            Directory.Delete(dir, recursive: true);
        }
    }

    [Fact]
    public void SuggestPackPath_searches_secondary_steam_libraries()
    {
        var dir = Path.Combine(Path.GetTempPath(), "gmoddem-steam-library-" + Guid.NewGuid().ToString("N"));
        var steam = Path.Combine(dir, "Steam");
        var library = Path.Combine(dir, "Library");
        var packFile = Path.Combine(library, "steamapps", "common", "GarrysMod", "garrysmod",
            "download", "data", "gluapack", "custom-pack.bsp");
        Directory.CreateDirectory(Path.GetDirectoryName(packFile)!);
        var previousSteamDir = Environment.GetEnvironmentVariable("STEAM_DIR");
        var previousPackDir = Environment.GetEnvironmentVariable("GMOD_GLUAPACK_DIR");
        try
        {
            File.WriteAllBytes(packFile, [1, 2, 3, 4]);
            Directory.CreateDirectory(Path.Combine(steam, "steamapps"));
            var escapedLibrary = library.Replace(@"\", @"\\", StringComparison.Ordinal);
            File.WriteAllText(Path.Combine(steam, "steamapps", "libraryfolders.vdf"),
                $"\"libraryfolders\"\n{{\n\"0\"\n{{\n\"path\" \"{escapedLibrary}\"\n}}\n}}\n");
            Environment.SetEnvironmentVariable("STEAM_DIR", steam);
            Environment.SetEnvironmentVariable("GMOD_GLUAPACK_DIR", null);

            var pack = new Gluapack();
            pack.ReadConVar("gluapack_file", "custom-pack");
            Assert.Equal(Path.GetFullPath(packFile), pack.SuggestPackPath(null));
        }
        finally
        {
            Environment.SetEnvironmentVariable("STEAM_DIR", previousSteamDir);
            Environment.SetEnvironmentVariable("GMOD_GLUAPACK_DIR", previousPackDir);
            Directory.Delete(dir, recursive: true);
        }
    }

    [ShortDemoFact]
    public void Real_demo_detects_gluapack_when_stubs_or_convars_are_present()
    {
        using var reader = DemoReader.Open(RealDemo.ShortPath);
        var events = new EventCollector();
        foreach (var command in reader.ReadCommands())
        {
            switch (command)
            {
                case PacketCommand packet:
                    events.AddPacket(packet);
                    break;
                case StringTablesCommand tables:
                    events.Precache.AddPayload(tables.Payload);
                    break;
            }
        }

        events.Gluapack.CollectFromDemo(events.Precache);
        if (!events.Gluapack.Detected)
        {
            return;
        }

        Assert.True(events.Gluapack.StubCount > 0 || events.Gluapack.FileName is { Length: > 0 });
    }

    private static byte[] BuildCatalog(byte[] salt, string path, string source)
    {
        var hash = MD5.HashData(Concat(salt, Encoding.UTF8.GetBytes(path)));
        var body = Encoding.UTF8.GetBytes(source);
        var data = new byte[1 + 48 + 4 + body.Length];
        data[0] = 1;
        hash.CopyTo(data, 1);
        hash.CopyTo(data, 17);
        hash.CopyTo(data, 33);
        BinaryPrimitives.WriteUInt32BigEndian(data.AsSpan(49), (uint)body.Length);
        body.CopyTo(data, 53);
        return data;
    }

    private static byte[] Concat(byte[] a, byte[] b)
    {
        var all = new byte[a.Length + b.Length];
        a.CopyTo(all, 0);
        b.CopyTo(all, a.Length);
        return all;
    }

    private static byte[] Rc4(byte[] key, byte[] data)
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
