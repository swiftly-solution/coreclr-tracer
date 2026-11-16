using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

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

    static string S(uint id) => Marshal.PtrToStringUTF8((nint)getString(tracer, id)) ?? "";

    public static int Run()
    {
        var path = Environment.GetEnvironmentVariable("CORECLR_PROFILER_PATH");
        if (string.IsNullOrEmpty(path)) return 0;
        var lib = NativeLibrary.Load(Path.GetFullPath(path));
        tracer = ((delegate* unmanaged[Cdecl]<nint>)NativeLibrary.GetExport(lib, "GetTracer"))();
        nint* vt = *(nint**)tracer;
        getString = (delegate* unmanaged[Cdecl]<nint, uint, byte*>)vt[0];
        var snapshot = (delegate* unmanaged[Cdecl]<Ev*, nuint, nuint>)NativeLibrary.GetExport(lib, "TracerCrashSnapshot");

        var th = new Thread(() => MarkerCall()) { Name = "tracer-check" };
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

        var buf = new Ev[1 << 16];
        int n;
        fixed (Ev* p = buf) n = (int)snapshot(p, (nuint)buf.Length);
        var evs = buf.Take(n).ToList();

        var counts = new int[11];
        foreach (var e in evs) if (e.Type < 11) counts[e.Type]++;
        Console.WriteLine($"TracerCheck: events={n} enter={counts[0]} exit={counts[1]} tail={counts[2]} " +
                          $"tcreate={counts[3]} tdestroy={counts[4]} tassign={counts[5]} u2m={counts[6]} " +
                          $"alloc={counts[7]} tname={counts[8]} cbegin={counts[9]} cend={counts[10]}");

        var fn = evs.FirstOrDefault(e => e.Type == 0 && S(e.Name).Contains("MarkerCall"));
        var alloc = evs.FirstOrDefault(e => e.Type == 7 && e.B >= 1234 && S(e.Source).Contains("AllocMarker"));
        var tname = evs.FirstOrDefault(e => e.Type == 8 && S(e.Name) == "tracer-check");
        var tassign = evs.FirstOrDefault(e => e.Type == 5 && S(e.Name) == "tracer-check");
        Console.WriteLine($"  enter : '{S(fn.Name)}' addr=0x{fn.B:x} module='{S(fn.Source)}'");
        Console.WriteLine($"  alloc : type='{S(alloc.Name)}' size={alloc.B} source='{S(alloc.Source)}'");
        Console.WriteLine($"  thread: name='{S(tname.Name)}' os={tname.B} assigned-with-name={tassign.A != 0}");

        var cb = evs.FirstOrDefault(e => e.Type == 9 && S(e.Name) == "custom-check");
        var ce = evs.FirstOrDefault(e => e.Type == 10 && S(e.Name) == "custom-check");
        Console.WriteLine($"  custom: source='{S(cb.Source)}' duration={(ce.Ts - cb.Ts) / 1e6:F1}ms same-thread={cb.Os == ce.Os}");

        int bad = 0;
        if (cb.Ts == 0 || ce.Ts < cb.Ts + 20_000_000 || cb.Os != ce.Os || S(cb.Source) != "custom-src") bad++;
        if (fn.A == 0 || fn.B == 0 || S(fn.Source).Length == 0) bad++;
        if (alloc.A == 0) bad++;
        if (tname.A == 0) bad++;
        Console.WriteLine($"TracerCheck: bad={bad}");
        return bad;
    }
}
