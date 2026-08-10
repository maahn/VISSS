cmake_minimum_required(VERSION 3.22)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS 1)

cmake_path(SET SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")

project("visss-data-acquisition-EVT" VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_COMPILE_WARNING_AS_ERROR ON)
# Compiler flags are added with target_compile_options because we don't want to add to all the targets.
if(MSVC)
	list(APPEND COMPILER_FLAGS /MP /W3)
else()
	list(APPEND COMPILER_FLAGS -Wall -Wextra -Wshadow -Wnon-virtual-dtor -Wpedantic)
endif(MSVC)

message(STATUS "The following flags will be passed to the compiler: ${COMPILER_FLAGS}")

# Set both eSdk and eSdkPro lib and include paths
cmake_path(SET ESDK_INCLUDE_PATH "$ENV{EMERGENT_DIR}/eSDK/include/")
cmake_path(SET ESDK_PRO_LIB_PATH "$ENV{ECAPTURE_PRO_DIR}/eSdkPro/lib/")
cmake_path(SET ESDK_PRO_INCLUDE_PATH "$ENV{ECAPTURE_PRO_DIR}/eSdkPro/include/")

message(STATUS "Using ESDK_INCLUDE_PATH = ${ESDK_INCLUDE_PATH}")
message(STATUS "Using ESDK_PRO_LIB_PATH = ${ESDK_PRO_LIB_PATH}")
message(STATUS "Using ESDK_PRO_INCLUDE_PATH = ${ESDK_PRO_INCLUDE_PATH}")

# EVT libraries
find_library(ESDK_PRO_LIB REQUIRED NAMES eSdkPro PATHS "${ESDK_PRO_LIB_PATH}" NO_DEFAULT_PATH)

# OpenCV highgui, for the live-preview GUI window (§3.25) - cv::imshow, matching the old
# pipeline's own approach exactly rather than something bespoke.
find_package(PkgConfig REQUIRED)
pkg_check_modules(OPENCV REQUIRED IMPORTED_TARGET opencv4)

# Create an executable target for this project
add_executable("${PROJECT_NAME}" "${SOURCE_DIR}/main.cpp")

# Add library include directories
include_directories(SYSTEM "${ESDK_PRO_INCLUDE_PATH}")
include_directories(SYSTEM "${ESDK_INCLUDE_PATH}")

# Add libraries/dependencies to build and link with libraries and executables.
target_link_libraries("${PROJECT_NAME}"
    PRIVATE ${ESDK_PRO_LIB}
    PRIVATE PkgConfig::OPENCV
)

# Compile flags for C++ compiler only for the specified target
target_compile_options("${PROJECT_NAME}" PUBLIC $<$<COMPILE_LANGUAGE:CXX>:${COMPILER_FLAGS}>)