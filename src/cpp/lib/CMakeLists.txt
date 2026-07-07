# Library CMakeLists.txt for EDSParser

# Create library from source files
set(LIB_SOURCES
    common.cpp
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
)

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
