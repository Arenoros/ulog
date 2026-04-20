"""Conan 2 recipe for ulog.

Serves two roles:
- As a `conan create .` target — produces a binary Conan package so
  downstream projects can `self.requires("ulog/<version>")`.
- As a `conan install .` target — resolves ulog's own dev deps
  (runtime + tests + benchmarks) for the local build-from-source
  workflow that the repo's CI and devs have always used.

Keep these two roles in mind when editing: `requirements()` covers
both paths, while `test_requires()` only fires during dev-local
installs (gtest + benchmark stay out of the installed package so
consumers don't drag them transitively).
"""

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain


class UlogConan(ConanFile):
    name = "ulog"
    version = "0.1.0"
    license = "Apache-2.0"
    url = "https://github.com/Arenoros/ulog"
    description = (
        "Standalone cross-platform C++17 logging library extracted "
        "from userver. TSKV / LTSV / JSON / OTLP formatters, sync + "
        "async loggers, file / fd / TCP / AF_UNIX sinks, rate "
        "limiting, structured tags, dynamic debug."
    )
    topics = ("logging", "observability", "c++17", "otlp", "userver")

    settings = "os", "arch", "compiler", "build_type"

    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "with_nlohmann": [True, False],
        "with_yaml": [True, False],
        "with_http": [True, False],
        "with_afunix": [True, False],
        "no_short_macros": [True, False],
        "erase_log_with_level": [0, 1, 2, 3, 4],
        # Exposed for downstream users that install ulog but still want
        # to build its tests / benches against the same recipe. Flipped
        # OFF by default so `conan create` produces a lean package.
        "build_tests": [True, False],
        "build_bench": [True, False],
        "bench_spdlog": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "with_nlohmann": False,
        "with_yaml": False,
        "with_http": False,
        "with_afunix": False,
        "no_short_macros": False,
        "erase_log_with_level": 0,
        "build_tests": False,
        "build_bench": False,
        "bench_spdlog": False,
        # Transitive-dep option defaults. Pattern keys apply to
        # matching deps without FORCING them — a consumer that
        # explicitly pins `boost/*:shared=True` wins. The ones kept
        # here are the options ulog's own code actually requires
        # (the boost modules we link against + the workaround for
        # boost/1.86-1.90's cobalt recipe bug under C++20). Options
        # ulog doesn't care about (boost/fmt shared) are NOT listed,
        # so consumers are free to choose.
        "boost/*:header_only": False,
        "boost/*:without_container": False,
        "boost/*:without_stacktrace": False,
        "boost/*:without_cobalt": True,
        'spdlog/*:external_fmt': True
    }

    exports_sources = (
        "CMakeLists.txt",
        "cmake/*",
        "include/*",
        "src/*",
        "tests/*",
        "bench/*",
        "examples/*",
        "third_party/*",
        "README.md",
        "LICENSE*",
    )

    def config_options(self):
        if self.settings.os == "Windows":
            self.options.rm_safe("fPIC")

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")
        # Transitive boost/fmt option defaults are declared in
        # `default_options` with `<dep>/*:<opt>` pattern keys — that
        # form applies our preferences without FORCING them, so a
        # consumer that has its own `boost/*:shared=True` policy is
        # free to keep it. Using `self.options["boost"].xxx = value`
        # here would raise on conflict.

    def requirements(self):
        self.requires("fmt/12.1.0", transitive_headers=True, override=True)
        self.requires("boost/1.90.0", transitive_headers=True)
        if self.options.with_nlohmann:
            self.requires("nlohmann_json/3.11.3", transitive_headers=True)
        if self.options.with_yaml:
            self.requires("yaml-cpp/0.8.0")
        if self.options.with_http:
            self.requires("cpp-httplib/0.20.1", transitive_headers=True)

    def build_requirements(self):
        # `test_requires` are build-only — never propagated to a
        # consumer that `requires` ulog. Local dev (`conan install .`
        # at the root) still picks them up for the gtest target.
        if self.options.build_tests:
            self.test_requires("gtest/1.17.0")
        if self.options.build_bench:
            self.test_requires("benchmark/1.9.4")
            if self.options.bench_spdlog:
                # spdlog/1.15.1 supports fmt >=10; Conan unifies with our
                # fmt/12.1.0 if the recipe range allows it.
                self.test_requires("spdlog/1.15.1")

    # No `def layout()` — the recipe stays layout-less on purpose.
    #
    # `conan install . --output-folder=build` is the canonical entry point
    # for both CI and local dev; without a layout method every generated
    # file lands flat under that folder, so CMake finds
    # `build/conan_toolchain.cmake` at the expected path.
    #
    # Adding `cmake_layout(self)` would nest generators under
    # `build/<BuildType>/generators/` on single-config generators and
    # break the CI configure step that currently references the flat
    # path. The build/source separation `cmake_layout` offers is not
    # needed here — a single build_type per `conan install` call.
    #
    # `conan create .` still works: it sets its own source/build/package
    # cache folders internally during the create pipeline, independent of
    # whether the recipe defines a layout.

    def generate(self):
        tc = CMakeToolchain(self)
        # Examples are never built from the recipe — they don't ship.
        tc.variables["ULOG_BUILD_EXAMPLES"] = False
        tc.variables["ULOG_BUILD_TESTS"] = bool(self.options.build_tests)
        tc.variables["ULOG_BUILD_BENCH"] = bool(self.options.build_bench)
        tc.variables["ULOG_BENCH_SPDLOG"] = bool(self.options.bench_spdlog)
        tc.variables["ULOG_WITH_NLOHMANN"] = bool(self.options.with_nlohmann)
        tc.variables["ULOG_WITH_YAML"] = bool(self.options.with_yaml)
        tc.variables["ULOG_WITH_HTTP"] = bool(self.options.with_http)
        tc.variables["ULOG_WITH_AFUNIX"] = bool(self.options.with_afunix)
        tc.variables["ULOG_NO_SHORT_MACROS"] = bool(self.options.no_short_macros)
        tc.variables["ULOG_ERASE_LOG_WITH_LEVEL"] = str(self.options.erase_log_with_level)
        tc.generate()
        CMakeDeps(self).generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        CMake(self).install()

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "ulog")
        self.cpp_info.set_property("cmake_target_name", "ulog::ulog")
        self.cpp_info.libs = ["ulog"]

        # Public dep wiring — mirrors target_link_libraries(ulog PUBLIC …).
        self.cpp_info.requires = [
            "fmt::fmt",
            "boost::container",
            "boost::stacktrace",
        ]
        if self.options.with_nlohmann:
            self.cpp_info.requires.append("nlohmann_json::nlohmann_json")
        if self.options.with_http:
            self.cpp_info.requires.append("cpp-httplib::cpp-httplib")

        # Platform system libs.
        if self.settings.os == "Windows":
            self.cpp_info.system_libs = ["ws2_32"]
        elif self.settings.os in ("Linux", "FreeBSD"):
            self.cpp_info.system_libs = ["pthread", "dl"]

        # Public compile definitions — match the PUBLIC
        # target_compile_definitions in CMakeLists.txt.
        defs = []
        if self.options.no_short_macros:
            defs.append("ULOG_NO_SHORT_MACROS=1")
        if self.options.with_http:
            defs.append("ULOG_HAVE_HTTP=1")
        if self.options.with_afunix and self.settings.os == "Windows":
            defs.append("ULOG_HAVE_AFUNIX=1")
        if int(str(self.options.erase_log_with_level)) != 0:
            defs.append(f"ULOG_ERASE_LOG_WITH_LEVEL={self.options.erase_log_with_level}")
        if defs:
            self.cpp_info.defines = defs
