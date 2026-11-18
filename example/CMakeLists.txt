cmake_minimum_required(VERSION 3.24)
project(iiSocialIntegrationInstalledExample LANGUAGES CXX)
# Resolve the old package first, so its dependency must find the new package.
find_package(iiSocialPublish 0.2 CONFIG REQUIRED)
find_package(iiSocialIntegration 0.2 CONFIG REQUIRED)
if(NOT EXISTS "${iiSocialIntegration_LICENSE_DIR}/dependencies.json" OR
   NOT EXISTS "${iiSocialPublish_LICENSE_DIR}/nlohmann-json/LICENSE.MIT" OR
   NOT EXISTS "${iiSocialIntegration_LICENSE_DIR}/LICENSE" OR
   NOT EXISTS "${iiSocialIntegration_LICENSE_DIR}/NOTICE")
    message(FATAL_ERROR "Installed package license directories are incomplete")
endif()
file(READ "${iiSocialIntegration_LICENSE_DIR}/LICENSE" _installed_license)
file(READ "${iiSocialIntegration_LICENSE_DIR}/NOTICE" _installed_notice)
if(NOT _installed_license MATCHES "Apache License" OR
   NOT _installed_license MATCHES "Version 2.0, January 2004" OR
   NOT _installed_notice MATCHES "Copyright \\(c\\) 2026 iisacc")
    message(FATAL_ERROR "Installed first-party license text is not Apache-2.0")
endif()
if(NOT iiSocialIntegration_SPDX_LICENSE STREQUAL "Apache-2.0" OR
   NOT iiSocialPublish_SPDX_LICENSE STREQUAL "Apache-2.0")
    message(FATAL_ERROR "Installed packages do not export SPDX license Apache-2.0")
endif()
add_executable(MigrationExample Migration.cpp)
target_link_libraries(MigrationExample PRIVATE iiSocialIntegration::iiSocialIntegration)
add_executable(LegacyCompatibility ../tests/CompatibilityTest.cpp)
target_link_libraries(LegacyCompatibility PRIVATE iiSocialPublish::iiSocialPublish)
enable_testing()
add_test(NAME MigrationExample COMMAND MigrationExample)
add_test(NAME LegacyCompatibility COMMAND LegacyCompatibility)
