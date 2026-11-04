using System.Text.Json;
using GmodDemo.Parser;

namespace GmodDemo.Parser.Tests;

public sealed class FireTests
{
    [Fact]
    public void Networked_lua_fire_retains_baseline_size_changes_pvs_reentry_and_removal()
    {
        var tables=new DataTableSet();
        var cls=new ServerClass { ClassId=0,Name="CSENT_anim",DataTableName="DT_Test" };
        foreach(var name in new[]{"m_vecOrigin[0]","m_vecOrigin[1]","m_vecOrigin[2]","m_RealClassName","m_flModelScale"})
        {
            bool integer=name=="m_RealClassName";
            cls.Flattened.Add(new(cls.Flattened.Count,"DT_Test",name,
                new(integer?SendPropType.Int:SendPropType.Float,name,
                    integer?SendPropFlags.Unsigned:SendPropFlags.NoScale,null,null,0,0,0,integer?16:32),null));
        }
        tables.Classes.Add(cls);tables.ClassesById[0]=cls;
        var world=new EntityWorld();world.AttachTables(tables);world.ForceMode(PropIndexMode.Source2013);
        world.Fires.ResolveClass=index=>index==9?"ent_fire":"ent_not_fire";
        static byte[] Packet(params (int Value,int Width)[] body)
        {
            var fields=new List<(int,int)> { ((int)NetMessageType.SvcPacketEntities,6),
                (32,13),(1,1),(1,32),(0,1),(1,13),(body.Sum(v=>v.Width),24),(0,1) };
            fields.AddRange(body);
            var bytes=new byte[(fields.Sum(v=>v.Item2)+7)/8];int bit=0;
            foreach(var (value,width) in fields) for(int i=0;i<width;++i,++bit)
                if((value&(1<<i))!=0) bytes[bit/8]|=(byte)(1<<(bit%8));
            return bytes;
        }
        int F(float value)=>BitConverter.SingleToInt32Bits(value);
        world.ApplyPacket(0,Packet((4,6),(0,1),(1,1),(0,1),(7,10),
            (1,1),(0,6),(F(10),32),(1,1),(0,6),(F(20),32),(1,1),(0,6),(F(30),32),
            (1,1),(0,6),(9,16),(1,1),(0,6),(F(1.5f),32),(0,1)),null);
        var first=Assert.Single(world.Fires.Keys);
        Assert.Equal(new Vec3(10,20,30),first.Origin);Assert.Equal(1.5f,first.Scale);
        var zeroSize=new FireTimeline { ResolveClass=world.Fires.ResolveClass };
        zeroSize.Observe(0,world.Snapshot(1)! with { ModelScale=0 });
        Assert.Equal(0,Assert.Single(zeroSize.Keys).Scale);
        zeroSize.Observe(1,world.Snapshot(1)! with { Origin=new Vec3(float.NaN,0,0) });
        Assert.Single(zeroSize.Keys);
        world.ApplyPacket(25,Packet((4,6),(0,1),(0,1),(1,1),(16,6),(F(0.5f),32),(0,1)),null);
        Assert.Equal(0.5f,world.Snapshot(1)?.ModelScale);
        Assert.Equal(0.5f,world.Fires.Keys[1].Scale);
        world.ApplyPacket(26,Packet((4,6),(0,1),(0,1),(0,1)),null);
        Assert.Equal(2,world.Fires.Keys.Count);
        world.Fires.Observe(27,world.Snapshot(1)! with { Index=2,RealClassIndex=8 });
        Assert.Equal(2,world.Fires.Keys.Count);
        world.ApplyPacket(30,Packet((4,6),(1,1),(0,1)),null);
        world.ApplyPacket(50,Packet((4,6),(0,1),(0,1),(0,1)),null);
        world.ApplyPacket(75,Packet((4,6),(1,1),(1,1)),null);
        Assert.Equal(0,world.Stats.PacketsFailed);
        Assert.Equal(new[]{true,true,false,true,false},world.Fires.Keys.Select(k=>k.Active));
        Assert.Equal(0.5f,world.Fires.Keys[3].Scale);
        using var output=new MemoryStream();
        using(var writer=new Utf8JsonWriter(output)) {
            writer.WriteStartObject();world.Fires.Write(writer,world.Fires.ResolveClass,25);writer.WriteEndObject();
        }
        using var document=JsonDocument.Parse(output.ToArray());
        var keys=document.RootElement.GetProperty("fires");
        Assert.Equal(5,keys.GetArrayLength());
        Assert.Equal(3,keys[4].GetProperty("time").GetDouble());
        Assert.False(keys[4].GetProperty("active").GetBoolean());
    }
}
