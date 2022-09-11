cmake_minimum_required(VERSION 3.5)
project(pybind11_json_vendor)

find_package(ament_cmake REQUIRED)

option(FORCE_BUILD_VENDOR_PKG
  "Build pybind11_json from source, even if system-install package is available"
  OFF
)

if(NOT FORCE_BUILD_VENDOR_PKG)
  find_package(pybind11_json QUIET)
endif()


if(NOT pybind11_json_FOUND)

  set(setup_install_dir "${CMAKE_CURRENT_BINARY_DIR}/pybind11_json")

  include(ExternalProject)
  ExternalProject_Add(pybind11_json
    GIT_REPOSITORY https://github.com/pybind/pybind11_json.git
    GIT_TAG 0.2.11
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

ament_export_dependencies(pybind11_json)

ament_package()

