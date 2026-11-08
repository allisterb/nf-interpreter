#
# Copyright (c) .NET Foundation and Contributors
# See LICENSE file in the project root for full license information.
#

# CMake toolchain file for cross-compiling to QNX 8.0 x86_64 with the QNX SDP's qcc/q++ drivers.
#
# Requires QNX_HOST and QNX_TARGET, e.g. set by running qnxsdp-env.bat / qnxsdp-env.sh.
#
# Usage (from the repo root; a relative toolchain path resolves against the -S directory):
#   cmake -S targets/posix -B build/posix-qnx-x86_64 -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=toolchain-x86_64-qnx.cmake

set(CMAKE_SYSTEM_NAME QNX)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

include(${CMAKE_CURRENT_LIST_DIR}/toolchain-qnx-common.cmake)
nf_qnx_toolchain(gcc_ntox86_64)
