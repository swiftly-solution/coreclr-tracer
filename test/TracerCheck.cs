using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;

public static unsafe class TracerCheck
{
    [StructLayout(LayoutKind.Sequential)]
    struct Ev { public ulong Ts; public uint Type; public uint Os; public ulong A, B; public uint Name, Source; }

    static byte[]? Sink;
    static nint tracer;
    static delegate* unmanaged[Cdecl]<nint, uint, byte*> getString;

    [MethodImpl(MethodImplOptions.NoInlining)]
    static void AllocMarker() { Sink = new byte[1234]; }

    [MethodImpl(MethodImplOptions.NoInlining)]
    static int MarkerCall() { AllocMarker(); return 1; }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    static void ReverseMarker(byte b) { MarkerCall(); }

    static delegate* unmanaged[Cdecl]<Ev*, nuint, nuint> snap;
    static Ev[] hbuf = new Ev[1 << 12];
    static int hn;
    static uint holdOs;
    static string holdName = "";
    static delegate* unmanaged[Cdecl]<nint, uint, byte*> gtn;
    static delegate* unmanaged[Cdecl]<nint, ulong, byte*> fnName, fnSrc;

    [MethodImpl(MethodImplOptions.NoInlining)]
    static void Hold()
    {
        fixed (Ev* p = hbuf) hn = (int)snap(p, (nuint)hbuf.Length);
        holdOs = hbuf.Take(hn).FirstOrDefault(e => e.Type == 0 && S(e.Name).Contains("TracerCheck.Hold")).Os;
        holdName = Marshal.PtrToStringUTF8((nint)gtn(tracer, holdOs)) ?? "";
    }

    static Ev[] nbuf = new Ev[1 << 16];
    static int nn;

    [MethodImpl(MethodImplOptions.NoInlining)]
    static int Nest(int d)
    {
        if (d == 0) { fixed (Ev* p = nbuf) nn = (int)snap(p, (nuint)nbuf.Length); return 0; }
        return Nest(d - 1) + 1;
    }

    static string S(uint id) => Marshal.PtrToStringUTF8((nint)getString(tracer, id)) ?? "";

    [MethodImpl(MethodImplOptions.NoInlining)]
    static int Leaf(int x) => x + 1;

    static Ev[] fbuf = new Ev[1 << 16];
    static int fn_;

    [MethodImpl(MethodImplOptions.NoInlining)]
    static void Flood()
    {
        int s = 0;
        for (int i = 0; i < 40000; i++) s += Leaf(i);
        fixed (Ev* p = fbuf) fn_ = (int)snap(p, (nuint)fbuf.Length) + (s & 0);
    }

    static ulong unloadAddr;
    static string unloadName = "";

    public static int DynSnap()
    {
        fixed (Ev* p = fbuf) fn_ = (int)snap(p, (nuint)fbuf.Length);
        var e = fbuf.Take(fn_).FirstOrDefault(e => e.Type == 0 && S(e.Name).Contains("UnloadMe"));
        unloadAddr = e.B;
        unloadName = S(e.Name);
        return 0;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    static WeakReference LoadAndRunCollectible()
    {
        var path = Path.Combine(Path.GetTempPath(), $"UnloadMe{Environment.ProcessId}.dll");
        var ab = new PersistedAssemblyBuilder(new AssemblyName("UnloadMe"), typeof(object).Assembly);
        var tb = ab.DefineDynamicModule("UnloadMe").DefineType("UnloadMe", TypeAttributes.Public | TypeAttributes.Abstract | TypeAttributes.Sealed);
        var il = tb.DefineMethod("Go", MethodAttributes.Public | MethodAttributes.Static, typeof(int), Type.EmptyTypes).GetILGenerator();
        il.Emit(OpCodes.Call, typeof(TracerCheck).GetMethod(nameof(DynSnap))!);
        il.Emit(OpCodes.Ldc_I4_1);        il.Emit(OpCodes.Add);
        il.Emit(OpCodes.Ret);
        tb.CreateType();
        ab.Save(path);

        var ctx = new AssemblyLoadContext("unload-check", isCollectible: true);
        ctx.LoadFromAssemblyPath(path).GetType("UnloadMe")!.GetMethod("Go")!.Invoke(null, null);
        ctx.Unload();
        return new WeakReference(ctx);
    }

    public static int Run()
    {
        var path = Environment.GetEnvironmentVariable("CORECLR_PROFILER_PATH");
        if (string.IsNullOrEmpty(path)) return 0;
        var lib = NativeLibrary.Load(Path.GetFullPath(path));
        tracer = ((delegate* unmanaged[Cdecl]<nint>)NativeLibrary.GetExport(lib, "GetTracer"))();
        nint* vt = *(nint**)tracer;
        getString = (delegate* unmanaged[Cdecl]<nint, uint, byte*>)vt[0];
        var snapshot = (delegate* unmanaged[Cdecl]<Ev*, nuint, nuint>)NativeLibrary.GetExport(lib, "TracerCrashSnapshot");
        snap = snapshot;
        gtn = (delegate* unmanaged[Cdecl]<nint, uint, byte*>)vt[7];
        fnName = (delegate* unmanaged[Cdecl]<nint, ulong, byte*>)vt[10];
        fnSrc = (delegate* unmanaged[Cdecl]<nint, ulong, byte*>)vt[11];

        var th = new Thread(() => { MarkerCall(); Hold(); }) { Name = "tracer-check" };
        th.Start();
        th.Join();

        var begin = (delegate* unmanaged[Cdecl]<nint, byte*, byte*, void>)vt[5];
        var end = (delegate* unmanaged[Cdecl]<nint, byte*, void>)vt[6];
        var cname = Marshal.StringToCoTaskMemUTF8("custom-check");
        var csrc = Marshal.StringToCoTaskMemUTF8("custom-src");
        begin(tracer, (byte*)cname, (byte*)csrc);
        Thread.Sleep(20);
        end(tracer, (byte*)cname);
        Marshal.FreeCoTaskMem(cname);
        Marshal.FreeCoTaskMem(csrc);

        var rev = (delegate* unmanaged[Cdecl]<byte, void>)&ReverseMarker;
        for (int i = 0; i < 100; i++) rev(1);

        var buf = new Ev[1 << 16];
        int n = 0;
        new List<int> { 1 }.ForEach(_ => { fixed (Ev* p = buf) n = (int)snapshot(p, (nuint)buf.Length); });
        var evs = buf.Take(n).ToList();

        var counts = new int[12];
        foreach (var e in evs) if (e.Type < 12) counts[e.Type]++;
        Console.WriteLine($"TracerCheck: events={n} enter={counts[0]} u2m={counts[6]}");

        var fn = evs.FirstOrDefault(e => e.Type == 0 && S(e.Name).Contains("TracerCheck.Run"));
        Console.WriteLine($"  open  : '{S(fn.Name)}' addr=0x{fn.B:x} module='{S(fn.Source)}'");

        int bad = 0;
        if (fn.A == 0 || fn.B == 0 || S(fn.Source).Length == 0) bad++;
        if (evs.Any(e => e.Type != 0 && e.Type != 6)) bad++;
        double upNs = (DateTime.Now - System.Diagnostics.Process.GetCurrentProcess().StartTime).TotalMilliseconds * 1e6;
        Console.WriteLine($"  ts    : {fn.Ts} ns (uptime {upNs:F0} ns)");
        if (fn.Ts == 0 || fn.Ts > upNs) bad++;
        foreach (var e in evs.Where(e => e.Type == 0 && S(e.Name).StartsWith("System.")))
        {
            Console.WriteLine($"  system hooked: '{S(e.Name)}'");
            bad++;
        }
        foreach (var e in evs.Where(e => S(e.Name).Contains("Marker")))
        {
            Console.WriteLine($"  leaked: type={e.Type} '{S(e.Name)}'");
            bad++;
        }
        var setObj = (delegate* unmanaged[Cdecl]<nint, byte, byte>)vt[8];
        var isObj = (delegate* unmanaged[Cdecl]<nint, byte>)vt[9];
        bool offDefault = isObj(tracer) == 0;
        bool onOk = setObj(tracer, 1) != 0 && isObj(tracer) != 0;
        for (int i = 0; i < 1000; i++) AllocMarker();
        bool offOk = setObj(tracer, 0) != 0 && isObj(tracer) == 0;
        Console.WriteLine($"  alloc : default_off={offDefault} on={onOk} off={offOk}");
        if (!offDefault || !onOk || !offOk) bad++;

        Nest(64);
        var nts = nbuf.Take(nn).Where(e => e.Type == 0 && S(e.Name).Contains("TracerCheck.Nest")).Select(e => e.Ts).ToList();
        int ties = 0, backwards = 0;
        ulong minD = ulong.MaxValue, maxD = 0;
        for (int i = 1; i < nts.Count; i++)
        {
            if (nts[i] < nts[i - 1]) { backwards++; continue; }
            if (nts[i] == nts[i - 1]) { ties++; continue; }
            var d = nts[i] - nts[i - 1];
            minD = Math.Min(minD, d); maxD = Math.Max(maxD, d);
        }
        Console.WriteLine($"  nest  : frames={nts.Count} ties={ties} min_delta={minD} ns max_delta={maxD} ns");
        if (nts.Count != 65 || backwards > 0 || ties > 0) bad++;

        var nestEv = nbuf.Take(nn).FirstOrDefault(e => e.Type == 0 && S(e.Name).Contains("TracerCheck.Nest"));
        string nameAt = Marshal.PtrToStringUTF8((nint)fnName(tracer, nestEv.B + 1)) ?? "";
        string srcAt = Marshal.PtrToStringUTF8((nint)fnSrc(tracer, nestEv.B)) ?? "";
        string nameNone = Marshal.PtrToStringUTF8((nint)fnName(tracer, 1)) ?? "?";
        Console.WriteLine($"  addr  : Nest code=0x{nestEv.B:x} name='{nameAt}' source='{srcAt}' unknown='{nameNone}'");
        if (nestEv.B == 0 || nameAt != S(nestEv.Name) || srcAt != S(nestEv.Source) || nameNone != "") bad++;

        Flood();
        bool floodKept = fbuf.Take(fn_).Any(e => e.Type == 0 && S(e.Name).Contains("TracerCheck.Flood"))
                         && fbuf.Take(fn_).Any(e => e.Type == 0 && S(e.Name).Contains("TracerCheck.Run"));
        Console.WriteLine($"  flood : outer frames kept after 40000 hooked calls={floodKept}");
        if (!floodKept) bad++;

        var alc = LoadAndRunCollectible();
        string before = Marshal.PtrToStringUTF8((nint)fnName(tracer, unloadAddr + 1)) ?? "";
        bool resolved = unloadAddr != 0 && before == unloadName && before.Contains("UnloadMe");
        bool gone = false;
        for (int i = 0; i < 100 && !gone; i++)
        {
            GC.Collect(); GC.WaitForPendingFinalizers(); Thread.Sleep(10);
            gone = !alc.IsAlive && (Marshal.PtrToStringUTF8((nint)fnName(tracer, unloadAddr + 1)) ?? "") == "";
        }
        Console.WriteLine($"  unload: resolved='{before}' forgotten_after_unload={gone} contextAlive={alc.IsAlive}");
        if (!resolved || !gone) bad++;
        Console.WriteLine($"  thread: os={holdOs} name='{holdName}'");
        if (holdName != "tracer-check") bad++;
        Console.WriteLine($"TracerCheck: bad={bad}");
        return bad;
    }
}
