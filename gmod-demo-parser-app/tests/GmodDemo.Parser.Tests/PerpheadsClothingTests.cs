using System.Text;
using System.Text.Json;
using GmodDemo.Parser;

namespace GmodDemo.Parser.Tests;

public sealed class PerpheadsClothingTests
{
    [Theory]
    [InlineData(10)]
    [InlineData(14)]
    public void Server_lua_layout_preserves_sync_entries_and_entity_references(int width)
    {
        var decoder = new PerpheadsMessageDecoder();
        decoder.Process(0, "perpheads_bonemerge", [0, 0]);
        var sync = new MessageBits();
        sync.UInt(2, 8);
        Store(sync, width, 1, 74);
        Store(sync, width, 2, 91);
        decoder.Process(36, "ph_clothing_data_sync", sync.Bytes());

        var reference = new MessageBits();
        reference.UInt(110, 16);
        reference.UInt(2, 32);
        decoder.Process(50, "ph_clothing_data_store_reference", reference.Bytes());
        decoder.ResolveClothingLayout(Lua(width));

        var entries = Export(decoder);
        Assert.Equal(3, entries.GetArrayLength());
        for (var i = 0; i < 2; i++)
        {
            var entry = entries[i];
            Assert.Equal(i + 1, entry.GetProperty("index").GetInt32());
            Assert.Equal(i == 0 ? 74 : 91, entry.GetProperty("entities")[0].GetInt32());
            Assert.Equal("models/player/test.mdl", entry.GetProperty("modelOverride").GetString());
            var item = entry.GetProperty("clothing")[0];
            Assert.Equal(36, item.GetProperty("type").GetInt32());
            Assert.Equal(700, item.GetProperty("dataNumber").GetInt32());
            Assert.Equal(3, item.GetProperty("skin").GetInt32());
            Assert.Equal(255, item.GetProperty("color")[3].GetInt32());
            Assert.Equal("custom", item.GetProperty("name").GetString());
            Assert.Equal(7, item.GetProperty("decal").GetInt32());
            Assert.Equal(8, item.GetProperty("leftMonogram").GetInt32());
            Assert.Equal(9, item.GetProperty("rightMonogram").GetInt32());
        }
        Assert.Equal(110, entries[2].GetProperty("e").GetInt32());
        Assert.Equal(2, entries[2].GetProperty("index").GetInt32());
    }

    [Fact]
    public void Replay_keeps_string_table_updates_in_message_order()
    {
        var decoder = new PerpheadsMessageDecoder();
        for (uint index = 1; index <= 2; index++)
        {
            var strings = new MessageBits();
            strings.UInt(1, 16);
            strings.String($"models/player/{index}.mdl");
            decoder.Process((int)index, "perpheads_stringtable_add", strings.Bytes());
            var store = new MessageBits();
            Store(store, 10, index, index + 70, index);
            decoder.Process((int)index, "ph_clothing_data_store_new", store.Bytes());
        }
        decoder.ResolveClothingLayout(Lua(10));
        var entries = Export(decoder);
        Assert.Equal("models/player/1.mdl", entries[0].GetProperty("modelOverride").GetString());
        Assert.Equal("models/player/2.mdl", entries[1].GetProperty("modelOverride").GetString());
    }

    [Fact]
    public void September_store_message_uses_ten_bits()
    {
        var decoder = new PerpheadsMessageDecoder();
        decoder.Process(58, "ph_clothing_data_store_new", Convert.FromHexString(
            "D602000005040404040C90000020E0FFFFFF1F20000008F8FFFFFF070B000002FEFFFFFF11300300"));
        decoder.ResolveClothingLayout(Lua(10));
        var entry = Assert.Single(Export(decoder).EnumerateArray());
        Assert.Equal(726, entry.GetProperty("index").GetInt32());
        Assert.Equal(51, entry.GetProperty("entities")[0].GetInt32());
        Assert.Equal(3, entry.GetProperty("clothing").GetArrayLength());
        Assert.Equal(36, entry.GetProperty("clothing")[0].GetProperty("type").GetInt32());
    }

    [Fact]
    public void Truncated_sync_does_not_keep_partial_store_entries()
    {
        var decoder = new PerpheadsMessageDecoder();
        var sync = new MessageBits();
        sync.UInt(2, 8);
        Store(sync, 10, 1, 74);
        decoder.Process(36, "ph_clothing_data_sync", sync.Bytes());
        decoder.ResolveClothingLayout(Lua(10));
        Assert.Empty(Export(decoder).EnumerateArray());
    }

    private static string Lua(int width) =>
        $"function Clothing:ReadClothing()\n local new = Clothing:ClothingFromIndex(net.ReadUInt(16))\n new.DataNumber = net.ReadUInt({width})\nend";

    private static JsonElement Export(PerpheadsMessageDecoder decoder, double tickRate = 25)
    {
        using var stream = new MemoryStream();
        using (var writer = new Utf8JsonWriter(stream))
        {
            writer.WriteStartObject();
            decoder.WriteClothingData(writer, tickRate);
            writer.WriteEndObject();
        }
        using var doc = JsonDocument.Parse(stream.ToArray());
        return doc.RootElement.GetProperty("clothingData").Clone();
    }

    [Theory]
    [InlineData(20, false, 2, 6)]
    [InlineData(25, true, 91, 301)]
    [InlineData(33.333333, false, 27, 88)]
    [InlineData(66.666667, true, 58, 17)]
    [InlineData(100, false, 105, 203)]
    public void Legacy_bonemerge_uses_server_layout_and_recording_tick_rate(
        double tickRate, bool reordered, uint entity, uint entry)
    {
        var decoder = new PerpheadsMessageDecoder();
        var spawn = new MessageBits();
        spawn.UInt(entity, 16);
        spawn.UInt(entry, 16);
        spawn.UInt(0, 1);
        spawn.String("clothing_ent");
        spawn.UInt(0, 1);
        spawn.String("models/perpheads/civilian/female/jeans.mdl");
        spawn.UInt(1, 1);
        spawn.UInt(1, 1);
        Color(spawn);
        spawn.UInt(1, 1);
        spawn.UInt(7, 8);
        spawn.UInt(0, 1);
        spawn.UInt(0, 1);
        var names = reordered
            ? new[] { "clothing_data_number", "future_numeric_field", "clothing_type", "clothing_skin" }
            : new[] { "clothing_skin", "clothing_type", "clothing_decal", "clothing_decal_color",
                "left_monogram", "left_monogram_color", "right_monogram", "right_monogram_color", "clothing_data_number" };
        foreach (var name in names)
        {
            var value = name switch { "clothing_skin" => 3, "clothing_type" => 31, "clothing_data_number" => 12, _ => 0 };
            spawn.UInt(3, 8);
            var raw = unchecked((ulong)BitConverter.DoubleToInt64Bits(value));
            spawn.UInt((uint)raw, 32);
            spawn.UInt((uint)(raw >> 32), 32);
        }
        decoder.Process(10, "perpheads_bonemerge", spawn.Bytes());
        var update = new MessageBits();
        update.UInt(entity, 16);
        update.UInt(entry, 16);
        update.UInt(0, 1);
        decoder.Process(11, "perpheads_bonemerge_entry_visible", update.Bytes());
        update = new MessageBits();
        update.UInt(entity, 16);
        update.UInt(entry, 16);
        update.UInt(1, 1);
        decoder.Process(12, "perpheads_bonemerge_entry_visible", update.Bytes());
        decoder.Process(13, "perpheads_bonemerge_entry_removed", update.Bytes());
        var lua = "function ENT:Initialize()\n"
            + string.Join("\n", names.Select(n => $"self:NetworkVar(\"{n}\")")) + "\nend";
        decoder.ResolveClothingLayout(null, lua);
        var events = Export(decoder, tickRate);
        Assert.Equal(4, events.GetArrayLength());
        var first = events[0];
        Assert.True(first.GetProperty("explicitParts").GetBoolean());
        Assert.Equal(entity, first.GetProperty("entities")[0].GetUInt32());
        for (var i = 0; i < events.GetArrayLength(); i++)
            Assert.Equal((10 + i) / tickRate, events[i].GetProperty("time").GetDouble(), 9);
        var item = first.GetProperty("clothing")[0];
        Assert.Equal(31, item.GetProperty("type").GetInt32());
        Assert.Equal(3, item.GetProperty("skin").GetInt32());
        Assert.Equal(7, item.GetProperty("modelSkin").GetInt32());
        Assert.Equal(12, item.GetProperty("dataNumber").GetInt32());
        Assert.Equal("models/perpheads/civilian/female/jeans.mdl", item.GetProperty("modelPath").GetString());
        Assert.Equal(0, events[1].GetProperty("clothing").GetArrayLength());
        Assert.Equal(1, events[2].GetProperty("clothing").GetArrayLength());
        Assert.Equal(0, events[3].GetProperty("clothing").GetArrayLength());
    }

    [Fact]
    public void Legacy_clothing_without_server_layout_fails_instead_of_guessing_variable_order()
    {
        var decoder = new PerpheadsMessageDecoder();
        decoder.Process(10, "perpheads_bonemerge", []);
        var error = Assert.Throws<InvalidDataException>(() => decoder.ResolveClothingLayout(null));
        Assert.Contains("variable layout", error.Message);
    }

    private static void Store(MessageBits bits, int width, uint index, uint entity, uint modelStringIndex = 0)
    {
        bits.UInt(index, 32);
        bits.UInt(0, 1);
        bits.UInt(1, 1);
        bits.UInt(modelStringIndex == 0 ? 0u : 1u, 1);
        if (modelStringIndex == 0) bits.String("models/player/test.mdl");
        else bits.UInt(modelStringIndex, 16);
        foreach (var value in new uint[] { 8, 1, 1, 14, 14, 1 }) bits.UInt(value, 8);
        bits.UInt(36, 16);
        bits.UInt(700, width);
        bits.UInt(1, 1);
        bits.UInt(12345, 32);
        bits.UInt(1, 1);
        bits.String("custom");
        bits.UInt(3, 8);
        Color(bits);
        foreach (var value in new uint[] { 7, 8, 9 })
        {
            bits.UInt(1, 1);
            bits.UInt(value, 8);
            Color(bits);
        }
        bits.UInt(1, 8);
        bits.UInt(entity, 16);
    }

    private static void Color(MessageBits bits)
    {
        foreach (var value in new uint[] { 12, 34, 56, 255 }) bits.UInt(value, 8);
    }

    private sealed class MessageBits
    {
        private readonly List<byte> _bytes = [];
        private int _position;

        public void UInt(uint value, int width)
        {
            for (var i = 0; i < width; i++, _position++)
            {
                if (_position / 8 == _bytes.Count) _bytes.Add(0);
                _bytes[_position / 8] |= (byte)(((value >> i) & 1) << (_position % 8));
            }
        }

        public void String(string text)
        {
            foreach (var value in Encoding.UTF8.GetBytes(text)) UInt(value, 8);
            UInt(0, 8);
        }

        public byte[] Bytes() => [.. _bytes];
    }
}
