cmake_minimum_required(VERSION 3.5)
project(nlohmann_json_schema_validator_vendor)

find_package(ament_cmake REQUIRED)

option(FORCE_BUILD_VENDOR_PKG
  "Build nlohmann_json_schema_validator from source, even if system-install package is available"
  OFF
)

if(NOT FORCE_BUILD_VENDOR_PKG)
  find_package(nlohmann_json_schema_validator QUIET)
endif()


if(NOT nlohmann_json_schema_validator_FOUND)

  set(setup_install_dir "${CMAKE_CURRENT_BINARY_DIR}/nlohmann_json_schema_validator")

  include(ExternalProject)
  ExternalProject_Add(nlohmann_json_schema_validator
    PREFIX nlohmann_json_schema_validator
    URL https://github.com/pboettch/json-schema-validator/archive/refs/tags/2.1.0.zip
    URL_MD5 9542b03326e5a7ed8fdd6548192f9474
    CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX=${setup_install_dir}
      -DBUILD_SHARED_LIBS=true
    INSTALL_DIR ${setup_install_dir}
  )

  install(
    DIRECTORY ${setup_install_dir}/
    DESTINATION ${CMAKE_INSTALL_PREFIX}
    USE_SOURCE_PERMISSIONS
  )
endif()

ament_export_dependencies(nlohmann_json_schema_validator)

ament_package()
