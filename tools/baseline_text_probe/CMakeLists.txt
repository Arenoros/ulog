cmake_minimum_required(VERSION 3.20)

if(WIN32)
  message(
    FATAL_ERROR
      "The pinned userver baseline does not support a native Windows build. Configure this probe on Linux or macOS, or in a supported VM, and retry."
  )
endif()

project(ulog-baseline-text-probe LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../baseline_probe/UlogBaselineProbe.cmake")
ulog_add_baseline_probe(
  TARGET ulog-baseline-text-probe SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}" DESCRIPTION text
)
