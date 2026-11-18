cmake_minimum_required(VERSION 3.24)
project(iiSocialIntegration VERSION 0.2.0 LANGUAGES CXX)
set(iiSocialIntegration_SPDX_LICENSE "Apache-2.0")
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)
option(BUILD_SHARED_LIBS "Build iiSocialIntegration as a shared library" ON)
find_package(CURL 7.68 REQUIRED)
find_package(LibXml2 2.9 REQUIRED)
find_package(Threads REQUIRED)
set(_social_public_headers
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Publishing/Publishing.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Review/Review.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Migration/Migration.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Import/UrlImport.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/GraphQL/GraphQL.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/GraphQL/GraphQLC.h"
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
add_library(iiSocialIntegration src/Publishing/Publishing.cpp src/Transport/CurlTransport.cpp src/Transport/CurlRuntime.cpp src/Review/Review.cpp src/Migration/Migration.cpp src/Import/UrlImport.cpp src/GraphQL/GraphQL.cpp src/GraphQL/GraphQLC.cpp)
add_library(iiSocialIntegration::iiSocialIntegration ALIAS iiSocialIntegration)
add_library(iiSocialPublish::iiSocialPublish ALIAS iiSocialIntegration)
set_target_properties(iiSocialIntegration PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON WINDOWS_EXPORT_ALL_SYMBOLS ON)
if(BUILD_SHARED_LIBS AND CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set_target_properties(iiSocialIntegration PROPERTIES FRAMEWORK TRUE FRAMEWORK_VERSION A
        MACOSX_FRAMEWORK_IDENTIFIER "com.iisacc.iiSocialIntegration")
elseif(BUILD_SHARED_LIBS AND APPLE)
    set_property(TARGET iiSocialIntegration APPEND PROPERTY INSTALL_RPATH "@loader_path")
elseif(BUILD_SHARED_LIBS AND UNIX)
    set_property(TARGET iiSocialIntegration APPEND PROPERTY INSTALL_RPATH "$ORIGIN")
endif()
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
install(TARGETS iiSocialIntegration EXPORT iiSocialIntegrationTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} FRAMEWORK DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ${_social_public_headers}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iiSocialIntegration)
install(FILES src/GraphQL/iiSocialIntegration.py
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/python)
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
install(FILES README.md LICENSE NOTICE DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSocialIntegration)
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSocialIntegration/docs)
install(FILES LICENSE NOTICE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/iiSocialIntegration)
install(DIRECTORY third_party/licenses/ DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/iiSocialIntegration)
install(FILES third_party/dependencies.json DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/iiSocialIntegration)
if(BUILD_TESTING)
    add_executable(iiSocialIntegrationGraphQLTests tests/GraphQLTest.cpp)
    target_link_libraries(iiSocialIntegrationGraphQLTests PRIVATE iiSocialIntegration::iiSocialIntegration)
    target_include_directories(iiSocialIntegrationGraphQLTests PRIVATE third_party)
    add_test(NAME iiSocialIntegration.GraphQL COMMAND iiSocialIntegrationGraphQLTests)
    add_test(NAME iiSocialIntegration.SourceLayout COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/source_layout.cmake")
    add_test(NAME iiSocialIntegration.License COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DINTEGRATION_CONFIG=${CMAKE_CURRENT_BINARY_DIR}/iiSocialIntegrationConfig.cmake"
        "-DPUBLISH_CONFIG=${CMAKE_CURRENT_BINARY_DIR}/iiSocialPublishConfig.cmake"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/license.cmake")
    add_executable(iiSocialIntegrationReviewTests tests/ReviewTest.cpp)
    target_link_libraries(iiSocialIntegrationReviewTests PRIVATE iiSocialIntegration::iiSocialIntegration)
    target_compile_definitions(iiSocialIntegrationReviewTests PRIVATE "IISOCIAL_TEST_RESOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/resources/test-fixtures\"")
    target_include_directories(iiSocialIntegrationReviewTests PRIVATE third_party)
    add_test(NAME iiSocialIntegration.Review COMMAND iiSocialIntegrationReviewTests)
    add_executable(iiSocialIntegrationTests tests/PublishingTest.cpp)
    target_link_libraries(iiSocialIntegrationTests PRIVATE iiSocialIntegration::iiSocialIntegration)
    if(BUILD_SHARED_LIBS AND NOT CMAKE_CROSSCOMPILING)
        add_test(NAME iiSocialIntegration.shared_link COMMAND ${CMAKE_COMMAND}
            "-DIMAGE=$<TARGET_FILE:iiSocialIntegrationTests>"
            "-DEXPECTED=$<TARGET_FILE:iiSocialIntegration>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/shared_link.cmake")
    endif()
    target_compile_definitions(iiSocialIntegrationTests PRIVATE "IISOCIAL_TEST_RESOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/resources/test-fixtures\"")
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
    add_test(NAME iiSocialIntegration.GraphQLPython
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/GraphQLPythonTest.py"
            "$<TARGET_FILE:iiSocialIntegration>" "${CMAKE_CURRENT_SOURCE_DIR}/src/GraphQL")
    add_test(NAME iiSocialIntegration.TlsLoopback
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/TransportLoopback.py"
            "$<TARGET_FILE:iiSocialIntegrationTests>" "${CMAKE_CURRENT_SOURCE_DIR}/resources/test-fixtures/certificate.pem"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/test-fixtures/key.pem"
            "$<TARGET_FILE:iiSocialIntegrationUrlImportTests>"
            "$<TARGET_FILE:iiSocialIntegration>" "${CMAKE_CURRENT_SOURCE_DIR}/src/GraphQL")
    set_tests_properties(iiSocialIntegration.TlsLoopback PROPERTIES TIMEOUT 60)
    add_test(NAME iiSocialIntegration.ResourceBoundary COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}" "-DBUILD_ROOT=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/ResourceBoundary.cmake")
    set_tests_properties(iiSocialIntegration.ResourceBoundary PROPERTIES
        DEPENDS "iiSocialIntegration.Review;iiSocialIntegration.youtube;iiSocialIntegration.TlsLoopback")
endif()
