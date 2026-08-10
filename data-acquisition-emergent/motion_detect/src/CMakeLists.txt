# Source files
target_sources("${PROJECT_NAME}" PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/motiondetecttask.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/kernel.cu"
)

# Header files
target_sources("${PROJECT_NAME}" PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/motiondetecttask.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/kernel.cuh"
    "${CMAKE_CURRENT_SOURCE_DIR}/font8x14.h"
)
