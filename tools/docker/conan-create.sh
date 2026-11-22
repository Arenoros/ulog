#!/bin/sh
# Build, test and package Ulog with one of the image's toolchains.
# Usage: tools/docker/conan-create.sh gcc12|clang16-libcxx [conan create args]
#   BUILD_TYPE=Debug tools/docker/conan-create.sh clang16-libcxx \
#       -o "&:enable_asan=True" -o "&:enable_ubsan=True"
# Later -o options override the defaults below.
set -eu
profile=$(dirname "$0")/profiles/$1; shift
# Clang sanitizers need llvm-symbolizer for readable reports.
export ASAN_SYMBOLIZER_PATH=/usr/bin/llvm-symbolizer-16
export TSAN_OPTIONS="${TSAN_OPTIONS:-} external_symbolizer_path=/usr/bin/llvm-symbolizer-16"
conan create . -pr:h="$profile" -pr:b="$profile" -s build_type="${BUILD_TYPE:-Release}" \
    -o "&:build_tests=True" -o "&:build_stress_tests=True" -o "&:dependency_smoke=True" \
    -o "&:warnings_as_errors=True" \
    --lockfile=conan.lock --build=missing "$@"
