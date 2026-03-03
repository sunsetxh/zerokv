from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext
import sys
import os
import subprocess

# Build extension using CMake
class CMakeBuildExt(build_ext):
    def build_extensions(self):
        # Build the C++ library first
        build_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        cmake_build_dir = os.path.join(build_dir, 'build')

        # Create build directory if needed
        if not os.path.exists(cmake_build_dir):
            os.makedirs(cmake_build_dir)

        # Configure and build
        os.chdir(cmake_build_dir)
        subprocess.run(['cmake', '..', '-DBUILD_PYTHON=ON', '-DBUILD_TESTS=OFF', '-DBUILD_EXAMPLES=OFF'],
                      check=True, capture_output=True)
        subprocess.run(['make', '-j4', '_zerokv'], check=True, capture_output=True)

        # Copy the built module
        src = os.path.join(cmake_build_dir, 'lib Zerokv.so')
        if os.path.exists(src):
            dst = os.path.join(self.build_lib, 'zerokv')
            if not os.path.exists(dst):
                os.makedirs(dst)
            import shutil
            shutil.copy(src, os.path.join(dst, '_zerokv.so'))

        # Remove extension from build list since we built it manually
        for ext in self.extensions:
            self.extensions.remove(ext)

        super().build_extensions()

setup(
    name="zerokv",
    version="0.1.0",
    description="High-performance distributed KV store for AI training",
    author="ZeroKV Team",
    packages=["zerokv"],
    package_data={"zerokv": ["__init__.py"]},
    install_requires=[],
    python_requires=">=3.8",
    ext_modules=[],  # Will be built by CMake
    cmdclass={'build_ext': CMakeBuildExt},
)
