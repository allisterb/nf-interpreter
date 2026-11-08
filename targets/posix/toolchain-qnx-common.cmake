#
# Copyright (c) .NET Foundation and Contributors
# See LICENSE file in the project root for full license information.
#

# Shared settings for the QNX toolchain files. qcc/q++ select the target with -V<target>, which
# CMake's QCC compiler module passes from CMAKE_<LANG>_COMPILER_TARGET.

macro(nf_qnx_toolchain qcc_target)

    if(NOT DEFINED ENV{QNX_HOST} OR NOT DEFINED ENV{QNX_TARGET})
        message(FATAL_ERROR "QNX_HOST and QNX_TARGET must be set; run the QNX SDP environment script first.")
    endif()

    file(TO_CMAKE_PATH "$ENV{QNX_HOST}" _nf_qnx_host)
    file(TO_CMAKE_PATH "$ENV{QNX_TARGET}" _nf_qnx_target)

    if(CMAKE_HOST_WIN32)
        set(_nf_qnx_exe ".exe")
    else()
        set(_nf_qnx_exe "")
    endif()

    set(CMAKE_C_COMPILER "${_nf_qnx_host}/usr/bin/qcc${_nf_qnx_exe}")
    set(CMAKE_CXX_COMPILER "${_nf_qnx_host}/usr/bin/q++${_nf_qnx_exe}")
    set(CMAKE_C_COMPILER_TARGET ${qcc_target})
    set(CMAKE_CXX_COMPILER_TARGET ${qcc_target})

    # Target libraries live under QNX_TARGET/<arch>/{lib,usr/lib}; headers under QNX_TARGET/usr/include.
    string(REGEX REPLACE "^gcc_nto" "" _nf_qnx_arch ${qcc_target})
    set(CMAKE_FIND_ROOT_PATH "${_nf_qnx_target}/${_nf_qnx_arch}" "${_nf_qnx_target}")

    # Do not search host paths for libraries/includes/packages.
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

endmacro()
