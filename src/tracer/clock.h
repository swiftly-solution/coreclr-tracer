#ifndef _src_tracer_clock_h
#define _src_tracer_clock_h

#include <chrono>
#include <cstdint>

#ifdef _WIN32
#include <intrin.h>
#endif

const auto g_start = std::chrono::steady_clock::now();

static uint64_t ChronoNs()
{
    auto elapsed = std::chrono::steady_clock::now() - g_start;
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
}

#ifdef _WIN32

static bool HasInvariantTsc()
{
    int regs[4];
    __cpuid(regs, static_cast<int>(0x80000000));
    if (static_cast<unsigned>(regs[0]) < 0x80000007u)
    {
        return false;
    }
    __cpuid(regs, static_cast<int>(0x80000007));
    return (regs[3] >> 8) & 1;
}

const bool g_useTsc = HasInvariantTsc();

static uint64_t Ticks()
{
    return g_useTsc ? __rdtsc() : ChronoNs();
}

struct Anchor
{
    uint64_t tsc;
    uint64_t ns;
};

static Anchor SampleAnchor()
{
    uint64_t before = __rdtsc();
    uint64_t ns = ChronoNs();
    uint64_t after = __rdtsc();
    return Anchor{before + (after - before) / 2, ns};
}

const Anchor g_loadAnchor = g_useTsc ? SampleAnchor() : Anchor{};

struct Calib
{
    Anchor now;

    uint64_t ToNs(uint64_t ticks) const
    {
        if (!g_useTsc)
        {
            return ticks;
        }
        if (ticks <= g_loadAnchor.tsc || now.tsc <= g_loadAnchor.tsc || now.ns <= g_loadAnchor.ns)
        {
            return g_loadAnchor.ns;
        }
        uint64_t high;
        uint64_t remainder;
        uint64_t low = _umul128(ticks - g_loadAnchor.tsc, now.ns - g_loadAnchor.ns, &high);
        return g_loadAnchor.ns + _udiv128(high, low, now.tsc - g_loadAnchor.tsc, &remainder);
    }
};

static Calib MakeCalib()
{
    return Calib{g_useTsc ? SampleAnchor() : Anchor{}};
}

#else

static uint64_t Ticks()
{
    return ChronoNs();
}

struct Calib
{
    uint64_t ToNs(uint64_t ticks) const
    {
        return ticks;
    }
};

static Calib MakeCalib()
{
    return {};
}

#endif

#endif
