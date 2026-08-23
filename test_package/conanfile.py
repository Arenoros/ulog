from conan import ConanFile
from conan.tools.build import can_run
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.env import VirtualRunEnv


class UlogTestPackage(ConanFile):
    test_type = "explicit"
    settings = "os", "arch", "compiler", "build_type"

    def requirements(self):
        self.requires(self.tested_reference_str)

    def layout(self):
        cmake_layout(self)

    def generate(self):
        dependencies = CMakeDeps(self)
        dependencies.generate()
        toolchain = CMakeToolchain(self)
        toolchain.generate()
        runtime = VirtualRunEnv(self)
        runtime.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def test(self):
        if can_run(self):
            self.run(
                f'ctest --test-dir "{self.build_folder}" --output-on-failure '
                f'-C "{self.settings.build_type}"',
                env="conanrun",
            )
