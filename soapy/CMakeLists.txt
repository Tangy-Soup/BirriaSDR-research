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
