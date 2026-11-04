namespace GmodDemo.Parser;

public static class BitMath
{
    /// <summary>Floor(log2(value)) for value &gt; 0; 0 when value is 0. Matches Valve Q_log2.</summary>
    public static int Log2(int value)
    {
        var result = 0;
        while (value > 1)
        {
            value >>= 1;
            result++;
        }

        return result;
    }
}
