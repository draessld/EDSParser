# Root CMakeLists.txt for EDSParser
cmake_minimum_required(VERSION 3.10)
project(EDSParser VERSION 1.0.0 LANGUAGES CXX)

# Include GNUInstallDirs for standard installation directories
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Add the C++ source directory
add_subdirectory(src/cpp)

# Configure and install the CMake package config file
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/EDSParserConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/EDSParserConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/EDSParser"
)

# Generate version file
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/EDSParserConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

# Install CMake config files
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/EDSParserConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/EDSParserConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/EDSParser"
)
