"""test_package for the ConanCenter ulog recipe.

ConanCenter's CI runs this as part of the PR gate: it builds a
tiny consumer that `requires("ulog/<version>")`, compiles a one-file
main against `ulog::ulog`, and runs it once. If the binary exits
zero, the recipe is considered integration-clean.
"""

import os

from conan import ConanFile
from conan.tools.build import can_run
from conan.tools.cmake import CMake, cmake_layout


class UlogTestPackageConan(ConanFile):
    settings = "os", "arch", "compiler", "build_type"
    generators = "CMakeDeps", "CMakeToolchain"
    test_type = "explicit"

    def requirements(self):
        self.requires(self.tested_reference_str)

    def layout(self):
        cmake_layout(self)

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def test(self):
        if can_run(self):
            bin_path = os.path.join(self.cpp.build.bindir, "test_package")
            self.run(bin_path, env="conanrun")
