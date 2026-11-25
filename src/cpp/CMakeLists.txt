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

# Repo root, resolved from this file rather than CMAKE_SOURCE_DIR so it is
# correct whether the build is configured at the repo root, at src/cpp, or as a
# submodule of another project (biofmi).
get_filename_component(EDSPARSER_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/lib)
include_directories(${CMAKE_BINARY_DIR}/generated)
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

# Every unit test states its expectations with assert(). A Release build defines
# NDEBUG, which compiles those away — the suite then prints "All tests passed"
# without checking anything, and Release is what INSTALL.sh builds. Undefine it
# for the test targets only, so the suite is meaningful in every build type.
function(edsparser_add_unit_test name)
    add_executable(${name} ${TEST_DIR}/${name}.cpp)
    target_link_libraries(${name} edsparser_lib)
    target_compile_options(${name} PRIVATE -UNDEBUG)
endfunction()

# Test: EDS parsing
edsparser_add_unit_test(test_eds)
add_test(NAME test_eds COMMAND test_eds)

# Test: Sources parsing
edsparser_add_unit_test(test_sources)
add_test(NAME test_sources COMMAND test_sources)

# Test: Statistics
edsparser_add_unit_test(test_stats)
add_test(NAME test_stats COMMAND test_stats)

# Test: Merging
edsparser_add_unit_test(test_merge)
add_test(NAME test_merge COMMAND test_merge)

# Test: MSA transformation
edsparser_add_unit_test(test_msa)
add_test(NAME test_msa COMMAND test_msa)

# Test: VCF transformation
edsparser_add_unit_test(test_vcf)
add_test(NAME test_vcf COMMAND test_vcf)

# Test: CLI tool integration tests
edsparser_add_unit_test(test_integration)
add_test(NAME test_integration COMMAND test_integration ${CMAKE_BINARY_DIR}/tools WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# Memory stress tests (built but NOT run automatically - too slow for CI)
# Run manually when needed: ./test_memory_smoke or ./test_memory_stress
edsparser_add_unit_test(test_memory_stress)
edsparser_add_unit_test(test_memory_smoke)

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
