target_sources(ulog PRIVATE
    detail/file_handle.cpp
    detail/socket.cpp
    detail/timestamp.cpp

    impl/logger_base.cpp

    format.cpp
    json_string.cpp
    level.cpp
    log.cpp
    log_extra.cpp
    log_helper.cpp
    null_logger.cpp
)
