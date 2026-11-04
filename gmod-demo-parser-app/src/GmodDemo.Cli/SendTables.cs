namespace GmodDemo.Parser;

/// <summary>
/// Send-property types as written by Source SDK 2013 <c>SendTable_WriteInfos</c>.
/// Type 7 is <c>DPT_Int64</c> with <c>SUPPORTS_INT64</c>, otherwise GMod's custom table.
/// </summary>
public enum SendPropType
{
    Int = 0,
    Float = 1,
    Vector = 2,
    VectorXY = 3,
    String = 4,
    Array = 5,
    DataTable = 6,
    Int64 = 7,
    GmodTable = 8,
}

/// <summary>Networked <c>SPROP_*</c> bits from SDK 2013 <c>dt_common.h</c>.</summary>
[Flags]
public enum SendPropFlags
{
    Unsigned = 1 << 0,
    Coord = 1 << 1,
    NoScale = 1 << 2,
    RoundDown = 1 << 3,
    RoundUp = 1 << 4,
    Normal = 1 << 5,
    Exclude = 1 << 6,
    Xyze = 1 << 7,
    InsideArray = 1 << 8,
    ProxyAlwaysYes = 1 << 9,
    ChangesOften = 1 << 10,
    IsAVectorElem = 1 << 11,
    Collapsible = 1 << 12,
    CoordMp = 1 << 13,
    CoordMpLowPrecision = 1 << 14,
    CoordMpIntegral = 1 << 15,
}

public sealed record SendPropDef(
    SendPropType Type,
    string Name,
    SendPropFlags Flags,
    string? ExcludeTable,
    string? DataTableName,
    int NumElements,
    float LowValue,
    float HighValue,
    int Bits);

public sealed class SendTableDef
{
    public required string Name { get; init; }

    public required bool NeedsDecoder { get; init; }

    public List<SendPropDef> Props { get; } = [];
}

public sealed record ServerClassDef(int ClassId, string Name, string DataTableName);

public sealed record FlattenedProp(int Index, string Table, string Name, SendPropDef Prop, SendPropDef? ArrayElement);

public sealed class ServerClass
{
    public required int ClassId { get; init; }

    public required string Name { get; init; }

    public required string DataTableName { get; init; }

    public List<FlattenedProp> Flattened { get; } = [];

    public bool IsPlayer { get; init; }
}

/// <summary>
/// Parses the <c>dem_datatables</c> payload: send tables, then the server-class list.
/// Layout is Source SDK 2013 <c>DataTable_WriteSendTablesBuffer</c> /
/// <c>SendTable_WriteInfos</c> / <c>DataTable_WriteClassInfosBuffer</c>.
/// </summary>
public sealed class DataTableSet
{
    public const int NumPropsBits = 10;
    public const int TypeBits = 5;
    public const int FlagBits = 16;
    public const int NumElementsBits = 10;
    public const int NumBitsBits = 7;

    /// <summary>
    /// Default send-prop priority in later Source. Absent from the bitstream; only
    /// <see cref="SendPropFlags.ChangesOften"/> (priority 64) is recoverable from flags.
    /// </summary>
    public const int DefaultPriority = 128;

    public const int ChangesOftenPriority = 64;

    public List<SendTableDef> Tables { get; } = [];

    public List<ServerClassDef> ClassDefs { get; } = [];

    public List<ServerClass> Classes { get; } = [];

    public Dictionary<int, ServerClass> ClassesById { get; } = [];

    public int BitsLeft { get; private set; }

    public bool Overflowed { get; private set; }

    public bool HasBaseEntity => Tables.Any(t => t.Name == "DT_BaseEntity");

    public bool HasOrigin => Tables.Any(t => t.Props.Any(p => p.Name is "m_vecOrigin" or "m_vecOrigin[0]"));

    public static DataTableSet Parse(byte[] payload)
    {
        var reader = new BitReader(payload);
        var set = new DataTableSet();

        while (reader.ReadOneBit() != 0 && !reader.IsOverflowed)
        {
            var needsDecoder = reader.ReadOneBit() != 0;
            var name = reader.ReadString();
            var numProps = (int)reader.ReadUBitLong(NumPropsBits);
            var table = new SendTableDef { Name = name, NeedsDecoder = needsDecoder };
            for (var i = 0; i < numProps && !reader.IsOverflowed; i++)
            {
                table.Props.Add(ReadProp(ref reader));
            }

            set.Tables.Add(table);
        }

        if (!reader.IsOverflowed && reader.BitsLeft >= 16)
        {
            var classCount = reader.ReadWord();
            for (var i = 0; i < classCount && !reader.IsOverflowed; i++)
            {
                var classId = reader.ReadWord();
                var className = reader.ReadString();
                var tableName = reader.ReadString();
                set.ClassDefs.Add(new ServerClassDef(classId, className, tableName));
            }
        }

        Flatten(set);
        set.BitsLeft = reader.BitsLeft;
        set.Overflowed = reader.IsOverflowed;
        return set;
    }

    private static SendPropDef ReadProp(ref BitReader reader)
    {
        var type = (SendPropType)reader.ReadUBitLong(TypeBits);
        var name = reader.ReadString();
        if (type == SendPropType.Int64 && name == "m_GMOD_DataTable")
        {
            type = SendPropType.GmodTable;
        }
        var flags = (SendPropFlags)reader.ReadUBitLong(FlagBits);

        string? excludeTable = null;
        string? dataTableName = null;
        var numElements = 0;
        var low = 0f;
        var high = 0f;
        var bits = 0;

        if (type == SendPropType.DataTable)
        {
            dataTableName = reader.ReadString();
        }
        else if ((flags & SendPropFlags.Exclude) != 0)
        {
            excludeTable = reader.ReadString();
        }
        else if (type == SendPropType.Array)
        {
            numElements = (int)reader.ReadUBitLong(NumElementsBits);
        }
        else
        {
            low = reader.ReadBitFloat();
            high = reader.ReadBitFloat();
            bits = (int)reader.ReadUBitLong(NumBitsBits);
        }

        return new SendPropDef(type, name, flags, excludeTable, dataTableName, numElements, low, high, bits);
    }

    private static void Flatten(DataTableSet set)
    {
        var byName = new Dictionary<string, SendTableDef>(StringComparer.Ordinal);
        foreach (var table in set.Tables)
        {
            byName.TryAdd(table.Name, table);
        }

        foreach (var def in set.ClassDefs)
        {
            if (!byName.TryGetValue(def.DataTableName, out var table))
            {
                continue;
            }

            var excludes = new HashSet<string>(StringComparer.Ordinal);
            CollectExcludes(table, byName, excludes);

            var gathered = new List<FlattenedProp>();
            Gather(table, byName, excludes, gathered);

            var sorted = SortByPriority(gathered);
            var serverClass = new ServerClass
            {
                ClassId = def.ClassId,
                Name = def.Name,
                DataTableName = def.DataTableName,
                IsPlayer = IsPlayerClass(def.Name, def.DataTableName),
            };
            for (var i = 0; i < sorted.Count; i++)
            {
                serverClass.Flattened.Add(sorted[i] with { Index = i });
            }

            set.Classes.Add(serverClass);
            set.ClassesById[def.ClassId] = serverClass;
        }
    }

    private static bool IsPlayerClass(string className, string dataTableName) =>
        dataTableName is "DT_BasePlayer" or "DT_GMOD_Player" or "DT_HL2_Player" or "DT_HL2MP_Player"
        || className is "CBasePlayer" or "CGMOD_Player" or "CHL2_Player" or "CHL2MP_Player";

    private static void CollectExcludes(
        SendTableDef table,
        Dictionary<string, SendTableDef> byName,
        HashSet<string> excludes)
    {
        foreach (var prop in table.Props)
        {
            if ((prop.Flags & SendPropFlags.Exclude) != 0)
            {
                excludes.Add($"{prop.ExcludeTable}.{prop.Name}");
            }
            else if (prop.Type == SendPropType.DataTable && prop.DataTableName is { } child
                     && byName.TryGetValue(child, out var childTable))
            {
                CollectExcludes(childTable, byName, excludes);
            }
        }
    }

    /// <summary>
    /// Source SDK 2013 <c>GatherProps</c> / <c>GatherProps_IterateProps</c>. Non-collapsible
    /// child tables are flushed onto the output immediately; collapsible children are inlined
    /// into the current table's local list, which is appended afterwards.
    /// </summary>
    private static void Gather(
        SendTableDef table,
        Dictionary<string, SendTableDef> byName,
        HashSet<string> excludes,
        List<FlattenedProp> output)
    {
        var local = new List<FlattenedProp>();
        Iterate(table, byName, excludes, output, local);
        output.AddRange(local);
    }

    private static void Iterate(
        SendTableDef table,
        Dictionary<string, SendTableDef> byName,
        HashSet<string> excludes,
        List<FlattenedProp> output,
        List<FlattenedProp> local)
    {
        for (var i = 0; i < table.Props.Count; i++)
        {
            var prop = table.Props[i];
            if ((prop.Flags & (SendPropFlags.Exclude | SendPropFlags.InsideArray)) != 0)
            {
                continue;
            }

            if (excludes.Contains($"{table.Name}.{prop.Name}"))
            {
                continue;
            }

            if (prop.Type == SendPropType.DataTable && prop.DataTableName is { } child
                && byName.TryGetValue(child, out var childTable))
            {
                if ((prop.Flags & SendPropFlags.Collapsible) != 0)
                {
                    Iterate(childTable, byName, excludes, output, local);
                }
                else
                {
                    Gather(childTable, byName, excludes, output);
                }

                continue;
            }

            SendPropDef? arrayElement = null;
            if (prop.Type == SendPropType.Array && i > 0)
            {
                arrayElement = table.Props[i - 1];
            }

            local.Add(new FlattenedProp(local.Count, table.Name, prop.Name, prop, arrayElement));
        }
    }

    private static List<FlattenedProp> SortByPriority(List<FlattenedProp> props)
    {
        var priorities = new SortedSet<int> { ChangesOftenPriority };
        foreach (var prop in props)
        {
            priorities.Add(Priority(prop.Prop));
        }

        var list = props.ToList();
        var start = 0;
        foreach (var priority in priorities)
        {
            while (true)
            {
                var current = start;
                while (current < list.Count)
                {
                    var flags = list[current].Prop.Flags;
                    var propPriority = Priority(list[current].Prop);
                    if (propPriority == priority
                        || (priority == ChangesOftenPriority && (flags & SendPropFlags.ChangesOften) != 0))
                    {
                        if (start != current)
                        {
                            (list[start], list[current]) = (list[current], list[start]);
                        }

                        start++;
                        break;
                    }

                    current++;
                }

                if (current == list.Count)
                {
                    break;
                }
            }
        }

        return list;
    }

    private static int Priority(SendPropDef prop) =>
        (prop.Flags & SendPropFlags.ChangesOften) != 0 ? ChangesOftenPriority : DefaultPriority;
}
