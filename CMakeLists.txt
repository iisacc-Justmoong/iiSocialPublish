cmake_minimum_required(VERSION 3.24)
project(iiSocialIntegration VERSION 0.2.0 LANGUAGES CXX)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)
find_package(CURL 7.68 REQUIRED)
find_package(LibXml2 2.9 REQUIRED)
find_package(Threads REQUIRED)
set(_social_public_headers
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Publishing/Publishing.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Review/Review.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Migration/Migration.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Import/UrlImport.h"
)
foreach(_header IN LISTS _social_public_headers)
    get_filename_component(_name "${_header}" NAME)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/include/iiSocialIntegration/${_name}"
        CONTENT "#pragma once\n#include \"${_header}\"\n")
endforeach()
foreach(_name Publishing.h Review.h Compatibility.h)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/include/iiSocialPublish/${_name}"
        CONTENT "#pragma once\n#include \"${CMAKE_CURRENT_SOURCE_DIR}/src/Compatibility/${_name}\"\n")
endforeach()
add_library(iiSocialIntegration STATIC src/Publishing/Publishing.cpp src/Transport/CurlTransport.cpp src/Transport/CurlRuntime.cpp src/Review/Review.cpp src/Migration/Migration.cpp src/Import/UrlImport.cpp)
add_library(iiSocialIntegration::iiSocialIntegration ALIAS iiSocialIntegration)
add_library(iiSocialPublish::iiSocialPublish ALIAS iiSocialIntegration)
set_target_properties(iiSocialIntegration PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
target_compile_features(iiSocialIntegration PUBLIC cxx_std_23)
target_include_directories(iiSocialIntegration PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE third_party src/Transport)
target_link_libraries(iiSocialIntegration PRIVATE CURL::libcurl LibXml2::LibXml2 Threads::Threads)
if(MSVC)
    target_compile_options(iiSocialIntegration PUBLIC /utf-8 PRIVATE /W4 /permissive-)
else()
    target_compile_options(iiSocialIntegration PRIVATE -Wall -Wextra -Wpedantic)
endif()
set(_package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/iiSocialIntegration")
install(TARGETS iiSocialIntegration EXPORT iiSocialIntegrationTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ${_social_public_headers}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iiSocialIntegration)
install(FILES src/Compatibility/Publishing.h src/Compatibility/Review.h src/Compatibility/Compatibility.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iiSocialPublish)
install(EXPORT iiSocialIntegrationTargets NAMESPACE iiSocialIntegration:: DESTINATION "${_package_dir}")
configure_package_config_file(cmake/iiSocialIntegrationConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/iiSocialIntegrationConfig.cmake" INSTALL_DESTINATION "${_package_dir}")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/iiSocialIntegrationConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/iiSocialIntegrationConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/iiSocialIntegrationConfigVersion.cmake" DESTINATION "${_package_dir}")
set(_legacy_package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/iiSocialPublish")
configure_package_config_file(cmake/iiSocialPublishConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/iiSocialPublishConfig.cmake" INSTALL_DESTINATION "${_legacy_package_dir}")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/iiSocialPublishConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/iiSocialPublishConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/iiSocialPublishConfigVersion.cmake" DESTINATION "${_legacy_package_dir}")
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSocialIntegration)
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSocialIntegration/docs)
install(DIRECTORY third_party/licenses/ DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/iiSocialIntegration)
install(FILES third_party/dependencies.json DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/iiSocialIntegration)
if(BUILD_TESTING)
    add_test(NAME iiSocialIntegration.SourceLayout COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/source_layout.cmake")
    add_executable(iiSocialIntegrationReviewTests tests/ReviewTest.cpp)
    target_link_libraries(iiSocialIntegrationReviewTests PRIVATE iiSocialIntegration::iiSocialIntegration)
    target_include_directories(iiSocialIntegrationReviewTests PRIVATE third_party)
    add_test(NAME iiSocialIntegration.Review COMMAND iiSocialIntegrationReviewTests)
    add_executable(iiSocialIntegrationTests tests/PublishingTest.cpp)
    target_link_libraries(iiSocialIntegrationTests PRIVATE iiSocialIntegration::iiSocialIntegration)
    target_include_directories(iiSocialIntegrationTests PRIVATE third_party)
    foreach(_case catalog validation x facebook instagram threads pinterest tumblr mastodon linkedin youtube youtube_validation youtube_errors errors transport)
        add_test(NAME iiSocialIntegration.${_case} COMMAND iiSocialIntegrationTests ${_case})
    endforeach()
    add_executable(iiSocialIntegrationMigrationTests tests/MigrationTest.cpp)
    target_link_libraries(iiSocialIntegrationMigrationTests PRIVATE iiSocialIntegration::iiSocialIntegration)
    target_include_directories(iiSocialIntegrationMigrationTests PRIVATE third_party)
    add_test(NAME iiSocialIntegration.Migration COMMAND iiSocialIntegrationMigrationTests)
    add_executable(iiSocialIntegrationCompatibilityTests tests/CompatibilityTest.cpp)
    target_link_libraries(iiSocialIntegrationCompatibilityTests PRIVATE iiSocialPublish::iiSocialPublish)
    add_test(NAME iiSocialIntegration.Compatibility COMMAND iiSocialIntegrationCompatibilityTests)
    add_executable(iiSocialIntegrationUrlImportTests tests/UrlImportTest.cpp)
    target_link_libraries(iiSocialIntegrationUrlImportTests PRIVATE iiSocialIntegration::iiSocialIntegration)
    add_test(NAME iiSocialIntegration.UrlImport COMMAND iiSocialIntegrationUrlImportTests)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    find_program(IISOCIALINTEGRATION_OPENSSL openssl REQUIRED)
    add_test(NAME iiSocialIntegration.TlsLoopback
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/TransportLoopback.py"
            "$<TARGET_FILE:iiSocialIntegrationTests>" "${CMAKE_CURRENT_BINARY_DIR}/tls-loopback" "${IISOCIALINTEGRATION_OPENSSL}"
            "$<TARGET_FILE:iiSocialIntegrationUrlImportTests>")
    set_tests_properties(iiSocialIntegration.TlsLoopback PROPERTIES TIMEOUT 60)
endif()
