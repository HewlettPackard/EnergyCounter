SET(CMAKE_C_FLAGS "-O3")

OPTION(DEBUG "Debug mode." OFF)

# use cmake -D DEBUG:BOOL=TRUE
IF(DEBUG)
    SET(CMAKE_C_FLAGS "-O0 -g -fsanitize=address -fno-omit-frame-pointer")
ENDIF(DEBUG)

SET(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# Check if NVIDIA GPU support should be enabled
FIND_LIBRARY(DCGM_LIB dcgm)
if(DCGM_LIB AND NOT DEFINED DISABLE_GPU_NVIDIA)
    MESSAGE(STATUS "Enabling NVIDIA GPU support")
    SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DNVIDIA_GPU")
else()
    MESSAGE(STATUS "Disabling NVIDIA GPU support")
    SET(DCGM_LIB "")
    SET(DISABLE_GPU_NVIDIA "")
endif()

# Check if AMD GPU support should be enabled
FIND_LIBRARY(ROCM_LIB
             NAMES rocm_smi64
             HINTS "/opt/rocm/rocm_smi/lib")
if(ROCM_LIB AND NOT DEFINED DISABLE_GPU_AMD)
    GET_FILENAME_COMPONENT(ROCM_DIR ${ROCM_LIB}"/../../" ABSOLUTE)
    MESSAGE(STATUS "Enabling AMD GPU support")
    SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DAMD_GPU")
    INCLUDE_DIRECTORIES(${ROCM_DIR}"/include")
else()
    MESSAGE(STATUS "Disabling AMD GPU support")
    SET(ROCM_LIB "")
    SET(DISABLE_GPU_AMD "")
endif()

# Check if CPU package support should be enabled
if(NOT DEFINED DISABLE_CPU)
    MESSAGE(STATUS "Enabling CPU package support")
    SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DCPU_PACKAGE")
else()
    MESSAGE(STATUS "Disabling CPU package support")
    SET(DISABLE_CPU "")
endif()

# Check if DRAM support should be enabled
if(NOT DEFINED DISABLE_DRAM)
    MESSAGE(STATUS "Enabling DRAM support")
    SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DDRAM_PACKAGE")
else()
    MESSAGE(STATUS "Disabling DRAM support")
    SET(DISABLE_DRAM "")
endif()

INCLUDE_DIRECTORIES("${PROJECT_BINARY_DIR}")

# Add source files
FILE(GLOB SOURCES "*.c")

ADD_EXECUTABLE(ecounter ${SOURCES})

TARGET_LINK_LIBRARIES(ecounter ${DCGM_LIB} ${ROCM_LIB} m)

INSTALL(TARGETS ecounter DESTINATION ${CMAKE_INSTALL_PREFIX})
