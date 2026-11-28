#ifndef _src_tracer_varint_h
#define _src_tracer_varint_h

#include <cstddef>
#include <cstdint>

constexpr unsigned kVarintBitsPerByte = 7;
constexpr uint8_t kVarintValueMask = (1u << kVarintBitsPerByte) - 1;
constexpr uint8_t kVarintContinueFlag = 1u << kVarintBitsPerByte;
constexpr size_t kVarintMaxBytes32 = 5;
constexpr size_t kVarintMaxBytes64 = 10;

inline uint8_t* WriteVarint(uint8_t* out, uint64_t value)
{
    while (value > kVarintValueMask)
    {
        *out++ = static_cast<uint8_t>(value & kVarintValueMask) | kVarintContinueFlag;
        value >>= kVarintBitsPerByte;
    }
    *out++ = static_cast<uint8_t>(value);
    return out;
}

inline const uint8_t* ReadVarint(const uint8_t* in, uint64_t& value)
{
    value = 0;
    for (unsigned shift = 0; shift < 64; shift += kVarintBitsPerByte)
    {
        uint8_t byte = *in++;
        value |= static_cast<uint64_t>(byte & kVarintValueMask) << shift;
        if ((byte & kVarintContinueFlag) == 0)
        {
            break;
        }
    }
    return in;
}

#endif
