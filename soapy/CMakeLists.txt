enable_language(CXX)
find_package(SoapySDR CONFIG REQUIRED)

SOAPY_SDR_MODULE_UTIL(
    TARGET birriasdrSupport
    SOURCES BirriaSDR.cpp
    LIBRARIES birriasdr)
set_target_properties(birriasdrSupport PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

# Modules install to <libdir>/SoapySDR/modules<ABI>; find libbirriasdr two levels up.
file(RELATIVE_PATH BIRRIA_MODULE_TO_LIB
    "${CMAKE_INSTALL_FULL_LIBDIR}/SoapySDR/modules${SOAPY_SDR_ABI_VERSION}" "${CMAKE_INSTALL_FULL_LIBDIR}")
if(APPLE)
    set_target_properties(birriasdrSupport PROPERTIES INSTALL_RPATH "@loader_path/${BIRRIA_MODULE_TO_LIB}")
elseif(UNIX)
    set_target_properties(birriasdrSupport PROPERTIES INSTALL_RPATH "$ORIGIN/${BIRRIA_MODULE_TO_LIB}")
endif()

if(BUILD_TESTING)
    add_executable(test_soapy_bands tests/test_bands.cpp)
    target_include_directories(test_soapy_bands PRIVATE ${PROJECT_SOURCE_DIR}/include)
    set_target_properties(test_soapy_bands PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    add_test(NAME soapy_bands COMMAND test_soapy_bands)
endif()
