# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use an out-of-source build: cmake -S . -B build")
endif()
if(DEFINED CMAKE_GENERATOR AND NOT CMAKE_GENERATOR STREQUAL "Unix Makefiles")
    message(FATAL_ERROR "aix-fuse supports the Unix Makefiles generator")
endif()
if(NOT DEFINED CMAKE_C_COMPILER)
    find_program(AIX_FUSE_DEFAULT_GCC NAMES gcc REQUIRED)
    set(CMAKE_C_COMPILER "${AIX_FUSE_DEFAULT_GCC}" CACHE FILEPATH "AIX C compiler" FORCE)
endif()
project(aix_fuse VERSION 0.1.0 LANGUAGES C)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "AIX" OR CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "Build and test aix-fuse directly on AIX")
endif()
if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "aix-fuse requires GCC on AIX")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_C_ARCHIVE_CREATE "<CMAKE_AR> -X64 rc <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_C_ARCHIVE_APPEND "<CMAKE_AR> -X64 r <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_C_ARCHIVE_FINISH ":")

set(AIX_FUSE_PACKAGE_VERSION "${PROJECT_VERSION}" CACHE STRING "RPM package version")
set(AIX_FUSE_PACKAGE_RELEASE "1" CACHE STRING "RPM package release")
set(AIX_FUSE_COMPAT_VERSION "3.19.0")
set(AIX_FUSE_USFS_SOURCE_DIR "" CACHE PATH "Existing USFS source tree used instead of downloading")
set(AIX_FUSE_USFS_REPOSITORY "https://github.com/hettica/usfs.git" CACHE STRING "Native USFS repository")
set(AIX_FUSE_USFS_REVISION "d888e9574addbd51b757770706381a1d0a594f45"
    CACHE STRING "Published immutable 40-character USFS commit")

if(AIX_FUSE_USFS_SOURCE_DIR)
    get_filename_component(AIX_FUSE_USFS_RESOLVED_SOURCE_DIR
        "${AIX_FUSE_USFS_SOURCE_DIR}" REALPATH BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set(AIX_FUSE_USFS_SOURCE_KIND local)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bundled/usfs/src/client/CMakeLists.txt")
    set(AIX_FUSE_USFS_RESOLVED_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bundled/usfs")
    set(AIX_FUSE_USFS_SOURCE_KIND bundled)
else()
    string(LENGTH "${AIX_FUSE_USFS_REVISION}" _aix_fuse_revision_length)
    if(NOT _aix_fuse_revision_length EQUAL 40 OR
       NOT AIX_FUSE_USFS_REVISION MATCHES "^[0-9a-fA-F]+$")
        message(FATAL_ERROR
            "A compatible native USFS revision has not been pinned for this release. "
            "Set AIX_FUSE_USFS_SOURCE_DIR to the prepared native USFS checkout, or "
            "set AIX_FUSE_USFS_REVISION to a published compatible commit SHA.")
    endif()
    include(FetchContent)
    FetchContent_Declare(aix_fuse_usfs
        GIT_REPOSITORY "${AIX_FUSE_USFS_REPOSITORY}"
        GIT_TAG "${AIX_FUSE_USFS_REVISION}"
        GIT_SHALLOW FALSE
        UPDATE_DISCONNECTED TRUE)
    FetchContent_GetProperties(aix_fuse_usfs)
    if(NOT aix_fuse_usfs_POPULATED)
        FetchContent_Populate(aix_fuse_usfs)
    endif()
    set(AIX_FUSE_USFS_RESOLVED_SOURCE_DIR "${aix_fuse_usfs_SOURCE_DIR}")
    set(AIX_FUSE_USFS_SOURCE_KIND pinned)
endif()

foreach(_required IN ITEMS LICENSE src/client/CMakeLists.txt
        src/client/include/usfs/usfs.h src/client/lib/usfs_client.c
        src/common/usfs_validate.c)
    if(NOT EXISTS "${AIX_FUSE_USFS_RESOLVED_SOURCE_DIR}/${_required}")
        message(FATAL_ERROR "The selected USFS source lacks ${_required}; use the native API revision")
    endif()
endforeach()
add_subdirectory("${AIX_FUSE_USFS_RESOLVED_SOURCE_DIR}/src/client"
    "${CMAKE_CURRENT_BINARY_DIR}/native-client" EXCLUDE_FROM_ALL)

find_program(AIX_FUSE_GIT_EXECUTABLE NAMES git)
function(aix_fuse_source_identity directory revision_output dirty_output)
    set(_revision source-snapshot)
    set(_dirty unknown)
    if(AIX_FUSE_GIT_EXECUTABLE)
        execute_process(COMMAND "${AIX_FUSE_GIT_EXECUTABLE}" rev-parse --show-toplevel
            WORKING_DIRECTORY "${directory}" RESULT_VARIABLE _root_status
            OUTPUT_VARIABLE _git_root OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        get_filename_component(_source_real "${directory}" REALPATH)
        if(NOT _root_status EQUAL 0 OR NOT _git_root STREQUAL _source_real)
            set(${revision_output} "${_revision}" PARENT_SCOPE)
            set(${dirty_output} "${_dirty}" PARENT_SCOPE)
            return()
        endif()
        execute_process(COMMAND "${AIX_FUSE_GIT_EXECUTABLE}" rev-parse HEAD
            WORKING_DIRECTORY "${directory}" RESULT_VARIABLE _status
            OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(_status EQUAL 0 AND NOT _head STREQUAL "")
            set(_revision "${_head}")
            execute_process(COMMAND "${AIX_FUSE_GIT_EXECUTABLE}" status --porcelain
                WORKING_DIRECTORY "${directory}" RESULT_VARIABLE _status
                OUTPUT_VARIABLE _changes OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            if(_status EQUAL 0)
                if(_changes STREQUAL "")
                    set(_dirty no)
                else()
                    set(_dirty yes)
                endif()
            endif()
        endif()
    endif()
    set(${revision_output} "${_revision}" PARENT_SCOPE)
    set(${dirty_output} "${_dirty}" PARENT_SCOPE)
endfunction()

# A packaged source tree has no Git metadata. Retain its recorded input
# identities; packaging always hashes the actual sources again after edits.
function(aix_fuse_packaged_identity component revision_output dirty_output)
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/source-provenance.txt" _identity_lines
        REGEX "^${component}_(revision|dirty)=")
    set(_revision "")
    set(_dirty "")
    foreach(_identity_line IN LISTS _identity_lines)
        if(_identity_line MATCHES "^${component}_revision=(.+)$")
            set(_revision "${CMAKE_MATCH_1}")
        elseif(_identity_line MATCHES "^${component}_dirty=(yes|no|unknown)$")
            set(_dirty "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(_revision STREQUAL "" OR _dirty STREQUAL "")
        message(FATAL_ERROR "The packaged source provenance lacks the ${component} input identity")
    endif()
    set(${revision_output} "${_revision}" PARENT_SCOPE)
    set(${dirty_output} "${_dirty}" PARENT_SCOPE)
endfunction()

aix_fuse_source_identity("${CMAKE_CURRENT_SOURCE_DIR}" _adapter_revision _adapter_dirty)
aix_fuse_source_identity("${AIX_FUSE_USFS_RESOLVED_SOURCE_DIR}" _native_revision _native_dirty)
if(AIX_FUSE_USFS_SOURCE_KIND STREQUAL bundled AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/source-provenance.txt")
    if(_adapter_revision STREQUAL source-snapshot)
        aix_fuse_packaged_identity(adapter _adapter_revision _adapter_dirty)
    endif()
    if(_native_revision STREQUAL source-snapshot)
        aix_fuse_packaged_identity(native _native_revision _native_dirty)
    endif()
endif()
set(AIX_FUSE_BUILD_REVISION "${_adapter_revision}" CACHE STRING "Adapter revision recorded in artifacts")
set(AIX_FUSE_BUILD_DIRTY "${_adapter_dirty}" CACHE STRING "Adapter worktree state recorded in artifacts")
set(AIX_FUSE_USFS_BUILD_REVISION "${_native_revision}" CACHE STRING "Native source revision recorded in artifacts")
set(AIX_FUSE_USFS_BUILD_DIRTY "${_native_dirty}" CACHE STRING "Native worktree state recorded in artifacts")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(packaging/fuse3.pc.in "${CMAKE_CURRENT_BINARY_DIR}/generated/fuse3.pc" @ONLY)

function(aix_fuse_target target)
    target_compile_options(${target} PRIVATE -maix64 -Wall -Wextra -Werror)
    target_compile_definitions(${target} PRIVATE DEBUG)
    target_link_options(${target} PRIVATE -maix64)
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${AIX_FUSE_USFS_RESOLVED_SOURCE_DIR}/src/client/include")
endfunction()
function(aix_fuse_output target directory name)
    set_target_properties(${target} PROPERTIES OUTPUT_NAME "${name}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${directory}"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${directory}")
endfunction()

add_library(_aix_fuse3 STATIC EXCLUDE_FROM_ALL
    src/fuse_adapter.c src/fuse_opt.c src/fuse_log.c
    $<TARGET_OBJECTS:USFS::client_objects>)
aix_fuse_target(_aix_fuse3)
target_link_libraries(_aix_fuse3 INTERFACE pthreads)
target_compile_options(_aix_fuse3 INTERFACE -maix64)
target_link_options(_aix_fuse3 INTERFACE -maix64)
target_include_directories(_aix_fuse3 INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(_aix_fuse3 PROPERTIES PREFIX "lib" SUFFIX ".a")
aix_fuse_output(_aix_fuse3 bin fuse3)

foreach(_example IN ITEMS mirror memfs)
    add_executable(_aix_fuse_${_example} EXCLUDE_FROM_ALL "examples/${_example}.c")
    aix_fuse_target(_aix_fuse_${_example})
    target_link_libraries(_aix_fuse_${_example} PRIVATE _aix_fuse3)
    aix_fuse_output(_aix_fuse_${_example} bin "aix_fuse_${_example}")
endforeach()

foreach(_unit IN ITEMS fuse_opt_test fuse_log_test)
    add_executable(_aix_${_unit} EXCLUDE_FROM_ALL
        tests/harness/tap.c "tests/unit/${_unit}.c")
    aix_fuse_target(_aix_${_unit})
    target_include_directories(_aix_${_unit} PRIVATE tests/harness)
    target_link_libraries(_aix_${_unit} PRIVATE _aix_fuse3)
    aix_fuse_output(_aix_${_unit} tests/unit "${_unit}")
endforeach()

add_library(_aix_fuse_adapter_seam OBJECT EXCLUDE_FROM_ALL src/fuse_adapter.c)
aix_fuse_target(_aix_fuse_adapter_seam)
target_compile_definitions(_aix_fuse_adapter_seam PRIVATE
    calloc=adapter_test_calloc)
add_executable(_aix_fuse_adapter_test EXCLUDE_FROM_ALL
    tests/harness/tap.c tests/unit/fuse_adapter_test.c
    src/fuse_opt.c src/fuse_log.c $<TARGET_OBJECTS:_aix_fuse_adapter_seam>)
aix_fuse_target(_aix_fuse_adapter_test)
target_include_directories(_aix_fuse_adapter_test PRIVATE tests/harness)
target_link_libraries(_aix_fuse_adapter_test PRIVATE pthreads)
aix_fuse_output(_aix_fuse_adapter_test tests/unit fuse_adapter_test)

add_library(_aix_fuse_opt_alloc_seam OBJECT EXCLUDE_FROM_ALL src/fuse_opt.c)
aix_fuse_target(_aix_fuse_opt_alloc_seam)
target_compile_definitions(_aix_fuse_opt_alloc_seam PRIVATE
    _POSIX_C_SOURCE=200809L USFS_ALLOC_SEAM)
add_executable(_aix_fuse_opt_alloc_test EXCLUDE_FROM_ALL
    tests/harness/tap.c tests/unit/fuse_opt_alloc_test.c src/fuse_log.c
    $<TARGET_OBJECTS:_aix_fuse_opt_alloc_seam>)
aix_fuse_target(_aix_fuse_opt_alloc_test)
target_compile_definitions(_aix_fuse_opt_alloc_test PRIVATE
    _POSIX_C_SOURCE=200809L USFS_AIX_ALLOC_SEAM)
target_include_directories(_aix_fuse_opt_alloc_test PRIVATE tests/harness)
target_link_libraries(_aix_fuse_opt_alloc_test PRIVATE pthreads)
aix_fuse_output(_aix_fuse_opt_alloc_test tests/unit fuse_opt_alloc_test)

foreach(_helper IN ITEMS mt_daemon path_daemon io_probe)
    add_executable(_aix_fuse_${_helper} EXCLUDE_FROM_ALL "tests/helpers/${_helper}.c")
    aix_fuse_target(_aix_fuse_${_helper})
    target_include_directories(_aix_fuse_${_helper} PRIVATE tests/harness)
    if(NOT _helper STREQUAL io_probe)
        target_link_libraries(_aix_fuse_${_helper} PRIVATE _aix_fuse3)
    endif()
    if(_helper STREQUAL path_daemon)
        target_sources(_aix_fuse_${_helper} PRIVATE tests/harness/tap.c)
    endif()
    aix_fuse_output(_aix_fuse_${_helper} tests/helpers "${_helper}")
endforeach()

set(AIX_FUSE_RPM_FILE
    "aix-fuse-${AIX_FUSE_PACKAGE_VERSION}-${AIX_FUSE_PACKAGE_RELEASE}.aix${CMAKE_HOST_SYSTEM_VERSION}.ppc.rpm")
add_custom_target(package ALL
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/aix/build-package.sh"
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}"
        "${AIX_FUSE_USFS_RESOLVED_SOURCE_DIR}"
        "${AIX_FUSE_PACKAGE_VERSION}" "${AIX_FUSE_PACKAGE_RELEASE}"
        "${AIX_FUSE_BUILD_REVISION}" "${AIX_FUSE_BUILD_DIRTY}"
        "${AIX_FUSE_USFS_BUILD_REVISION}" "${AIX_FUSE_USFS_BUILD_DIRTY}"
        "${AIX_FUSE_USFS_SOURCE_KIND}" "${CMAKE_HOST_SYSTEM_VERSION}"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/packages/${AIX_FUSE_RPM_FILE}"
        "${CMAKE_CURRENT_BINARY_DIR}/packages/${AIX_FUSE_RPM_FILE}.sha256"
    DEPENDS _aix_fuse3 _aix_fuse_mirror _aix_fuse_memfs
    VERBATIM)
add_custom_target(test
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/aix/run-target.sh"
        test "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS _aix_fuse_adapter_test _aix_fuse_opt_test
        _aix_fuse_opt_alloc_test _aix_fuse_log_test
    USES_TERMINAL VERBATIM)
add_custom_target(test-e2e
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/aix/run-target.sh"
        test-e2e "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS _aix_fuse_mirror _aix_fuse_memfs
        _aix_fuse_mt_daemon _aix_fuse_path_daemon _aix_fuse_io_probe
    USES_TERMINAL VERBATIM)
message(STATUS "Native USFS source: ${AIX_FUSE_USFS_RESOLVED_SOURCE_DIR} (${AIX_FUSE_USFS_SOURCE_KIND})")
message(STATUS "aix-fuse targets: package (default), test, test-e2e")
