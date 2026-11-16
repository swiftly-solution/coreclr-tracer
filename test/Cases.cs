namespace Cases;

public class Plain
{
    public class Nested
    {
        public class Deeper { }
    }

    public class GenNested<T> { }
}

public class Outer<T>
{
    public class Inner { }
    public class Inner2<U> { }
    public struct VtIn<U> { }

    public class Mid<U>
    {
        public class Deep<V> { }
        public class DeepPlain { }
    }
}

public struct Vt<T> { }
public struct PlainStruct { public int X; }

public class Outer2<A, B>
{
    public class Inner { }
    public class Inner2<C> { }
    public class Inner3<C, D> { }
}

public class Gen1<A> { }
public class Gen3<A, B, C> { }
public class Constrained<T> where T : class, new() { }

public enum Color { Red }

public class HasEnum<T>
{
    public enum E { X }
    public interface I { }
    public delegate T D(T x);
}

public delegate int CustomDelegate(string s);
public delegate T GenDelegate<T>(T x);

public interface IFoo<T> { }
public class Impl : IFoo<int> { }

public record Rec(int X);

public class Ünïcödé
{
    public class Ñested { }
}

public unsafe class FuncHost<T>
{
    public FuncHost() { }
    static FuncHost() { }

    public T Ret() => default!;
    public ref int RefRet(int[] a) => ref a[0];
    public int Prop { get; set; }

    public static int Many(int a, string b, ref double c, out long d, in Guid e, int[] f, int[,] g,
                           List<T> h, Dictionary<string, T[]> i, T j)
    { d = 0; return 0; }

    public void Inst<U>(U u, T t, Outer<T>.Inner2<U> n) { }

    public static TOut Conv<TIn, TOut>(TIn input, Func<TIn, TOut> f) => f(input);

    public void Ptr(int* p, delegate*<int, string, void> fp, delegate* unmanaged[Cdecl]<void*, int> fp2) { }

    public void Sp(Span<T> s, ReadOnlySpan<char> r, T[][] jag, T[,] md, (int a, T b) tup, int? n) { }

    public static FuncHost<T> operator +(FuncHost<T> a, FuncHost<T> b) => a;

    public class Nest<U>
    {
        public void M(U u, T t) { }
    }
}

public static class VarArgHost
{
    public static void Va(int a, __arglist) { }
}

public static class Closures
{
    public static object Capture(int x)
    {
        Func<int> f = () => x + 1;
        return f.Target!;
    }

    public static object CaptureGen<T>(T x)
    {
        Func<T> f = () => x;
        return f.Target!;
    }

    public static Func<int, int> NoCapture() => x => x * 2;
    public static Func<T, T> NoCaptureGen<T>() => x => x;

    public static IEnumerable<int> Iter() { yield return 1; }
    public static IEnumerable<T> IterGen<T>(T t) { yield return t; }

    public static async Task<int> AsyncRun()
    {
        await Task.Yield();
        return 1;
    }
}
