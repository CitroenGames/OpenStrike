#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#endif

// Chrome Trace Event Format profiler for VPK operations.
// Output is loadable in chrome://tracing or ui.perfetto.dev.

struct EditorTraceEvent
{
    const char* name;
    const char* category;
    uint64_t    ts;     // start timestamp in microseconds
    uint64_t    dur;    // duration in microseconds
    uint32_t    tid;
    uint32_t    pid;
};

struct EditorTraceMetadata
{
    const char* metaType; // "thread_name" or "process_name"
    const char* name;
    uint32_t    tid;
    uint32_t    pid;
};

class EditorProfiler
{
public:
    static EditorProfiler& Instance();

    void BeginSession(const char* sessionName = "EditorTrace");
    void EndSession();
    bool IsActive() const { return m_active.load(std::memory_order_relaxed); }

    void RecordEvent(const EditorTraceEvent& ev);
    void SetThreadName(const char* name);

    // Write all events to JSON. Returns file path on success, empty on failure.
    std::string FlushToFile(const char* outputPath = nullptr);

    size_t GetEventCount() const;

private:
    EditorProfiler() = default;

    std::atomic<bool>                m_active{false};
    std::string                      m_sessionName;
    mutable std::mutex               m_mutex;
    std::vector<EditorTraceEvent>    m_events;
    std::vector<EditorTraceMetadata> m_metadata;
};

// ---- Timing helpers ----

inline uint64_t EditorProfiler_GetTimestampUS()
{
    static const auto s_start = std::chrono::high_resolution_clock::now();
    auto now = std::chrono::high_resolution_clock::now();
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        now - s_start).count();
}

inline uint32_t EditorProfiler_GetThreadId()
{
#ifdef _WIN32
    return (uint32_t)::GetCurrentThreadId();
#else
    return 0;
#endif
}

// ---- RAII scope guard ----

class EditorProfileScope
{
public:
    EditorProfileScope(const char* name, const char* category = "vpk")
        : m_name(name), m_category(category)
    {
        if (EditorProfiler::Instance().IsActive())
        {
            m_active = true;
            m_startUS = EditorProfiler_GetTimestampUS();
            m_tid = EditorProfiler_GetThreadId();
        }
    }

    ~EditorProfileScope()
    {
        if (m_active)
        {
            uint64_t endUS = EditorProfiler_GetTimestampUS();
            EditorTraceEvent ev;
            ev.name     = m_name;
            ev.category = m_category;
            ev.ts       = m_startUS;
            ev.dur      = endUS - m_startUS;
            ev.tid      = m_tid;
            ev.pid      = 1;
            EditorProfiler::Instance().RecordEvent(ev);
        }
    }

    EditorProfileScope(const EditorProfileScope&) = delete;
    EditorProfileScope& operator=(const EditorProfileScope&) = delete;

private:
    const char* m_name;
    const char* m_category;
    uint64_t    m_startUS = 0;
    uint32_t    m_tid = 0;
    bool        m_active = false;
};

// ---- Macros ----

#define EDITOR_CONCAT_IMPL(a, b) a##b
#define EDITOR_CONCAT(a, b) EDITOR_CONCAT_IMPL(a, b)

#define EDITOR_PROFILE_SCOPE(name) \
    EditorProfileScope EDITOR_CONCAT(_edProfileScope, __LINE__)(name, "startup")

#define EDITOR_PROFILE_SCOPE_CAT(name, cat) \
    EditorProfileScope EDITOR_CONCAT(_edProfileScope, __LINE__)(name, cat)

#define EDITOR_PROFILE_FUNCTION() \
    EditorProfileScope EDITOR_CONCAT(_edProfileScope, __LINE__)(__FUNCTION__, "startup")
