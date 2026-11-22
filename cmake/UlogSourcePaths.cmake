include_guard(GLOBAL)

# ulog_target_source_path_prefixes(<target> <INTERFACE|PUBLIC|PRIVATE> <prefix>...)
#
# Defines ULOG_SOURCE_PATH_PREFIXES for <target>, so Ulog call sites in its translation units drop
# the first matching prefix, and the separators after it, from their source paths. A prefix matches
# whole path components only. Call it once per target, with the same prefixes for every translation
# unit that shares inline code containing Ulog macros.
function(ulog_target_source_path_prefixes target scope)
  if(NOT scope MATCHES "^(INTERFACE|PUBLIC|PRIVATE)$")
    message(
      FATAL_ERROR
        "ulog_target_source_path_prefixes: scope must be INTERFACE, PUBLIC, or PRIVATE, not '${scope}'."
    )
  endif()
  if(ARGC LESS 3)
    message(FATAL_ERROR "ulog_target_source_path_prefixes: give at least one prefix.")
  endif()

  set(_ulog_literals)
  foreach(_ulog_prefix IN LISTS ARGN)
    if(_ulog_prefix STREQUAL "")
      message(FATAL_ERROR "ulog_target_source_path_prefixes: a prefix must not be empty.")
    endif()
    # Forward slashes match either separator on Windows and need no escaping in a C++ literal.
    file(TO_CMAKE_PATH "${_ulog_prefix}" _ulog_prefix)
    string(REPLACE "\\" "\\\\" _ulog_prefix "${_ulog_prefix}")
    string(REPLACE "\"" "\\\"" _ulog_prefix "${_ulog_prefix}")
    list(APPEND _ulog_literals "\"${_ulog_prefix}\"")
  endforeach()
  list(JOIN _ulog_literals "," _ulog_value)
  target_compile_definitions(${target} ${scope} "ULOG_SOURCE_PATH_PREFIXES=${_ulog_value}")
endfunction()
