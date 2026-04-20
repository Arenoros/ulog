target_sources(ulog PRIVATE
    detail/file_handle.cpp
    detail/socket.cpp
    detail/timestamp.cpp

    impl/logger_base.cpp
    impl/tag_writer.cpp
    impl/formatters/json.cpp
    impl/formatters/ltsv.cpp
    impl/formatters/otlp_json.cpp
    impl/formatters/raw.cpp
    impl/formatters/tskv.cpp

    sinks/fd_sink.cpp
    sinks/file_sink.cpp
    sinks/instrumented_sink.cpp
    sinks/otlp_batch_sink.cpp
    sinks/tcp_socket_sink.cpp

    dynamic_debug.cpp
    stacktrace_cache.cpp
    format.cpp
    json_string.cpp
    level.cpp
    log.cpp
    log_extra.cpp
    log_helper.cpp
    null_logger.cpp
    record_enricher.cpp
    sync_logger.cpp
    async_logger.cpp
    config.cpp
    tracing_hook.cpp
)

if(NOT WIN32)
    target_sources(ulog PRIVATE
        sinks/unix_socket_sink.cpp
        posix/sigusr1_handler.cpp
    )
endif()

if(WIN32)
    target_sources(ulog PRIVATE win/named_event_handler.cpp)
    # Win10 1803+ brings AF_UNIX via <afunix.h>. Opt in with
    # `-DULOG_WITH_AFUNIX=ON` — the top-level CMakeLists then pushes
    # `ULOG_HAVE_AFUNIX=1` as a PUBLIC compile definition and the sink
    # source comes into the build.
    if(ULOG_WITH_AFUNIX)
        target_sources(ulog PRIVATE sinks/unix_socket_sink.cpp)
    endif()
endif()
