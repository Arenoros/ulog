add_executable(ulog-example-simple simple/main.cpp)
target_link_libraries(ulog-example-simple PRIVATE ulog::ulog)

add_executable(ulog-example-async async_file/main.cpp)
target_link_libraries(ulog-example-async PRIVATE ulog::ulog)

add_executable(ulog-example-tracing tracing_hook/main.cpp)
target_link_libraries(ulog-example-tracing PRIVATE ulog::ulog)

add_executable(ulog-example-custom-sink custom_sink/main.cpp)
target_link_libraries(ulog-example-custom-sink PRIVATE ulog::ulog)

add_executable(ulog-example-otlp otlp_json/main.cpp)
target_link_libraries(ulog-example-otlp PRIVATE ulog::ulog)
