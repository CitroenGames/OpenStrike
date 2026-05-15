#include "editor_profiler.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>

EditorProfiler& EditorProfiler::Instance()
{
    static EditorProfiler s_instance;
    return s_instance;
}

void EditorProfiler::BeginSession(const char* sessionName)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_events.clear();
    m_metadata.clear();
    m_events.reserve(4096);
    m_sessionName = sessionName ? sessionName : "EditorTrace";
    m_active.store(true, std::memory_order_release);

    EditorTraceMetadata meta;
    meta.metaType = "process_name";
    meta.name     = "Hammer 2.0";
    meta.tid      = 0;
    meta.pid      = 1;
    m_metadata.push_back(meta);
}

void EditorProfiler::EndSession()
{
    m_active.store(false, std::memory_order_release);
}

void EditorProfiler::RecordEvent(const EditorTraceEvent& ev)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_events.push_back(ev);
}

void EditorProfiler::SetThreadName(const char* name)
{
    uint32_t tid = EditorProfiler_GetThreadId();

    std::lock_guard<std::mutex> lock(m_mutex);

    // Avoid duplicate entries for the same thread
    for (const auto& m : m_metadata)
    {
        if (strcmp(m.metaType, "thread_name") == 0 && m.tid == tid)
            return;
    }

    EditorTraceMetadata meta;
    meta.metaType = "thread_name";
    meta.name     = name;
    meta.tid      = tid;
    meta.pid      = 1;
    m_metadata.push_back(meta);
}

size_t EditorProfiler::GetEventCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_events.size();
}

std::string EditorProfiler::FlushToFile(const char* outputPath)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_events.empty() && m_metadata.empty())
        return "";

    std::string path;
    if (outputPath && outputPath[0])
    {
        path = outputPath;
    }
    else
    {
        time_t now = time(nullptr);
        struct tm t;
#ifdef _WIN32
        localtime_s(&t, &now);
#else
        localtime_r(&now, &t);
#endif
        char buf[128];
        snprintf(buf, sizeof(buf), "editor_trace_%04d%02d%02d_%02d%02d%02d.json",
            t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
            t.tm_hour, t.tm_min, t.tm_sec);
        path = buf;
    }

    FILE* f = fopen(path.c_str(), "w");
    if (!f)
        return "";

    fprintf(f, "{\"traceEvents\":[\n");

    bool first = true;

    for (const auto& meta : m_metadata)
    {
        if (!first) fprintf(f, ",\n");
        first = false;

        if (strcmp(meta.metaType, "process_name") == 0)
        {
            fprintf(f,
                "{\"name\":\"process_name\",\"ph\":\"M\","
                "\"pid\":%u,\"tid\":0,"
                "\"args\":{\"name\":\"%s\"}}",
                meta.pid, meta.name);
        }
        else
        {
            fprintf(f,
                "{\"name\":\"thread_name\",\"ph\":\"M\","
                "\"pid\":%u,\"tid\":%u,"
                "\"args\":{\"name\":\"%s\"}}",
                meta.pid, meta.tid, meta.name);
        }
    }

    for (const auto& ev : m_events)
    {
        if (!first) fprintf(f, ",\n");
        first = false;

        fprintf(f,
            "{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"X\","
            "\"pid\":%u,\"tid\":%u,"
            "\"ts\":%llu,\"dur\":%llu}",
            ev.name, ev.category,
            ev.pid, ev.tid,
            (unsigned long long)ev.ts,
            (unsigned long long)ev.dur);
    }

    fprintf(f, "\n]}\n");
    fclose(f);

    m_events.clear();
    m_metadata.clear();

    return path;
}
