#!/usr/bin/env python3

from setuptools import setup, Extension, find_packages
from setuptools.command.build_ext import build_ext
import sys
import os

class CMakeExtension(Extension):
    def __init__(self, name, sourcedir=''):
        Extension.__init__(self, name, sources=[])
        self.sourcedir = os.path.abspath(sourcedir)

class CMakeBuild(build_ext):
    def run(self):
        for ext in self.extensions:
            self.build_extension(ext)

    def build_extension(self, ext):
        import subprocess
        extdir = os.path.abspath(os.path.dirname(self.get_ext_fullpath(ext.name)))

        # CMake configure
        cmake_args = [
            f'-DCMAKE_LIBRARY_OUTPUT_DIRECTORY={extdir}',
            f'-DPYTHON_EXECUTABLE={sys.executable}',
            '-DBUILD_PYTHON=ON',
            '-DBUILD_TESTS=OFF',
            '-DBUILD_EXAMPLES=OFF',
        ]

        # Build type
        cfg = 'Debug' if self.debug else 'Release'
        build_args = ['--config', cfg]

        cmake_args += [f'-DCMAKE_BUILD_TYPE={cfg}']
        build_args += ['--', '-j4']

        env = os.environ.copy()
        env['CXXFLAGS'] = f'{env.get("CXXFLAGS", "")} -DVERSION_INFO={self.distribution.get_version()}'

        if not os.path.exists(self.build_temp):
            os.makedirs(self.build_temp)

        # CMake configure
        subprocess.check_call(['cmake', ext.sourcedir] + cmake_args,
                            cwd=self.build_temp, env=env)

        # CMake build
        subprocess.check_call(['cmake', '--build', '.'] + build_args,
                            cwd=self.build_temp)

# Read README for long description
def read_file(filename):
    with open(os.path.join(os.path.dirname(__file__), '..', filename), encoding='utf-8') as f:
        return f.read()

setup(
    name='zerokv',
    version='1.0.0',
    author='ZeroKV Team',
    author_email='zerokv-dev@example.com',
    description='High-performance NPU memory KV middleware with zero-copy transfer',
    long_description=read_file('README.md'),
    long_description_content_type='text/markdown',
    url='https://github.com/example/zerokv-middleware',
    packages=find_packages(),
    ext_modules=[CMakeExtension('zerokv.zerokv_native', sourcedir='..')],
    cmdclass={'build_ext': CMakeBuild},
    python_requires='>=3.7',
    install_requires=[
        'numpy>=1.19.0',
    ],
    extras_require={
        'torch': ['torch>=1.9.0'],
        'dev': [
            'pytest>=6.0.0',
            'pytest-cov>=2.12.0',
            'black>=21.6b0',
            'flake8>=3.9.0',
        ],
    },
    classifiers=[
        'Development Status :: 4 - Beta',
        'Intended Audience :: Developers',
        'Topic :: Software Development :: Libraries',
        'License :: OSI Approved :: Apache Software License',
        'Programming Language :: Python :: 3',
        'Programming Language :: Python :: 3.7',
        'Programming Language :: Python :: 3.8',
        'Programming Language :: Python :: 3.9',
        'Programming Language :: Python :: 3.10',
        'Programming Language :: C++',
    ],
    keywords='npu gpu rdma zero-copy kv-store distributed',
    project_urls={
        'Documentation': 'https://example.com/zerokv/docs',
        'Source': 'https://github.com/example/zerokv-middleware',
        'Tracker': 'https://github.com/example/zerokv-middleware/issues',
    },
)
