using System.Collections.Concurrent;
using System.Collections.Immutable;
using System.Reflection;
using System.Reflection.Emit;
using Cases;

public static unsafe class Entrypoint
{
    private static readonly List<object> Keep = new();
    private static object? Sink;

    public static void Main(string[] args)
    {
        Keep.Add(new Plain());
        Keep.Add(new Plain.Nested());
        Keep.Add(new Plain.Nested.Deeper());
        Keep.Add(new Plain.GenNested<int>());

        Keep.Add(new Outer<int>());
        Keep.Add(new Outer<int>.Inner());
        Keep.Add(new Outer<int>.Inner2<string>());
        Keep.Add(new Outer<int>.Mid<string>());
        Keep.Add(new Outer<int>.Mid<string>.Deep<double>());
        Keep.Add(new Outer<int>.Mid<string>.DeepPlain());
        Keep.Add(new Outer<string>.Inner2<object>());
        Keep.Add(new Outer2<int, string>.Inner());
        Keep.Add(new Outer2<int, string>.Inner2<double>());
        Keep.Add(new Outer2<int, string>.Inner3<double, char>());
        Keep.Add(new Constrained<Plain>());

        Keep.Add(new Gen3<int, string, Plain>());
        Keep.Add(new Gen1<Gen1<Gen1<Gen1<int>>>>());
        Keep.Add(new Gen1<int[][]>());
        Keep.Add(new Gen1<int[,]>());
        Keep.Add(new Gen1<Outer<int>.Mid<string>.Deep<Plain.Nested>>());
        Keep.Add(new Outer<Outer<int>.Inner2<string>>.Inner2<List<int>>());
        Keep.Add(new List<int[]>());
        Keep.Add(new Dictionary<string, List<int>[]>());
        Keep.Add(new Dictionary<int, Dictionary<string, List<int[]>>>());

        Keep.Add(new GlobalType());
        Keep.Add(new GlobalType.Nested());
        Keep.Add(new GlobalGen<int>());
        Keep.Add(new GlobalGen<int>.Nested<string>());

        Keep.Add(new Ünïcödé());
        Keep.Add(new Ünïcödé.Ñested());
        Keep.Add(new Ünïcödé[2]);

        Keep.Add(new Rec(1));
        Keep.Add(Activator.CreateInstance(typeof(Color))!);
        Keep.Add(Activator.CreateInstance(typeof(HasEnum<int>.E))!);
        Keep.Add(new Impl());
        Keep.Add(new IFoo<int>[2]);
        Keep.Add(new HasEnum<string>.I[2]);
        Keep.Add(new Action(() => { }));
        Keep.Add(new Func<int, string>(i => ""));
        Keep.Add(new CustomDelegate(s => 0));
        Keep.Add(new GenDelegate<string>(s => s));
        Keep.Add(new HasEnum<int>.D(x => x));

        Keep.Add(Activator.CreateInstance(typeof(Vt<int>))!);
        Keep.Add(Activator.CreateInstance(typeof(Outer<int>.VtIn<string>))!);
        Keep.Add(Activator.CreateInstance(typeof(ValueTuple<int, string>))!);
        Keep.Add(Activator.CreateInstance(typeof(PlainStruct))!);

        Keep.Add(new ConcurrentDictionary<string, int>());
        Keep.Add(new HashSet<Plain>());
        Keep.Add(new SortedDictionary<Plain, Vt<int>>());
        Keep.Add(new Stack<(int, string)>());
        Keep.Add(new Lazy<List<string>>());
        Keep.Add(new TaskCompletionSource<Plain>());
        Keep.Add(ImmutableArray.CreateBuilder<int>());
        Keep.Add(ImmutableList.CreateBuilder<Plain>());

        Keep.Add(Closures.Capture(1));
        Keep.Add(Closures.CaptureGen(1));
        Keep.Add(Closures.CaptureGen("s"));
        Keep.Add(Closures.NoCapture());
        Keep.Add(Closures.NoCaptureGen<int>());
        Keep.Add(Closures.Iter());
        Keep.Add(Closures.IterGen("s"));
        Closures.AsyncRun().GetAwaiter().GetResult();
        Keep.Add(new { A = 1, B = "x" });

        Keep.Add(new bool[2]);
        Keep.Add(new char[2]);
        Keep.Add(new sbyte[2]);
        Keep.Add(new byte[2]);
        Keep.Add(new short[2]);
        Keep.Add(new ushort[2]);
        Keep.Add(new int[2]);
        Keep.Add(new uint[2]);
        Keep.Add(new long[2]);
        Keep.Add(new ulong[2]);
        Keep.Add(new float[2]);
        Keep.Add(new double[2]);
        Keep.Add(new nint[2]);
        Keep.Add(new nuint[2]);
        Keep.Add(new string[2]);
        Keep.Add(new object[2]);
        Keep.Add(new decimal[2]);
        Keep.Add(new Guid[2]);
        Keep.Add(new DateTime[2]);
        Keep.Add(new Array[2]);
        Keep.Add(new Enum[2]);
        Keep.Add(new ValueType[2]);
        Keep.Add(new IDisposable[2]);

        Keep.Add(new Color[2]);
        Keep.Add(new HasEnum<int>.E[2]);
        Keep.Add(new Plain.Nested[2]);
        Keep.Add(new List<int>[2]);
        Keep.Add(new Vt<int>[2]);
        Keep.Add(new int?[2]);
        Keep.Add(new (int, string)[2]);
        Keep.Add(new Action[2]);
        Keep.Add(new Func<int, int>[2]);
        Keep.Add(new CustomDelegate[2]);
        Keep.Add(new Outer<int>.Inner2<string>[2]);
        Keep.Add(new Outer<int>.Mid<string>.Deep<double>[2][]);
        Keep.Add(new GlobalGen<int>.Nested<string>[2]);

        Keep.Add(new int[2][]);
        Keep.Add(new int[2][][]);
        Keep.Add(new int[2, 2]);
        Keep.Add(new int[2, 2, 2]);
        Keep.Add(new int[1, 1, 1, 1]);
        Keep.Add(new int[2][,]);
        Keep.Add(new int[2, 2][]);
        Keep.Add(new int[2][,][]);
        Keep.Add(new string[2][]);
        Keep.Add(new List<int[]>[2][]);

        Keep.Add(new int*[2]);
        Keep.Add(new void*[2]);
        Keep.Add(new int**[2]);
        Keep.Add(new delegate*<int, void>[2]);

        var asm = AssemblyBuilder.DefineDynamicAssembly(new AssemblyName("DynAsm"), AssemblyBuilderAccess.Run);
        var mod = asm.DefineDynamicModule("DynMod");
        var dynType = mod.DefineType("DynNs.DynType", TypeAttributes.Public).CreateType();
        Keep.Add(Activator.CreateInstance(dynType)!);
        var genBuilder = mod.DefineType("DynNs.DynGen`1", TypeAttributes.Public);
        genBuilder.DefineGenericParameters("T");
        var dynGen = genBuilder.CreateType().MakeGenericType(typeof(int));
        Keep.Add(Activator.CreateInstance(dynGen)!);
        Keep.Add(Activator.CreateInstance(typeof(List<>).MakeGenericType(dynType))!);

        var threads = Enumerable.Range(0, 8).Select(_ => new Thread(() =>
        {
            for (int i = 0; i < 2000; i++)
            {
                Sink = new Gen3<byte, sbyte, short>();
            }
        })).ToList();
        threads.ForEach(t => t.Start());
        threads.ForEach(t => t.Join());

        var host = new FuncHost<int>();
        host.Ret();
        host.RefRet(new int[1]);
        host.Prop = host.Prop + 1;
        double c = 0;
        FuncHost<int>.Many(1, "b", ref c, out _, Guid.Empty, new int[1], new int[1, 1],
                           new List<int>(), new Dictionary<string, int[]>(), 1);
        host.Inst("u", 1, new Outer<int>.Inner2<string>());
        FuncHost<int>.Conv(1, (int x) => x.ToString());
        host.Ptr(null, null, null);
        host.Sp(default, default, null!, null!, (1, 1), null);
        VarArgHost.Va(1, __arglist());
        Sink = host + host;
        new FuncHost<int>.Nest<string>().M("u", 1);

        Console.WriteLine("Hello, World!");
        GC.KeepAlive(Keep);
    }
}
