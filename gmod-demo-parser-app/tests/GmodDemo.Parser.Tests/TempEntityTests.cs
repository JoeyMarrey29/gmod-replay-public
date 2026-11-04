using System.Text.Json;
using GmodDemo.Parser;

namespace GmodDemo.Parser.Tests;

public class TempEntityTests
{
    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    public void Player_attack_exports_serialized_player_and_omitted_primary_event(int animation)
    {
        var tables = new DataTableSet();
        var cls = new ServerClass { ClassId=0,Name="CTEPlayerAnimEvent",DataTableName="DT_TEPlayerAnimEvent" };
        foreach (var (name,width) in new[] { ("m_hPlayer",23),("m_iEvent",6) })
        {
            var prop = new SendPropDef(SendPropType.Int,name,SendPropFlags.Unsigned,null,null,0,0,0,width);
            cls.Flattened.Add(new(cls.Flattened.Count,cls.DataTableName,name,prop,null));
        }
        tables.Classes.Add(cls); tables.ClassesById.Add(0,cls);
        var bits = new Bits();
        bits.UInt(0,1); bits.UInt(1,1); bits.UInt(1,1); // No delay, new class 0.
        bits.UInt(1,1); bits.UInt(0,6); bits.UInt((274u << 13) | 65u,23);
        if (animation != 0)
        {
            bits.UInt(1,1); bits.UInt(0,6); bits.UInt((uint)animation,6);
        }
        bits.UInt(0,1);
        var reader = new BitReader(Message(1,bits));
        var decoder = new TempEntityDecoder();
        decoder.Read(ref reader,250,tables);
        Assert.Equal(0,decoder.FailedMessages);
        using var output = new MemoryStream();
        using (var writer = new Utf8JsonWriter(output))
        {
            writer.WriteStartObject();
            decoder.WritePlayerAnimations(writer,25);
            writer.WriteEndObject();
        }
        using var doc = JsonDocument.Parse(output.ToArray());
        var attack = Assert.Single(doc.RootElement.GetProperty("playerAnimations").EnumerateArray());
        Assert.Equal(65,attack.GetProperty("entity").GetInt32());
        Assert.Equal(animation,attack.GetProperty("event").GetInt32());
        Assert.Equal(10,attack.GetProperty("time").GetDouble());
        Assert.Equal(0,attack.GetProperty("data").GetInt32());
    }

    [Theory]
    [InlineData(45,true,"gdcw_universal_impact")]
    [InlineData(66,true,"gdcw_universal_impact")]
    [InlineData(70,true,"gdcw_universal_impact")]
    [InlineData(67,false,"gdcw_universal_impact")]
    [InlineData(77,false,"gdcw_universal_impact")]
    [InlineData(87,false,"gdcw_universal_impact")]
    [InlineData(67,false,"gdcw_universal_penetrate")]
    [InlineData(70,true,"gdcw_universal_penetrate")]
    public void Firearm_impact_only_emits_blood_for_recorded_flesh_material(int material,bool blood,string name)
    {
        var decoder = new PerpheadsMessageDecoder();
        var strings = new Bits();
        strings.UInt(1,16);
        foreach (var c in name) strings.UInt(c,8);
        strings.UInt(0,8);
        decoder.Process(1,"perpheads_stringtable_add",strings.Bytes());
        var bits = new Bits();
        bits.UInt(1,1); bits.UInt(1,16);
        bits.UInt(0,3); // Zero origin.
        bits.UInt(0,1); bits.UInt(1,1); bits.UInt(0,1); // Normal: (0,-1,0).
        bits.UInt(1,1); bits.UInt(0,1); bits.UInt(1,1); bits.UInt(0,14);
        bits.UInt(BitConverter.SingleToUInt32Bits(0.5f),32);
        bits.UInt(BitConverter.SingleToUInt32Bits(material),32);
        decoder.Process(100,"perp_effects",bits.Bytes());
        Assert.Equal(blood ? 1 : 0,decoder.BloodImpacts.Count);
        Assert.Equal(blood ? 0 : 1,decoder.SurfaceImpacts.Count);
        if (!blood)
        {
            var hit = Assert.Single(decoder.SurfaceImpacts);
            Assert.Equal(material,hit.Material);
            Assert.Equal(0.5f,hit.Scale);
            Assert.Equal(name,hit.Name);
            using var output = new MemoryStream();
            using (var writer = new Utf8JsonWriter(output))
            {
                writer.WriteStartObject();
                new TempEntityDecoder().Write(writer,new PrecacheTables(),25,
                    decoder.BloodImpacts,decoder.SurfaceImpacts);
                writer.WriteEndObject();
            }
            using var doc = JsonDocument.Parse(output.ToArray());
            var exported = Assert.Single(doc.RootElement.GetProperty("effects").EnumerateArray());
            Assert.Equal(material,exported.GetProperty("material").GetSingle());
            Assert.Equal(name,exported.GetProperty("name").GetString());
            Assert.Equal(4,exported.GetProperty("time").GetDouble());
            Assert.Equal(0.5f,exported.GetProperty("scale").GetSingle());
            Assert.Equal(new float[]{0,-1,0},exported.GetProperty("normal").EnumerateArray()
                .Select(v => v.GetSingle()).ToArray());
        }
        if (blood)
        {
            Assert.Equal(0.5f,decoder.BloodImpacts[0].Scale);
            Assert.Equal(new float[]{0,-1,0},decoder.BloodImpacts[0].Normal);
        }
        decoder.Process(101,"perp_effects",bits.Bytes()[..^1]);
        Assert.Equal(blood ? 1 : 0,decoder.BloodImpacts.Count);
        Assert.Equal(blood ? 0 : 1,decoder.SurfaceImpacts.Count);
    }

    [Fact]
    public void Perp_blood_hit_exports_recorded_position_without_gunfire()
    {
        var bits = new Bits();
        bits.UInt(1,1); bits.UInt(0,1); bits.UInt(1,1);
        // X = -12.5, Y = 0, Z = 3, using net.WriteVector coordinates.
        bits.UInt(1,1); bits.UInt(1,1); bits.UInt(1,1);
        bits.UInt(11,14); bits.UInt(16,5);
        bits.UInt(1,1); bits.UInt(0,1); bits.UInt(0,1); bits.UInt(2,14);
        var perp = new PerpheadsMessageDecoder();
        perp.Process(250,"ph_bloodsplat",bits.Bytes());
        perp.Process(251,"ph_bloodsplat",[]);
        perp.Process(252,"ph_bullet_sound",[0,0]);
        var hit = Assert.Single(perp.BloodImpacts);
        Assert.Equal(new float[]{-12.5f,0,3},hit.Position);
        using var output = new MemoryStream();
        using (var writer = new Utf8JsonWriter(output))
        {
            writer.WriteStartObject();
            new TempEntityDecoder().Write(writer,new PrecacheTables(),25,perp.BloodImpacts);
            writer.WriteEndObject();
        }
        using var doc = JsonDocument.Parse(output.ToArray());
        var effect = Assert.Single(doc.RootElement.GetProperty("effects").EnumerateArray());
        Assert.Equal("bloodimpact",effect.GetProperty("name").GetString());
        Assert.Equal("dispatch",effect.GetProperty("kind").GetString());
        Assert.Equal(10,effect.GetProperty("time").GetDouble());
        Assert.Equal(-12.5f,effect.GetProperty("origin")[0].GetSingle());
    }

    [Fact]
    public void Sendprop_normal_has_no_component_presence_bits_and_keeps_next_field_aligned()
    {
        var bits = new Bits();
        bits.UInt(0,12);
        bits.UInt(0,12);
        bits.UInt(1,1);
        bits.UInt(173,8);
        var reader = new BitReader(bits.Bytes());
        var prop = new SendPropDef(SendPropType.Vector,"normal",SendPropFlags.Normal,null,null,0,0,0,0);
        var value = Assert.IsType<PropValue.Vector>(PropDecoder.Decode(ref reader,new(0,"test","normal",prop,null)));
        Assert.Equal(new Vec3(0,0,-1),value.Value);
        Assert.Equal(25,reader.BitsRead);
        Assert.Equal(173u,reader.ReadUBitLong(8));
    }

    [Fact]
    public void Temp_entities_reuse_class_and_delta_values_within_a_message()
    {
        var tables = Tables();
        var payload = new Bits();
        payload.UInt(0,1);
        payload.UInt(1,1);
        payload.UInt(1,1);
        payload.UInt(1,1);
        payload.UInt(0,6);
        payload.UInt(47,8);
        payload.UInt(0,1);
        payload.UInt(0,1);
        payload.UInt(0,1);
        payload.UInt(0,1);
        var message = Message(2,payload);
        var reader = new BitReader(message);
        var decoder = new TempEntityDecoder();
        decoder.Read(ref reader,100,tables);
        Assert.Equal(0,decoder.FailedMessages);
        Assert.Equal(2,decoder.Rows.Count);
        Assert.All(decoder.Rows,row => Assert.Equal(new PropValue.Int(47),row.Properties["m_nIndex"]));
        var precache = new PrecacheTables();
        using var output = new MemoryStream();
        using (var writer = new Utf8JsonWriter(output))
        {
            writer.WriteStartObject();
            decoder.Write(writer,precache,25);
            writer.WriteEndObject();
        }
        using var doc = JsonDocument.Parse(output.ToArray());
        Assert.Empty(doc.RootElement.GetProperty("effects").EnumerateArray());
    }

    [Fact]
    public void Zero_count_is_a_reliable_single_event()
    {
        var payload = new Bits();
        payload.UInt(0,1);
        payload.UInt(1,1);
        payload.UInt(1,1);
        payload.UInt(0,1);
        var reader = new BitReader(Message(0,payload));
        var decoder = new TempEntityDecoder();
        decoder.Read(ref reader,100,Tables());
        Assert.Equal(0,decoder.FailedMessages);
        Assert.Single(decoder.Rows);
    }

    [Fact]
    public void Truncated_message_discards_all_its_events()
    {
        var payload = new Bits();
        payload.UInt(0,1);
        payload.UInt(1,1);
        payload.UInt(1,1);
        payload.UInt(1,1);
        payload.UInt(0,6);
        payload.UInt(47,8);
        payload.UInt(0,1);
        var reader = new BitReader(Message(3,payload));
        var decoder = new TempEntityDecoder();
        decoder.Read(ref reader,100,Tables());
        Assert.Equal(1,decoder.FailedMessages);
        Assert.Empty(decoder.Rows);
    }

    private static DataTableSet Tables()
    {
        var tables = new DataTableSet();
        var cls = new ServerClass { ClassId=0,Name="CTEDecal",DataTableName="DT_TEDecal" };
        var prop = new SendPropDef(SendPropType.Int,"m_nIndex",SendPropFlags.Unsigned,null,null,0,0,0,8);
        cls.Flattened.Add(new(0,"DT_TEDecal","m_nIndex",prop,null));
        tables.Classes.Add(cls);
        tables.ClassesById.Add(0,cls);
        return tables;
    }

    private static byte[] Message(uint count,Bits payload)
    {
        var message = new Bits();
        message.UInt(count,8);
        var length = (uint)payload.Length;
        do
        {
            var value = length & 127;
            length >>= 7;
            message.UInt(value | (length > 0 ? 128u : 0),8);
        } while (length > 0);
        foreach (var value in payload.Bytes()) message.UInt(value,8);
        return message.Bytes();
    }

    private sealed class Bits
    {
        private readonly List<byte> _bytes = [];
        public int Length { get; private set; }
        public void UInt(uint value,int width)
        {
            for (var i=0;i<width;++i,++Length)
            {
                if (Length/8 == _bytes.Count) _bytes.Add(0);
                _bytes[Length/8] |= (byte)(((value>>i)&1)<<(Length%8));
            }
        }
        public byte[] Bytes() => [.. _bytes];
    }
}
