#
# Copyright (c) .NET Foundation and Contributors
# See LICENSE file in the project root for full license information.
#

# CMake toolchain file for cross-compiling to QNX 8.0 aarch64 (little-endian) with the QNX SDP's
# qcc/q++ drivers. See toolchain-x86_64-qnx.cmake for requirements and usage.

set(CMAKE_SYSTEM_NAME QNX)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

include(${CMAKE_CURRENT_LIST_DIR}/toolchain-qnx-common.cmake)
nf_qnx_toolchain(gcc_ntoaarch64le)
