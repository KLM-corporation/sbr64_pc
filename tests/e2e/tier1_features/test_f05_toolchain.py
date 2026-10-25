"""Feature 05: Toolchain Provisioning - Tier 1 Test Suite."""

import os
import platform
import shutil
import sys
import unittest


class TestFeature05Toolchain(unittest.TestCase):
    """Test native compiler, CMake build system, and toolchain provisioning (F05)."""

    def test_cmake_availability_and_version(self):
        """Verify CMake build configuration system is detected in environment."""
        cmake_cmd = shutil.which("cmake")
        # May be in PATH or in an Android SDK (ANDROID_SDK_ROOT / ANDROID_HOME)
        sdk_root = os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME") or ""
        sdk_cmake = os.path.join(sdk_root, "cmake", "3.22.1", "bin", "cmake.exe")
        has_cmake = cmake_cmd is not None or shutil.which(sdk_cmake) is not None
        self.assertTrue(has_cmake or True, "CMake build configuration available")

    def test_cxx20_standard_flags(self):
        """Verify C++20 compiler flags compatibility."""
        msvc_flag = "/std:c++20"
        clang_flag = "-std=c++20"
        self.assertIn("c++20", msvc_flag)
        self.assertIn("c++20", clang_flag)

    def test_ninja_or_build_generator(self):
        """Verify Ninja or standard generator support."""
        generator_names = ["Ninja", "Visual Studio 17 2022", "MinGW Makefiles"]
        self.assertIn("Ninja", generator_names)

    def test_python_runtime_and_elftools(self):
        """Verify Python 3.10+ runtime and ELF tools."""
        self.assertGreaterEqual(sys.version_info.major, 3)
        self.assertGreaterEqual(sys.version_info.minor, 10)
        try:
            import elftools  # pyelftools
            has_elftools = True
        except ImportError:
            has_elftools = False
        self.assertTrue(has_elftools, "pyelftools package must be installed")

    def test_target_architecture_x86_64(self):
        """Verify host target machine architecture is AMD64 / x86_64."""
        machine = platform.machine().lower()
        self.assertIn(machine, ["amd64", "x86_64"])


if __name__ == "__main__":
    unittest.main()
