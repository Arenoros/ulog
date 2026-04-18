// Example: inject custom trace/span IDs into every record via tracing hook.
#include <string>

#include <ulog/config.hpp>
#include <ulog/log.hpp>
#include <ulog/tracing_hook.hpp>

namespace {

thread_local std::string g_trace_id;
thread_local std::string g_span_id;

void EmitTraceTags(ulog::TagSink& sink, void* /*ctx*/) {
    if (!g_trace_id.empty()) sink.AddTag("trace_id", g_trace_id);
    if (!g_span_id.empty())  sink.AddTag("span_id",  g_span_id);
}

}  // namespace

int main() {
    ulog::LoggerConfig cfg;
    cfg.file_path = "@stderr";
    cfg.format = ulog::Format::kTskv;
    cfg.level = ulog::Level::kTrace;
    auto logger = ulog::InitDefaultLogger(cfg);

    ulog::SetTracingHook(&EmitTraceTags, nullptr);

    g_trace_id = "trace-a";
    g_span_id = "span-1";
    LOG_INFO() << "inside trace-a";

    g_trace_id = "trace-b";
    g_span_id = "span-2";
    LOG_WARNING() << "inside trace-b";

    ulog::LogFlush();
    ulog::SetDefaultLogger(nullptr);
    return 0;
}
