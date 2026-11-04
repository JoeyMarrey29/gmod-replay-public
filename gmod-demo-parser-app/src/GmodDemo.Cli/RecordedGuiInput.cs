namespace GmodDemo.Parser;

// GMod adds a raw BUTTON_CODE list to Source's gameplay input. VGUI clicks do
// not necessarily set IN_ATTACK, and must not be inferred from that flag.
internal sealed record RecordedGuiInput(int Tick, int[] Keys, bool Moving)
{
    public static RecordedGuiInput? Read(UserCommand command)
    {
        var r = new BitReader(command.Payload);
        for (int i=0;i<2;i++) if (r.ReadOneBit()!=0) r.ReadUBitLong(32);
        for (int i=0;i<3;i++) if (r.ReadOneBit()!=0) r.ReadUBitLong(32);
        bool moving=false;
        for (int i=0;i<3;i++)
            if (r.ReadOneBit()!=0) {
                var value=BitConverter.Int32BitsToSingle((int)r.ReadUBitLong(32));
                moving |= float.IsFinite(value) && Math.Abs(value)>1;
            }
        if (r.ReadOneBit()!=0) r.ReadUBitLong(32); // gameplay buttons
        if (r.ReadOneBit()!=0) r.ReadUBitLong(8);
        if (r.ReadOneBit()!=0) {
            r.ReadUBitLong(13);
            if (r.ReadOneBit()!=0) r.ReadUBitLong(6);
        }
        for (int i=0;i<2;i++) if (r.ReadOneBit()!=0) r.ReadUBitLong(16);
        var keys=new List<int>();
        if (r.ReadOneBit()!=0)
            for (int i=0;i<5;i++) {
                int key=r.ReadByte();
                if (key==0) break;
                if (key>171) return null;
                keys.Add(key);
            }
        r.ReadUBitLong(6); // remaining GMod flags, not cursor coordinates
        return r.IsOverflowed || r.BitsLeft>7 ? null : new(command.Tick,keys.ToArray(),moving);
    }
}
