cmake_minimum_required(VERSION 3.25)

project(
    podman_manager
    VERSION 0.1.0
    DESCRIPTION "MVP root orchestrator library for rootless Podman users"
    LANGUAGES CXX)

option(PODMAN_MANAGER_BUILD_EXAMPLES "Build example services" ON)
option(PODMAN_MANAGER_BUILD_TESTS "Build tests" ON)
option(PODMAN_MANAGER_WITH_SDBUS "Build the sdbus-c++ systemd backend when available" ON)

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)

if(PODMAN_MANAGER_WITH_SDBUS AND PkgConfig_FOUND)
    pkg_check_modules(SDBUSCPP QUIET IMPORTED_TARGET sdbus-c++)
endif()

add_library(
    podman_manager
    src/deployment.cpp
    src/container_spec.cpp
    src/error.cpp
    src/http.cpp
    src/podman_client.cpp
    src/quadlet.cpp
    src/socket_validation.cpp
    src/systemd.cpp
    src/target.cpp)

if(SDBUSCPP_FOUND)
    target_sources(podman_manager PRIVATE src/systemd_sdbus.cpp)
endif()

add_library(podman_manager::podman_manager ALIAS podman_manager)

target_compile_features(podman_manager PUBLIC cxx_std_23)
target_include_directories(
    podman_manager
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(podman_manager PUBLIC CURL::libcurl Threads::Threads)

if(SDBUSCPP_FOUND)
    target_link_libraries(podman_manager PUBLIC PkgConfig::SDBUSCPP)
    target_compile_definitions(podman_manager PUBLIC PODMAN_MANAGER_HAS_SDBUS=1)
else()
    target_compile_definitions(podman_manager PUBLIC PODMAN_MANAGER_HAS_SDBUS=0)
endif()

target_compile_options(
    podman_manager
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>)

if(PODMAN_MANAGER_BUILD_EXAMPLES)
    add_executable(podman_manager_example_service examples/orchestrator_service.cpp)
    target_compile_features(podman_manager_example_service PRIVATE cxx_std_23)
    target_link_libraries(podman_manager_example_service PRIVATE podman_manager::podman_manager)
    target_compile_options(
        podman_manager_example_service
        PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>)
endif()

if(PODMAN_MANAGER_BUILD_TESTS)
    enable_testing()

    add_executable(podman_manager_tests tests/podman_manager_tests.cpp)
    target_compile_features(podman_manager_tests PRIVATE cxx_std_23)
    target_link_libraries(podman_manager_tests PRIVATE podman_manager::podman_manager)
    target_compile_options(
        podman_manager_tests
        PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>)

    add_test(NAME podman_manager_tests COMMAND podman_manager_tests)
endif()
