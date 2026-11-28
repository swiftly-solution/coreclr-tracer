#ifndef _src_tracer_profile_writer_h
#define _src_tracer_profile_writer_h

#include "../shared/api.h"
#include "varint.h"
#include <cstddef>
#include <cstdint>
#include <set>
#include <string_view>
#include <unordered_map>
#include <vector>

enum class ProtoWireType : uint32_t
{
    Varint = 0,
    LengthDelimited = 2,
};

enum class ProtoProfileField : uint32_t
{
    Version = 1,
    Strings = 2,
    Spans = 3,
    Allocations = 4,
    Threads = 5,
};

enum class ProtoSpanField : uint32_t
{
    Name = 1,
    Category = 2,
    OsThreadId = 3,
    StartNs = 4,
    DurationNs = 5,
    Source = 6,
    Open = 7,
};

enum class ProtoSpanCategory : uint32_t
{
    Managed = 0,
    NativeToManaged = 1,
    Custom = 2,
};

enum class ProtoAllocationField : uint32_t
{
    TypeName = 1,
    OsThreadId = 2,
    TimestampNs = 3,
    Size = 4,
    Function = 5,
};

enum class ProtoThreadNameField : uint32_t
{
    OsThreadId = 1,
    Name = 2,
};

constexpr uint32_t kProtoFieldNumberShift = 3;
constexpr size_t kProtoMaxTagBytes = kVarintMaxBytes32;
constexpr size_t kProtoMaxFieldBytes = kProtoMaxTagBytes + kVarintMaxBytes64;

class ProtoMessage
{
public:
    static constexpr size_t kCapacity = 8 * kProtoMaxFieldBytes;

    template <class Field> void Varint(Field field, uint64_t value)
    {
        m_end = WriteTag(m_end, static_cast<uint32_t>(field), ProtoWireType::Varint);
        m_end = WriteVarint(m_end, value);
    }

    const uint8_t* Data() const
    {
        return m_data;
    }

    size_t Size() const
    {
        return static_cast<size_t>(m_end - m_data);
    }

    static uint8_t* WriteTag(uint8_t* out, uint32_t field, ProtoWireType type)
    {
        return WriteVarint(out, (static_cast<uint64_t>(field) << kProtoFieldNumberShift) | static_cast<uint32_t>(type));
    }

private:
    uint8_t m_data[kCapacity];
    uint8_t* m_end = m_data;
};

using ProfileBlockWriter = bool (*)(const void* data, size_t size, void* context);

class ProfileWriter
{
public:
    static constexpr size_t kBlockBytes = 4 * 1024 * 1024;

    ProfileWriter(ProfileBlockWriter write, void* context) : m_write(write), m_context(context)
    {
        m_block.reserve(kBlockBytes + ProtoMessage::kCapacity + kProtoMaxFieldBytes);
    }

    void Add(const Event& e, uint64_t eventNs)
    {
        m_threads.insert(e.osThreadId);
        std::vector<OpenSpan>& stack = m_stacks[e.osThreadId];

        switch (e.type)
        {
        case EventType::FunctionEnter:
        case EventType::UnmanagedToManaged:
        case EventType::CustomBegin:
            stack.push_back(OpenSpan{e.type, e.osThreadId, e.a, e.nameId, e.sourceId, eventNs});
            break;
        case EventType::FunctionExit:
        case EventType::FunctionTailcall:
            Close(stack, eventNs, [&](const OpenSpan& s) { return s.type == EventType::FunctionEnter && s.a == e.a; });
            break;
        case EventType::ManagedToUnmanaged:
            Close(stack, eventNs, [&](const OpenSpan& s) { return s.type == EventType::UnmanagedToManaged && s.a == e.a; });
            break;
        case EventType::CustomEnd:
            Close(stack, eventNs, [&](const OpenSpan& s) { return s.type == EventType::CustomBegin && s.nameId == e.nameId; });
            break;
        case EventType::ObjectAllocated:
            WriteAllocation(e, eventNs);
            break;
        default:
            break;
        }
    }

    template <class ThreadNameId, class GetString> bool Finish(uint64_t endOfTraceNs, uint32_t stringCount, ThreadNameId&& threadNameId, GetString&& getString)
    {
        for (auto& [threadId, stack] : m_stacks)
        {
            while (!stack.empty())
            {
                WriteSpan(stack.back(), endOfTraceNs, true);
                stack.pop_back();
            }
        }

        for (uint32_t threadId : m_threads)
        {
            uint32_t nameId = threadNameId(threadId);
            if (nameId == 0)
            {
                continue;
            }
            ProtoMessage thread;
            thread.Varint(ProtoThreadNameField::OsThreadId, threadId);
            thread.Varint(ProtoThreadNameField::Name, nameId);
            WriteMessage(ProtoProfileField::Threads, thread);
        }

        ProtoMessage header;
        header.Varint(ProtoProfileField::Version, 1);
        Append(header.Data(), header.Size());

        for (uint32_t id = 0; id <= stringCount; ++id)
        {
            WriteString(ProtoProfileField::Strings, getString(id));
        }

        return Flush(true);
    }

    bool Failed() const
    {
        return m_failed;
    }

private:
    struct OpenSpan
    {
        EventType type;
        uint32_t osThreadId;
        uint64_t a;
        uint32_t nameId;
        uint32_t sourceId;
        uint64_t startNs;
    };

    template <class Match> void Close(std::vector<OpenSpan>& stack, uint64_t endNs, Match&& match)
    {
        for (size_t i = stack.size(); i-- > 0;)
        {
            if (!match(stack[i]))
            {
                continue;
            }
            while (stack.size() > i)
            {
                WriteSpan(stack.back(), endNs, false);
                stack.pop_back();
            }
            return;
        }
    }

    static ProtoSpanCategory CategoryOf(EventType type)
    {
        switch (type)
        {
        case EventType::UnmanagedToManaged:
            return ProtoSpanCategory::NativeToManaged;
        case EventType::CustomBegin:
            return ProtoSpanCategory::Custom;
        default:
            return ProtoSpanCategory::Managed;
        }
    }

    void WriteSpan(const OpenSpan& s, uint64_t endNs, bool open)
    {
        uint64_t durationNs = endNs > s.startNs ? endNs - s.startNs : 0;

        ProtoMessage span;
        span.Varint(ProtoSpanField::Name, s.nameId);
        span.Varint(ProtoSpanField::Category, static_cast<uint32_t>(CategoryOf(s.type)));
        span.Varint(ProtoSpanField::OsThreadId, s.osThreadId);
        span.Varint(ProtoSpanField::StartNs, s.startNs);
        span.Varint(ProtoSpanField::DurationNs, durationNs);
        span.Varint(ProtoSpanField::Source, s.sourceId);
        span.Varint(ProtoSpanField::Open, open ? 1 : 0);
        WriteMessage(ProtoProfileField::Spans, span);
    }

    void WriteAllocation(const Event& e, uint64_t timestampNs)
    {
        ProtoMessage alloc;
        alloc.Varint(ProtoAllocationField::TypeName, e.nameId);
        alloc.Varint(ProtoAllocationField::OsThreadId, e.osThreadId);
        alloc.Varint(ProtoAllocationField::TimestampNs, timestampNs);
        alloc.Varint(ProtoAllocationField::Size, e.b);
        alloc.Varint(ProtoAllocationField::Function, e.sourceId);
        WriteMessage(ProtoProfileField::Allocations, alloc);
    }

    void WriteMessage(ProtoProfileField field, const ProtoMessage& message)
    {
        WriteLengthDelimited(field, message.Data(), message.Size());
    }

    void WriteString(ProtoProfileField field, std::string_view value)
    {
        WriteLengthDelimited(field, reinterpret_cast<const uint8_t*>(value.data()), value.size());
    }

    void WriteLengthDelimited(ProtoProfileField field, const uint8_t* data, size_t size)
    {
        uint8_t prefix[kProtoMaxTagBytes + kVarintMaxBytes64];
        uint8_t* end = ProtoMessage::WriteTag(prefix, static_cast<uint32_t>(field), ProtoWireType::LengthDelimited);
        end = WriteVarint(end, size);
        Append(prefix, static_cast<size_t>(end - prefix));
        Append(data, size);
        Flush(false);
    }

    void Append(const uint8_t* data, size_t size)
    {
        m_block.insert(m_block.end(), data, data + size);
    }

    bool Flush(bool force)
    {
        if (m_failed)
        {
            m_block.clear();
            return false;
        }
        if (m_block.empty() || (!force && m_block.size() < kBlockBytes))
        {
            return true;
        }
        if (!m_write(m_block.data(), m_block.size(), m_context))
        {
            m_failed = true;
        }
        m_block.clear();
        return !m_failed;
    }

    ProfileBlockWriter m_write;
    void* m_context;
    bool m_failed = false;
    std::vector<uint8_t> m_block;
    std::unordered_map<uint32_t, std::vector<OpenSpan>> m_stacks;
    std::set<uint32_t> m_threads;
};

#endif
