using System.Runtime.CompilerServices;

public static class ArgStress
{
    [MethodImpl(MethodImplOptions.NoInlining)]
    static double Mix(int a, double b, long c, float d, int e, double f, short g, double h, byte i, float j)
        => a + b + c + d + e + f + g + h + i + j;

    [MethodImpl(MethodImplOptions.NoInlining)]
    static double RetD(double x) => x * 2.5;

    [MethodImpl(MethodImplOptions.NoInlining)]
    static float RetF(float x) => x + 0.5f;

    [MethodImpl(MethodImplOptions.NoInlining)]
    static long RetL(long x) => x * 3;

    [MethodImpl(MethodImplOptions.NoInlining)]
    static (long, long) RetPair(long x) => (x, x + 1);

    [MethodImpl(MethodImplOptions.NoInlining)]
    static int Fib(int n) => n < 2 ? n : Fib(n - 1) + Fib(n - 2);

    static int Work()
    {
        int bad = 0;
        for (int i = 0; i < 50000; i++)
        {
            short s = (short)(i & 0x7fff);
            byte by = (byte)i;
            double exp = i + 1.5 + i * 2L + 0.25f + (i + 1) + 2.5 + s + 3.5 + by + 1.75f;
            if (Mix(i, 1.5, i * 2L, 0.25f, i + 1, 2.5, s, 3.5, by, 1.75f) != exp) bad++;
            if (RetD(i + 0.125) != (i + 0.125) * 2.5) bad++;
            if (RetF(i * 0.5f) != i * 0.5f + 0.5f) bad++;
            if (RetL(i) != i * 3L) bad++;
            var (p, q) = RetPair(i);
            if (p != i || q != i + 1L) bad++;
        }
        if (Fib(18) != 2584) bad++;
        return bad;
    }

    public static int Run()
    {
        int bad = 0;
        var threads = Enumerable.Range(0, 4).Select(_ => new Thread(() => Interlocked.Add(ref bad, Work()))).ToList();
        threads.ForEach(t => t.Start());
        threads.ForEach(t => t.Join());
        Console.WriteLine($"ArgStress: bad={bad}");
        return bad;
    }
}
