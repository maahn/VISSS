cmake_minimum_required(VERSION 3.22)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS 1)

cmake_path(SET PLUGIN_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")

project("record" VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_COMPILE_WARNING_AS_ERROR ON)
# Compiler flags are added with target_compile_options because we don't want to add to all the targets.
if(MSVC)
	list(APPEND COMPILER_FLAGS /MP /W3)
else()
	list(APPEND COMPILER_FLAGS -Wall -Wextra -Wshadow -Wnon-virtual-dtor -Wpedantic)
endif(MSVC)

message(STATUS "The following flags will be passed to the compiler: ${COMPILER_FLAGS}")

# Compile-time git tag/branch for the metadata .txt header (matches the old pipeline's own
# compile-time GIT_TAG/GIT_BRANCH, PROCESSING_SPEC_teeldyne.md §3.24). --always falls back to an
# abbreviated commit hash if the repo has no tags yet, instead of failing.
find_package(Git QUIET)
if(GIT_FOUND)
	execute_process(
		COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		OUTPUT_VARIABLE VISSS_GIT_TAG
		OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_QUIET
	)
	execute_process(
		COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		OUTPUT_VARIABLE VISSS_GIT_BRANCH
		OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_QUIET
	)
endif()
if(NOT VISSS_GIT_TAG)
	set(VISSS_GIT_TAG "unknown")
endif()
if(NOT VISSS_GIT_BRANCH)
	set(VISSS_GIT_BRANCH "unknown")
endif()
message(STATUS "Using VISSS_GIT_TAG = ${VISSS_GIT_TAG}, VISSS_GIT_BRANCH = ${VISSS_GIT_BRANCH}")

# Set both eSdk and eSdkPro lib and include paths
cmake_path(SET ESDK_INCLUDE_PATH "$ENV{EMERGENT_DIR}/eSDK/include/")
cmake_path(SET ESDK_PRO_LIB_PATH "$ENV{ECAPTURE_PRO_DIR}/eSdkPro/lib/")
cmake_path(SET ESDK_PRO_INCLUDE_PATH "$ENV{ECAPTURE_PRO_DIR}/eSdkPro/include/")

message(STATUS "Using ESDK_INCLUDE_PATH = ${ESDK_INCLUDE_PATH}")
message(STATUS "Using ESDK_PRO_LIB_PATH = ${ESDK_PRO_LIB_PATH}")
message(STATUS "Using ESDK_PRO_INCLUDE_PATH = ${ESDK_PRO_INCLUDE_PATH}")

# EVT libraries
find_library(ESDK_PRO_LIB REQUIRED NAMES eSdkPro PATHS "${ESDK_PRO_LIB_PATH}" NO_DEFAULT_PATH)

# RecordTask uses the CUDA runtime API directly (cudaMemcpy2D/cudaMemset2D) - no .cu kernel file
# needed for the mono8->nv12 conversion, so plain C++ compilation is enough.
find_package(CUDAToolkit REQUIRED)

# System FFmpeg (already installed as a runtime dependency of eCapturePro itself) - used directly
# for hevc_nvenc encode + mp4 mux, replacing eSDK Pro's built-in NvencTask. See recordtask.h for
# why.
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil)

# Plugins are loaded as a shared library by eCaptureProServer, not compiled into a client app.
add_library("${PROJECT_NAME}" SHARED)

# Add library include directories
include_directories(SYSTEM "${ESDK_INCLUDE_PATH}")
include_directories(SYSTEM "${ESDK_PRO_INCLUDE_PATH}")
include_directories(SYSTEM "${CUDAToolkit_INCLUDE_DIRS}")

# Add source subdirectory
add_subdirectory("${PLUGIN_SOURCE_DIR}")

# Add libraries/dependencies to build and link with libraries and executables.
target_link_libraries("${PROJECT_NAME}"
    PUBLIC ${ESDK_PRO_LIB}
    PUBLIC CUDA::cudart
    PUBLIC PkgConfig::FFMPEG
)

# Compile flags for C++ compiler only for the specified target
target_compile_options("${PROJECT_NAME}" PUBLIC $<$<COMPILE_LANGUAGE:CXX>:${COMPILER_FLAGS}>)

target_compile_definitions("${PROJECT_NAME}" PRIVATE
	VISSS_GIT_TAG="${VISSS_GIT_TAG}"
	VISSS_GIT_BRANCH="${VISSS_GIT_BRANCH}"
)
