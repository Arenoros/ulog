#pragma once

#if defined(ULOG_STATIC_DEFINE)
#define ULOG_API
#elif defined(_WIN32) || defined(__CYGWIN__)
#if defined(ULOG_BUILDING_LIBRARY)
#define ULOG_API __declspec(dllexport)
#else
#define ULOG_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define ULOG_API __attribute__((visibility("default")))
#else
#define ULOG_API
#endif
