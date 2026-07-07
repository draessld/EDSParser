# CMakeLists.txt for EDSParser
cmake_minimum_required(VERSION 3.10)
project(EDSParser VERSION 1.0.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -pedantic")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG")

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/lib)
include_directories(BEFORE ${CMAKE_INCLUDE_PATH})

# Link directories
link_directories(BEFORE ${CMAKE_LIBRARY_PATH})

# Find required packages — prefer static Boost so the binary is portable
# across machines that may have a different Boost shared-library version.
set(Boost_USE_STATIC_LIBS ON)
find_package(Boost REQUIRED COMPONENTS program_options)
include_directories(${Boost_INCLUDE_DIRS})

# Find OpenMP for parallel processing
find_package(OpenMP)
if(OpenMP_CXX_FOUND)
    message(STATUS "Found OpenMP: ${OpenMP_CXX_VERSION}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")
else()
    message(WARNING "OpenMP not found. Parallel processing will be disabled.")
endif()

# Check for SDSL (required for MSA transformations)
find_path(SDSL_INCLUDE_DIR sdsl/suffix_arrays.hpp
    PATHS
        ${CMAKE_INCLUDE_PATH}
        $ENV{HOME}/include
        /usr/local/include
        /usr/include
)

if(SDSL_INCLUDE_DIR)
    message(STATUS "Found SDSL: ${SDSL_INCLUDE_DIR}")
    include_directories(${SDSL_INCLUDE_DIR})
else()
    message(WARNING "SDSL not found. Please install from https://github.com/simongog/sdsl-lite")
endif()

find_library(SDSL_LIBRARY
    NAMES sdsl sdsl-lite
    PATHS
        ${CMAKE_LIBRARY_PATH}
        $ENV{HOME}/lib
        /usr/local/lib
        /usr/lib
)

if(SDSL_LIBRARY)
    message(STATUS "Found SDSL library: ${SDSL_LIBRARY}")
else()
    message(WARNING "SDSL library not found")
endif()

# Find divsufsort libraries (required by SDSL for SA construction)
find_library(DIVSUFSORT_LIBRARY
    NAMES divsufsort
    PATHS
        ${CMAKE_LIBRARY_PATH}
        $ENV{HOME}/lib
        /usr/local/lib
        /usr/lib
)

find_library(DIVSUFSORT64_LIBRARY
    NAMES divsufsort64
    PATHS
        ${CMAKE_LIBRARY_PATH}
        $ENV{HOME}/lib
        /usr/local/lib
        /usr/lib
)

if(DIVSUFSORT_LIBRARY)
    message(STATUS "Found divsufsort library: ${DIVSUFSORT_LIBRARY}")
else()
    message(WARNING "divsufsort library not found")
endif()

if(DIVSUFSORT64_LIBRARY)
    message(STATUS "Found divsufsort64 library: ${DIVSUFSORT64_LIBRARY}")
else()
    message(WARNING "divsufsort64 library not found")
endif()

# Find zstd (optional, required for the EDZ_COMPRESSED source format).
# Searches the usual custom/conda locations first, then system paths.
find_path(ZSTD_INCLUDE_DIR zstd.h
    PATHS
        ${CMAKE_INCLUDE_PATH}
        $ENV{CONDA_PREFIX}/include
        $ENV{HOME}/include
        /usr/local/include
        /usr/include
)
find_library(ZSTD_LIBRARY
    NAMES zstd
    PATHS
        ${CMAKE_LIBRARY_PATH}
        $ENV{CONDA_PREFIX}/lib
        $ENV{HOME}/lib
        /usr/local/lib
        /usr/lib
)

if(ZSTD_INCLUDE_DIR AND ZSTD_LIBRARY)
    message(STATUS "Found zstd: ${ZSTD_LIBRARY} (EDZ_COMPRESSED enabled)")
    set(EDSPARSER_HAVE_ZSTD ON)
    include_directories(${ZSTD_INCLUDE_DIR})
else()
    message(STATUS "zstd not found — EDZ_COMPRESSED source format will be disabled")
    set(EDSPARSER_HAVE_ZSTD OFF)
endif()

# Enable testing
enable_testing()

# Add subdirectories
add_subdirectory(lib)
add_subdirectory(tools)

# Add test executables
set(TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/unit)

# Test: EDS parsing
add_executable(test_eds ${TEST_DIR}/test_eds.cpp)
target_link_libraries(test_eds edsparser_lib)
add_test(NAME test_eds COMMAND test_eds)

# Test: Sources parsing
add_executable(test_sources ${TEST_DIR}/test_sources.cpp)
target_link_libraries(test_sources edsparser_lib)
add_test(NAME test_sources COMMAND test_sources)

# Test: Statistics
add_executable(test_stats ${TEST_DIR}/test_stats.cpp)
target_link_libraries(test_stats edsparser_lib)
add_test(NAME test_stats COMMAND test_stats)

# Test: Merging
add_executable(test_merge ${TEST_DIR}/test_merge.cpp)
target_link_libraries(test_merge edsparser_lib)
add_test(NAME test_merge COMMAND test_merge)

# Test: MSA transformation
add_executable(test_msa ${TEST_DIR}/test_msa.cpp)
target_link_libraries(test_msa edsparser_lib)
add_test(NAME test_msa COMMAND test_msa)

# Test: VCF transformation
add_executable(test_vcf ${TEST_DIR}/test_vcf.cpp)
target_link_libraries(test_vcf edsparser_lib)
add_test(NAME test_vcf COMMAND test_vcf)

# Test: CLI tool integration tests
add_executable(test_integration ${TEST_DIR}/test_integration.cpp)
target_link_libraries(test_integration edsparser_lib)
add_test(NAME test_integration COMMAND test_integration ${CMAKE_BINARY_DIR}/tools WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# Memory stress tests (built but NOT run automatically - too slow for CI)
# Run manually when needed: ./test_memory_smoke or ./test_memory_stress
add_executable(test_memory_stress ${TEST_DIR}/test_memory_stress.cpp)
target_link_libraries(test_memory_stress edsparser_lib)

add_executable(test_memory_smoke ${TEST_DIR}/test_memory_smoke.cpp)
target_link_libraries(test_memory_smoke edsparser_lib)

# Note: These are excluded from 'ctest' / 'make test'
# To run: cd build/src/cpp && ./test_memory_smoke

# Print configuration summary
message(STATUS "")
message(STATUS "Configuration Summary:")
message(STATUS "  CMake version       : ${CMAKE_VERSION}")
message(STATUS "  Compiler            : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Build type          : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard        : ${CMAKE_CXX_STANDARD}")
message(STATUS "  Install prefix      : ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  Boost version       : ${Boost_VERSION}")
message(STATUS "  EDZ_COMPRESSED (zstd): ${EDSPARSER_HAVE_ZSTD}")
message(STATUS "")
