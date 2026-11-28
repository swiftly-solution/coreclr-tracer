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

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    static void ReverseMarker(byte b) { MarkerCall(); }

    static delegate* unmanaged[Cdecl]<Ev*, nuint, nuint> snap;
    static Ev[] hbuf = new Ev[1 << 12];
    static int hn;
    static uint holdOs;
    static string holdName = "";
    static delegate* unmanaged[Cdecl]<nint, uint, byte*> gtn;

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
    static int ProfWork(int x) => x + 1;

    record struct Span(uint Name, uint Category, uint Os, ulong Start, ulong Duration, uint Source, bool Open);

    class Profile
    {
        public uint Version;
        public List<string> Strings = new();
        public List<Span> Spans = new();
        public Dictionary<uint, uint> Threads = new();
        public int Blocks;
    }

    static MemoryStream profileBytes = new();
    static int profileBlocks;

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    static byte CollectBlock(void* data, nuint size, void* context)
    {
        profileBytes.Write(new ReadOnlySpan<byte>(data, checked((int)size)));
        profileBlocks++;
        return 1;
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    static byte RejectBlock(void* data, nuint size, void* context) => 0;

    static delegate* unmanaged[Cdecl]<nint, delegate* unmanaged[Cdecl]<void*, nuint, void*, byte>, void*, byte> writeProfile;

    static byte[] CaptureProfile(out bool ok)
    {
        profileBytes = new MemoryStream();
        profileBlocks = 0;
        ok = writeProfile(tracer, &CollectBlock, null) != 0;
        return profileBytes.ToArray();
    }

    static ulong ReadVarint(byte[] buf, ref int pos)
    {
        ulong value = 0;
        for (int shift = 0; ; shift += 7)
        {
            byte b = buf[pos++];
            value |= (ulong)(b & 0x7F) << shift;
            if ((b & 0x80) == 0) return value;
        }
    }

    static IEnumerable<(int Field, ulong Value, int Start, int Length)> Fields(byte[] buf, int pos, int end)
    {
        while (pos < end)
        {
            ulong tag = ReadVarint(buf, ref pos);
            int field = (int)(tag >> 3), wire = (int)(tag & 7);
            if (wire == 0) { yield return (field, ReadVarint(buf, ref pos), 0, 0); continue; }
            if (wire != 2) throw new InvalidDataException($"unexpected wire type {wire}");
            int len = (int)ReadVarint(buf, ref pos);
            yield return (field, 0, pos, len);
            pos += len;
        }
        if (pos != end) throw new InvalidDataException("field overruns message");
    }

    static Profile ParseProfile(byte[] buf)
    {
        var p = new Profile { Blocks = profileBlocks };
        foreach (var f in Fields(buf, 0, buf.Length))
        {
            switch (f.Field)
            {
                case 1: p.Version = (uint)f.Value; break;
                case 2: p.Strings.Add(System.Text.Encoding.UTF8.GetString(buf, f.Start, f.Length)); break;
                case 3:
                    var s = new Span();
                    foreach (var g in Fields(buf, f.Start, f.Start + f.Length))
                    {
                        switch (g.Field)
                        {
                            case 1: s.Name = (uint)g.Value; break;
                            case 2: s.Category = (uint)g.Value; break;
                            case 3: s.Os = (uint)g.Value; break;
                            case 4: s.Start = g.Value; break;
                            case 5: s.Duration = g.Value; break;
                            case 6: s.Source = (uint)g.Value; break;
                            case 7: s.Open = g.Value != 0; break;
                        }
                    }
                    p.Spans.Add(s);
                    break;
                case 5:
                    uint os = 0, name = 0;
                    foreach (var g in Fields(buf, f.Start, f.Start + f.Length))
                    {
                        if (g.Field == 1) os = (uint)g.Value;
                        if (g.Field == 2) name = (uint)g.Value;
                    }
                    p.Threads[os] = name;
                    break;
            }
        }
        return p;
    }

    static string Str(Profile p, uint id) => id < p.Strings.Count ? p.Strings[(int)id] : $"<bad string {id}>";

    static byte[] savedInside = Array.Empty<byte>();
    static bool savedInsideOk;

    [MethodImpl(MethodImplOptions.NoInlining)]
    static void SaveInside() => savedInside = CaptureProfile(out savedInsideOk);

    static int ProfileCheck(nint* vt)
    {
        const int threadCount = 4, calls = 250_000;
        var start = (delegate* unmanaged[Cdecl]<nint, void>)vt[2];
        writeProfile = (delegate* unmanaged[Cdecl]<nint, delegate* unmanaged[Cdecl]<void*, nuint, void*, byte>, void*, byte>)vt[3];
        var stop = (delegate* unmanaged[Cdecl]<nint, void>)vt[4];
        int bad = 0;

        GC.Collect();
        long before = System.Diagnostics.Process.GetCurrentProcess().PrivateMemorySize64;
        start(tracer);
        var threads = Enumerable.Range(0, threadCount).Select(_ => new Thread(() =>
        {
            int v = 0;
            for (int i = 0; i < calls; i++) v = ProfWork(v);
        })).ToList();
        threads.ForEach(t => t.Start());
        threads.ForEach(t => t.Join());
        long after = System.Diagnostics.Process.GetCurrentProcess().PrivateMemorySize64;

        var saver = new Thread(SaveInside) { Name = "prof-saver" };
        saver.Start();
        saver.Join();
        var profile = ParseProfile(savedInside);

        var work = profile.Spans.Where(s => Str(profile, s.Name).EndsWith("TracerCheck.ProfWork")).ToList();
        int workThreads = work.Select(s => s.Os).Distinct().Count();
        int backwards = 0;
        foreach (var g in work.GroupBy(s => s.Os))
        {
            ulong last = 0;
            foreach (var s in g) { if (s.Start < last) backwards++; last = s.Start; }
        }
        bool fieldsOk = work.All(s => !s.Open && s.Category == 0 && s.Start != 0 && Str(profile, s.Source).Length > 0);
        var open = profile.Spans.FirstOrDefault(s => s.Open && Str(profile, s.Name).EndsWith("TracerCheck.SaveInside"));
        string saverName = profile.Threads.TryGetValue(open.Os, out var nameId) ? Str(profile, nameId) : "";
        bool stringsOk = profile.Strings.Count > 0 && profile.Strings[0] == "" && profile.Spans.All(s => s.Name < profile.Strings.Count && s.Source < profile.Strings.Count);

        Console.WriteLine($"  profile: ok={savedInsideOk} bytes={savedInside.Length} blocks={profile.Blocks} version={profile.Version} strings={profile.Strings.Count} spans={profile.Spans.Count}");
        Console.WriteLine($"  profile: work={work.Count} threads={workThreads} backwards={backwards} fields={fieldsOk} open='{Str(profile, open.Name)}' open_thread='{saverName}'");
        Console.WriteLine($"  profile: private memory +{(after - before) / 1024.0 / 1024.0:F1} MiB for {2 * threadCount * calls} events");
        if (!savedInsideOk || profile.Version != 1 || work.Count != threadCount * calls || workThreads != threadCount || backwards > 0 || !fieldsOk || !stringsOk) bad++;
        if (!open.Open || saverName != "prof-saver") bad++;

        if (writeProfile(tracer, &RejectBlock, null) != 0) bad++;

        stop(tracer);
        var afterStop = ParseProfile(CaptureProfile(out _));
        start(tracer);
        ProfWork(1);
        var restarted = ParseProfile(CaptureProfile(out _));
        stop(tracer);
        int restartWork = restarted.Spans.Count(s => Str(restarted, s.Name).EndsWith("TracerCheck.ProfWork"));
        Console.WriteLine($"  profile: after stop spans={afterStop.Spans.Count} restart work spans={restartWork}");
        if (afterStop.Spans.Count != 0 || restartWork != 1) bad++;
        return bad;
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
        Console.WriteLine($"  thread: os={holdOs} name='{holdName}'");
        if (holdName != "tracer-check") bad++;
        bad += ProfileCheck(vt);
        Console.WriteLine($"TracerCheck: bad={bad}");
        return bad;
    }
}
