if(NOT SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required")
endif()
if(EXISTS "${SOURCE_ROOT}/src/iiSocialIntegration" OR EXISTS "${SOURCE_ROOT}/src/iiSocialPublish")
    message(FATAL_ERROR "Source responsibilities must not repeat public package names")
endif()
foreach(_file Publishing/Publishing.h Publishing/Publishing.cpp Review/Review.h Review/Review.cpp
        Migration/Migration.h Migration/Migration.cpp Import/UrlImport.h Import/UrlImport.cpp
        Transport/CurlRuntime.h Transport/CurlRuntime.cpp Transport/CurlTransport.cpp
        Compatibility/Publishing.h Compatibility/Review.h Compatibility/Compatibility.h)
    if(NOT EXISTS "${SOURCE_ROOT}/src/${_file}")
        message(FATAL_ERROR "Missing source in its responsibility directory: ${_file}")
    endif()
endforeach()
