import os

from conan import ConanFile
from conan.tools.build import can_run, check_min_cppstd
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy, load

required_conan_version = ">=2.8.0 <3"


class UlogConan(ConanFile):
    name = "ulog"
    description = "A standalone performance-oriented C++ logging library"
    license = "Apache-2.0"
    homepage = "https://github.com/arenoros/ulog"
    url = "https://github.com/arenoros/ulog"
    topics = ("logging", "performance", "asynchronous", "libuv")
    package_type = "library"

    settings = "os", "arch", "compiler", "build_type"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "build_tests": [True, False],
        "build_stress_tests": [True, False],
        "build_benchmarks": [True, False],
        "dependency_smoke": [True, False],
        "warnings_as_errors": [True, False],
        "enable_asan": [True, False],
        "enable_ubsan": [True, False],
        "enable_tsan": [True, False],
        "enable_clang_tidy": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "build_tests": False,
        "build_stress_tests": False,
        "build_benchmarks": False,
        "dependency_smoke": False,
        "warnings_as_errors": False,
        "enable_asan": False,
        "enable_ubsan": False,
        "enable_tsan": False,
        "enable_clang_tidy": False,
        "fmt/*:shared": False,
        "libuv/*:shared": False,
        "gtest/*:shared": False,
        "benchmark/*:shared": False,
    }

    exports_sources = (
        ".clang-tidy",
        "CMakeLists.txt",
        "LICENSE",
        "version.txt",
        "cmake/*",
        "docs/migration/baseline.md",
        "docs/migration/capability-manifest.md",
        "docs/migration/corpus/*.json",
        "include/*",
        "scripts/*.cmd",
        "scripts/*.py",
        "src/*",
        "tests/*",
        "benchmarks/*",
        "tools/baseline_text_probe/*",
        "!**/__pycache__/*",
        "!**/*.pyc",
    )

    def set_version(self):
        if not self.version:
            self.version = load(self, os.path.join(self.recipe_folder, "version.txt")).strip()

    def config_options(self):
        if self.settings.os == "Windows":
            self.options.rm_safe("fPIC")

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def validate(self):
        check_min_cppstd(self, 20)
        if self.options.enable_tsan and (self.options.enable_asan or self.options.enable_ubsan):
            raise ValueError(
                "enable_tsan cannot be combined with enable_asan or enable_ubsan; use a separate package build"
            )

    def build_requirements(self):
        if self.options.build_tests:
            self.test_requires("gtest/1.17.0")
        if self.options.build_benchmarks:
            self.test_requires("benchmark/1.9.5")
        if self.options.dependency_smoke:
            self.test_requires("fmt/12.1.0")
            self.test_requires("libuv/1.51.0")

    def package_id(self):
        for option in (
            "build_tests",
            "build_stress_tests",
            "build_benchmarks",
            "dependency_smoke",
            "warnings_as_errors",
            "enable_clang_tidy",
        ):
            self.info.options.rm_safe(option)

    def layout(self):
        cmake_layout(self)

    def generate(self):
        dependencies = CMakeDeps(self)
        dependencies.generate()

        toolchain = CMakeToolchain(self)
        toolchain.cache_variables["BUILD_SHARED_LIBS"] = bool(self.options.shared)
        toolchain.cache_variables["BUILD_TESTING"] = True
        toolchain.cache_variables["ULOG_BUILD_UNIT_TESTS"] = bool(self.options.build_tests)
        toolchain.cache_variables["ULOG_BUILD_PACKAGE_TESTS"] = can_run(self)
        toolchain.cache_variables["ULOG_BUILD_STRESS_TESTS"] = bool(
            self.options.build_stress_tests
        )
        toolchain.cache_variables["ULOG_BUILD_BENCHMARKS"] = bool(
            self.options.build_benchmarks
        )
        toolchain.cache_variables["ULOG_BUILD_DEPENDENCY_SMOKE_TESTS"] = bool(
            self.options.dependency_smoke
        )
        toolchain.cache_variables["ULOG_WARNINGS_AS_ERRORS"] = bool(
            self.options.warnings_as_errors
        )
        toolchain.cache_variables["ULOG_ENABLE_ASAN"] = bool(self.options.enable_asan)
        toolchain.cache_variables["ULOG_ENABLE_UBSAN"] = bool(self.options.enable_ubsan)
        toolchain.cache_variables["ULOG_ENABLE_TSAN"] = bool(self.options.enable_tsan)
        toolchain.cache_variables["ULOG_ENABLE_CLANG_TIDY"] = bool(
            self.options.enable_clang_tidy
        )
        toolchain.cache_variables["CMAKE_CTEST_ARGUMENTS"] = "--output-on-failure"
        toolchain.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
        if can_run(self):
            cmake.test()

    def package(self):
        cmake = CMake(self)
        cmake.install()
        copy(
            self,
            "LICENSE",
            src=self.source_folder,
            dst=os.path.join(self.package_folder, "licenses"),
        )

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "ulog")
        self.cpp_info.set_property("cmake_target_name", "ulog::ulog")
        self.cpp_info.libs = ["ulog"]
        if not self.options.shared:
            self.cpp_info.defines = ["ULOG_STATIC_DEFINE"]

        sanitizer_link_flags = []
        if self.options.enable_asan:
            sanitizer_link_flags.append("-fsanitize=address")
        if self.options.enable_ubsan:
            sanitizer_link_flags.append("-fsanitize=undefined")
        if self.options.enable_tsan:
            sanitizer_link_flags.append("-fsanitize=thread")
        self.cpp_info.sharedlinkflags = list(sanitizer_link_flags)
        self.cpp_info.exelinkflags = list(sanitizer_link_flags)
