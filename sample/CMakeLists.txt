# sample/ — Reference consumer for an INSTALLED oreo-kernel.
#
# This CMakeLists.txt is NOT consumed by the kernel build. It exists
# as documentation: the canonical recipe for a downstream project to
# link against an oreo-kernel install. To exercise it locally:
#
#   cmake --build build_msvc --config Release --target install \
#       --prefix /tmp/oreo-install
#   cmake -S sample -B sample/build \
#       -DCMAKE_PREFIX_PATH=/tmp/oreo-install
#   cmake --build sample/build --config Release
#   ./sample/build/Release/oreo_sample
#
# The CI workflow at .github/workflows/ci.yml has a `sample-consumer`
# job that runs this end-to-end so a regression in the install/export
# scripts surfaces on every push.

cmake_minimum_required(VERSION 3.20)
project(oreo_sample LANGUAGES C)

find_package(OreoKernel CONFIG REQUIRED)

add_executable(oreo_sample sample.c)
target_link_libraries(oreo_sample PRIVATE OreoKernel::oreo-kernel)
