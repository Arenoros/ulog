"""ConanCenter submission recipe for ulog.

Differs from the repo-root `conanfile.py` in two important ways:

1. Sources are fetched from a tagged GitHub tarball listed in
   `conandata.yml`, not bundled via `exports_sources`. ConanCenter
   forbids shipping source from a non-canonical location.
2. No dev-only knobs (`build_tests`, `build_bench`). ConanCenter
   recipes publish the consumable library; tests live in
   `test_package/` and run only as part of CI verification.

To submit: copy this `recipes/ulog/` folder into a fork of
https://github.com/conan-io/conan-center-index under the same path,
update `conandata.yml` with a real tarball URL + sha256, open a PR.
"""

import os

from conan import ConanFile
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy, get, rmdir
from conan.tools.scm import Version


class UlogConan(ConanFile):
    name = "ulog"
    description = (
        "Standalone cross-platform C++17 logging library extracted "
        "from userver. TSKV / LTSV / JSON / OTLP formatters, sync + "
        "async loggers, file / fd / TCP / AF_UNIX sinks, rate "
        "limiting, structured tags, dynamic debug."
    )
    license = "Apache-2.0"
    url = "https://github.com/conan-io/conan-center-index"
    homepage = "https://github.com/Arenoros/ulog"
    topics = ("logging", "observability", "c++17", "otlp", "userver")

    package_type = "library"
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
        # Transitive-dep option defaults. Pattern keys apply without
        # FORCING, so consumers can override each one if they want to.
        # Only the options ulog's own code actually needs are listed
        # here (the specific boost modules we link against).
        "boost/*:header_only": False,
        "boost/*:without_container": False,
        "boost/*:without_stacktrace": False,
        "boost/*:without_cobalt": True,
    }

    @property
    def _min_cppstd(self):
        return 17

    @property
    def _compilers_minimum_version(self):
        return {
            "gcc": "9",
            "clang": "11",
            "apple-clang": "11",
            "Visual Studio": "16",
            "msvc": "192",
        }

    def config_options(self):
        if self.settings.os == "Windows":
            self.options.rm_safe("fPIC")
        else:
            # AF_UNIX is Windows-only in this recipe — POSIX has it
            # natively, no opt-in needed.
            self.options.rm_safe("with_afunix")

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")
        # Transitive boost option defaults live in `default_options`
        # as `boost/*:<opt>` pattern keys — see the note there.
        # `self.options["boost"].xxx = value` would hard-force and
        # conflict with consumers that have their own boost policy.

    def requirements(self):
        self.requires("fmt/11.0.2", transitive_headers=True)
        self.requires("boost/1.86.0", transitive_headers=True)
        if self.options.with_nlohmann:
            self.requires("nlohmann_json/3.11.3", transitive_headers=True)
        if self.options.with_yaml:
            self.requires("yaml-cpp/0.8.0")
        if self.options.with_http:
            self.requires("cpp-httplib/0.18.1", transitive_headers=True)

    def validate(self):
        if self.settings.compiler.get_safe("cppstd"):
            check_min_cppstd(self, self._min_cppstd)
        minimum = self._compilers_minimum_version.get(str(self.settings.compiler))
        if minimum and Version(self.settings.compiler.version) < minimum:
            raise Exception(
                f"{self.ref} requires {self.settings.compiler} "
                f"{minimum} or higher"
            )

    def layout(self):
        cmake_layout(self, src_folder="src")

    def source(self):
        get(self, **self.conan_data["sources"][self.version], strip_root=True)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["ULOG_BUILD_TESTS"] = False
        tc.variables["ULOG_BUILD_EXAMPLES"] = False
        tc.variables["ULOG_BUILD_BENCH"] = False
        tc.variables["ULOG_WITH_NLOHMANN"] = bool(self.options.with_nlohmann)
        tc.variables["ULOG_WITH_YAML"] = bool(self.options.with_yaml)
        tc.variables["ULOG_WITH_HTTP"] = bool(self.options.with_http)
        tc.variables["ULOG_WITH_AFUNIX"] = bool(self.options.get_safe("with_afunix", False))
        tc.variables["ULOG_NO_SHORT_MACROS"] = bool(self.options.no_short_macros)
        tc.variables["ULOG_ERASE_LOG_WITH_LEVEL"] = str(self.options.erase_log_with_level)
        tc.generate()
        CMakeDeps(self).generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        copy(self, "LICENSE*", src=self.source_folder, dst=os.path.join(self.package_folder, "licenses"))
        cmake = CMake(self)
        cmake.install()
        # ConanCenter hooks flag these — strip CMake export fragments
        # the recipe's own package_info() handles.
        rmdir(self, os.path.join(self.package_folder, "lib", "cmake"))

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "ulog")
        self.cpp_info.set_property("cmake_target_name", "ulog::ulog")
        self.cpp_info.libs = ["ulog"]

        self.cpp_info.requires = [
            "fmt::fmt",
            "boost::container",
            "boost::stacktrace",
        ]
        if self.options.with_nlohmann:
            self.cpp_info.requires.append("nlohmann_json::nlohmann_json")
        if self.options.with_http:
            self.cpp_info.requires.append("cpp-httplib::cpp-httplib")

        if self.settings.os == "Windows":
            self.cpp_info.system_libs = ["ws2_32"]
        elif self.settings.os in ("Linux", "FreeBSD"):
            self.cpp_info.system_libs = ["pthread", "dl"]

        defs = []
        if self.options.no_short_macros:
            defs.append("ULOG_NO_SHORT_MACROS=1")
        if self.options.with_http:
            defs.append("ULOG_HAVE_HTTP=1")
        if self.options.get_safe("with_afunix") and self.settings.os == "Windows":
            defs.append("ULOG_HAVE_AFUNIX=1")
        if int(str(self.options.erase_log_with_level)) != 0:
            defs.append(f"ULOG_ERASE_LOG_WITH_LEVEL={self.options.erase_log_with_level}")
        if defs:
            self.cpp_info.defines = defs
