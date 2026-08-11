# Library CMakeLists.txt for EDSParser

# Build-time git stamp. Regenerated on every build (not only at configure time)
# so a binary always reports the commit it was really built from; configure_file
# inside the script leaves the header alone when nothing changed, so this does
# not cause spurious rebuilds.
set(EDSPARSER_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(EDSPARSER_BUILD_INFO_HPP "${EDSPARSER_GENERATED_DIR}/build_info.hpp")

add_custom_target(edsparser_build_info ALL
    COMMAND ${CMAKE_COMMAND}
        -D "GIT_SRC_DIR=${EDSPARSER_REPO_ROOT}"
        -D "IN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/build_info.hpp.in"
        -D "OUT_FILE=${EDSPARSER_BUILD_INFO_HPP}"
        -P "${EDSPARSER_REPO_ROOT}/cmake/GitVersion.cmake"
    BYPRODUCTS "${EDSPARSER_BUILD_INFO_HPP}"
    COMMENT "Stamping build with git commit"
    VERBATIM
)

# Generate once at configure time too, so the first compile has a header to
# include even before the custom target has run.
execute_process(COMMAND ${CMAKE_COMMAND}
    -D "GIT_SRC_DIR=${EDSPARSER_REPO_ROOT}"
    -D "IN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/build_info.hpp.in"
    -D "OUT_FILE=${EDSPARSER_BUILD_INFO_HPP}"
    -P "${EDSPARSER_REPO_ROOT}/cmake/GitVersion.cmake"
)

# Create library from source files
set(LIB_SOURCES
    common.cpp
    build_info.cpp
    memory_monitor.cpp
    formats/eds.cpp
    formats/sources.cpp
    transforms/eds_transforms.cpp
    transforms/eds_complexity.cpp
    transforms/msa_transforms.cpp
    transforms/vcf_transforms.cpp
)

set(LIB_HEADERS
    common.hpp
    memory_monitor.hpp
    formats/eds.hpp
    formats/sources.hpp
    transforms/eds_transforms.hpp
    transforms/msa_transforms.hpp
    transforms/vcf_transforms.hpp
)

# Create static library
add_library(edsparser_lib STATIC ${LIB_SOURCES})

# Link libraries
target_link_libraries(edsparser_lib
    ${Boost_LIBRARIES}
    ${SDSL_LIBRARY}
    ${DIVSUFSORT_LIBRARY}
    ${DIVSUFSORT64_LIBRARY}
)

# Link OpenMP if available
if(OpenMP_CXX_FOUND)
    target_link_libraries(edsparser_lib OpenMP::OpenMP_CXX)
endif()

# Link zstd if available — enables the EDZ_COMPRESSED source format
if(EDSPARSER_HAVE_ZSTD)
    target_compile_definitions(edsparser_lib PRIVATE EDSPARSER_HAVE_ZSTD)
    target_link_libraries(edsparser_lib ${ZSTD_LIBRARY})
endif()

# Set include directories for the library
target_include_directories(edsparser_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${EDSPARSER_GENERATED_DIR}
)

# The stamp header must exist before any library source compiles.
add_dependencies(edsparser_lib edsparser_build_info)

# Install targets
install(TARGETS edsparser_lib
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

# Install headers with directory structure preserved
install(FILES common.hpp memory_monitor.hpp
    DESTINATION include/edsparser
)

install(FILES
    formats/eds.hpp
    formats/sources.hpp
    DESTINATION include/edsparser/formats
)

install(FILES
    transforms/eds_transforms.hpp
    transforms/msa_transforms.hpp
    transforms/vcf_transforms.hpp
    DESTINATION include/edsparser/transforms
)
