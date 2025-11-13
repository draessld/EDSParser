# Tools CMakeLists.txt for EDSParser

# Transformation tools
add_executable(eds2leds eds2leds.cpp)
target_link_libraries(eds2leds edsparser_lib ${Boost_LIBRARIES} ${SDSL_LIBRARY})

add_executable(msa2eds msa2eds.cpp)
target_link_libraries(msa2eds edsparser_lib ${Boost_LIBRARIES} ${SDSL_LIBRARY})

add_executable(vcf2eds vcf2eds.cpp)
target_link_libraries(vcf2eds edsparser_lib ${Boost_LIBRARIES} ${SDSL_LIBRARY})

# Stats tool
add_executable(edsparser-stats stats.cpp)
target_link_libraries(edsparser-stats edsparser_lib ${Boost_LIBRARIES} ${SDSL_LIBRARY})

# Generate patterns tool
add_executable(edsparser-genpatterns genpatterns.cpp)
target_link_libraries(edsparser-genpatterns edsparser_lib ${Boost_LIBRARIES} ${SDSL_LIBRARY})

# Install targets
install(TARGETS
    eds2leds
    msa2eds
    vcf2eds
    edsparser-stats
    edsparser-genpatterns
    RUNTIME DESTINATION bin
)
