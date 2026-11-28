#ifndef _src_tracer_profile_buffer_h
#define _src_tracer_profile_buffer_h

#include "../shared/api.h"
#include "varint.h"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>

struct FnKey
{
    uint64_t a;
    uint64_t b;
    uint32_t nameId;
    uint32_t sourceId;

    bool operator==(const FnKey& other) const
    {
        return a == other.a && b == other.b && nameId == other.nameId && sourceId == other.sourceId;
    }
};

inline bool IsFunctionEvent(EventType type)
{
    return type == EventType::FunctionEnter || type == EventType::FunctionExit || type == EventType::FunctionTailcall || type == EventType::UnmanagedToManaged || type == EventType::ManagedToUnmanaged;
}

inline uint64_t ZigzagEncode(int64_t value)
{
    uint64_t signBits = static_cast<uint64_t>(value >> 63);
    return (static_cast<uint64_t>(value) << 1) ^ signBits;
}

inline int64_t ZigzagDecode(uint64_t value)
{
    uint64_t negativeMask = ~(value & 1) + 1;
    return static_cast<int64_t>((value >> 1) ^ negativeMask);
}

class ProfileBuffer
{
    struct Chunk;

public:
    static constexpr size_t kChunkBytes = 64 * 1024;

    class View
    {
    public:
        template <class ResolveFunction, class Emit> void ForEach(ResolveFunction&& resolveFunction, Emit&& emit) const
        {
            for (const Chunk* chunk = m_head; chunk != nullptr; chunk = chunk == m_tail ? nullptr : chunk->next)
            {
                const uint8_t* in = chunk->data;
                const uint8_t* end = in + (chunk == m_tail ? m_tailBytes : chunk->usedBytes);
                DecodeChunk(*chunk, in, end, resolveFunction, emit);
            }
        }

        uint64_t LastTimestamp() const
        {
            return m_lastTimestamp;
        }

        bool Empty() const
        {
            return m_head == nullptr;
        }

    private:
        friend class ProfileBuffer;

        const Chunk* m_head = nullptr;
        const Chunk* m_tail = nullptr;
        uint32_t m_tailBytes = 0;
        uint64_t m_lastTimestamp = 0;
    };

    std::mutex mutex;

    ProfileBuffer() = default;
    ProfileBuffer(const ProfileBuffer&) = delete;
    ProfileBuffer& operator=(const ProfileBuffer&) = delete;

    ~ProfileBuffer()
    {
        Clear();
    }

    void AppendFunction(uint64_t timestamp, EventType type, uint32_t threadId, uint32_t functionIndex)
    {
        uint8_t* out = BeginRecord(timestamp, threadId, type);
        if (out == nullptr)
        {
            return;
        }
        out = WriteVarint(out, functionIndex);
        EndRecord(out, timestamp);
    }

    void AppendEvent(const Event& e)
    {
        uint8_t* out = BeginRecord(e.timestampNs, e.osThreadId, e.type);
        if (out == nullptr)
        {
            return;
        }
        out = WriteVarint(out, e.a);
        out = WriteVarint(out, e.b);
        out = WriteVarint(out, e.nameId);
        out = WriteVarint(out, e.sourceId);
        EndRecord(out, e.timestampNs);
    }

    View Freeze() const
    {
        View view;
        view.m_head = m_head;
        view.m_tail = m_tail;
        view.m_tailBytes = m_tail != nullptr ? m_tail->usedBytes : 0;
        view.m_lastTimestamp = m_lastTimestamp;
        return view;
    }

    void Clear()
    {
        Chunk* chunk = m_head;
        while (chunk != nullptr)
        {
            Chunk* next = chunk->next;
            std::free(chunk);
            chunk = next;
        }
        m_head = nullptr;
        m_tail = nullptr;
        m_lastTimestamp = 0;
        m_lastThreadId = 0;
    }

private:
    static constexpr unsigned kTagBits = 4;
    static constexpr uint64_t kTagMask = (1u << kTagBits) - 1;
    static constexpr uint32_t kThreadSwitchTag = kTagMask;
    static constexpr size_t kChunkHeaderBytes = 24;

    static constexpr size_t kThreadSwitchRecordBytes = 1 + kVarintMaxBytes32;
    static constexpr size_t kLargestEventRecordBytes = kVarintMaxBytes64 * 3 + kVarintMaxBytes32 * 2;
    static constexpr size_t kMaxRecordBytes = kThreadSwitchRecordBytes + kLargestEventRecordBytes;

    static_assert(static_cast<uint32_t>(EventType::ManagedToUnmanaged) < kThreadSwitchTag, "EventType must fit in the tag bits");

    struct Chunk
    {
        Chunk* next;
        uint32_t usedBytes;
        uint32_t threadId;
        uint64_t firstTimestamp;
        uint8_t data[kChunkBytes - kChunkHeaderBytes];
    };
    static_assert(sizeof(Chunk) == kChunkBytes, "chunk header size changed");

    static uint64_t MakeHead(uint64_t encodedDelta, uint32_t tag)
    {
        return (encodedDelta << kTagBits) | tag;
    }

    static uint32_t HeadTag(uint64_t head)
    {
        return static_cast<uint32_t>(head & kTagMask);
    }

    static uint64_t HeadDelta(uint64_t head)
    {
        return head >> kTagBits;
    }

    static bool DeltaFitsInHead(uint64_t encodedDelta)
    {
        return (encodedDelta >> (64 - kTagBits)) == 0;
    }

    template <class ResolveFunction, class Emit> static void DecodeChunk(const Chunk& chunk, const uint8_t* in, const uint8_t* end, ResolveFunction& resolveFunction, Emit& emit)
    {
        uint32_t threadId = chunk.threadId;
        uint64_t timestamp = chunk.firstTimestamp;

        while (in < end)
        {
            uint64_t head;
            in = ReadVarint(in, head);

            if (HeadTag(head) == kThreadSwitchTag)
            {
                uint64_t newThreadId;
                in = ReadVarint(in, newThreadId);
                threadId = static_cast<uint32_t>(newThreadId);
                continue;
            }

            timestamp += static_cast<uint64_t>(ZigzagDecode(HeadDelta(head)));
            Event e{timestamp, static_cast<EventType>(HeadTag(head)), threadId, 0, 0, 0, 0};

            if (IsFunctionEvent(e.type))
            {
                uint64_t functionIndex;
                in = ReadVarint(in, functionIndex);
                FnKey fn = resolveFunction(static_cast<uint32_t>(functionIndex));
                e.a = fn.a;
                e.b = fn.b;
                e.nameId = fn.nameId;
                e.sourceId = fn.sourceId;
            }
            else
            {
                uint64_t nameId;
                uint64_t sourceId;
                in = ReadVarint(in, e.a);
                in = ReadVarint(in, e.b);
                in = ReadVarint(in, nameId);
                in = ReadVarint(in, sourceId);
                e.nameId = static_cast<uint32_t>(nameId);
                e.sourceId = static_cast<uint32_t>(sourceId);
            }

            emit(e);
        }
    }

    Chunk* AddChunk(uint64_t timestamp, uint32_t threadId)
    {
        Chunk* chunk = static_cast<Chunk*>(std::malloc(sizeof(Chunk)));
        if (chunk == nullptr)
        {
            return nullptr;
        }

        chunk->next = nullptr;
        chunk->usedBytes = 0;
        chunk->threadId = threadId;
        chunk->firstTimestamp = timestamp;

        if (m_tail != nullptr)
        {
            m_tail->next = chunk;
        }
        else
        {
            m_head = chunk;
        }
        m_tail = chunk;

        m_lastTimestamp = timestamp;
        m_lastThreadId = threadId;
        return chunk;
    }

    uint8_t* BeginRecord(uint64_t timestamp, uint32_t threadId, EventType type)
    {
        uint64_t encodedDelta = ZigzagEncode(static_cast<int64_t>(timestamp - m_lastTimestamp));

        bool needsNewChunk = m_tail == nullptr || m_tail->usedBytes + kMaxRecordBytes > sizeof(m_tail->data) || !DeltaFitsInHead(encodedDelta);
        if (needsNewChunk)
        {
            if (AddChunk(timestamp, threadId) == nullptr)
            {
                return nullptr;
            }
            encodedDelta = 0;
        }
        else if (threadId != m_lastThreadId)
        {
            uint8_t* out = m_tail->data + m_tail->usedBytes;
            out = WriteVarint(out, kThreadSwitchTag);
            out = WriteVarint(out, threadId);
            m_tail->usedBytes = static_cast<uint32_t>(out - m_tail->data);
            m_lastThreadId = threadId;
        }

        uint8_t* out = m_tail->data + m_tail->usedBytes;
        return WriteVarint(out, MakeHead(encodedDelta, static_cast<uint32_t>(type)));
    }

    void EndRecord(uint8_t* recordEnd, uint64_t timestamp)
    {
        m_tail->usedBytes = static_cast<uint32_t>(recordEnd - m_tail->data);
        m_lastTimestamp = timestamp;
    }

    Chunk* m_head = nullptr;
    Chunk* m_tail = nullptr;
    uint64_t m_lastTimestamp = 0;
    uint32_t m_lastThreadId = 0;
};

#endif
