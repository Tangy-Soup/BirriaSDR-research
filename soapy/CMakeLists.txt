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

if(BUILD_TESTING)
    add_executable(test_soapy_gain ../tests/test_soapy_gain.cpp $<TARGET_OBJECTS:test_soapy_gain_controls>)
    target_include_directories(test_soapy_gain PRIVATE ../include ../src ${LIBUSB_INCLUDE_DIR} ${SoapySDR_INCLUDE_DIRS})
    target_compile_definitions(test_soapy_gain PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_features(test_soapy_gain PRIVATE c_std_11 cxx_std_17)
    target_link_libraries(test_soapy_gain PRIVATE ${SoapySDR_LIBRARIES} Threads::Threads)
    add_test(NAME soapy_gain_commit COMMAND test_soapy_gain)
endif()
