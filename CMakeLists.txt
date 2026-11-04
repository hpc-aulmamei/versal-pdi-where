# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2026 ARH Laboratory
#  SPDX-License-Identifier: MIT
# ##################################################################################################
cmake_minimum_required(VERSION 3.21)
project(versal-pdi-where
        VERSION 0.1.0
        DESCRIPTION "Tell which reconfigurable pblock a Versal partial PDI configures"
        HOMEPAGE_URL "https://github.com/hpc-aulmamei/versal-pdi-where"
        LANGUAGES CXX)

include(GNUInstallDirs)
include(CheckIPOSupported)

option(PDI_WHERE_WERROR "Treat compiler warnings as errors" OFF)
option(PDI_WHERE_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(PDI_WHERE_LTO "Link-time optimisation for Release and RelWithDebInfo" ON)
option(PDI_WHERE_TESTS "Register the CTest tests" ${PROJECT_IS_TOP_LEVEL})
set(PDI_WHERE_BUILDS "$ENV{BUILDS}" CACHE PATH
    "Directory of SLASH builds for the real-partial test; empty leaves that test out")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(ZLIB REQUIRED)

if(PDI_WHERE_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Generated headers: the version and install paths (configure time), the git description (every
# build, rewritten only when it changes).
set(PDI_WHERE_DATADIR "${CMAKE_INSTALL_FULL_DATADIR}/${PROJECT_NAME}")
file(RELATIVE_PATH PDI_WHERE_DATADIR_REL "${CMAKE_INSTALL_FULL_BINDIR}" "${PDI_WHERE_DATADIR}")
set(gen "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(src/config.hpp.in "${gen}/config.hpp" @ONLY)
add_custom_target(pdi_where_git
    COMMAND "${CMAKE_COMMAND}" -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${gen}/git.hpp
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/git_version.cmake"
    BYPRODUCTS "${gen}/git.hpp"
    VERBATIM)

add_library(pdi_where_core STATIC src/pdi.cpp src/floorplan.cpp)
target_include_directories(pdi_where_core PUBLIC src)
target_compile_features(pdi_where_core PUBLIC cxx_std_17)
target_link_libraries(pdi_where_core PUBLIC ZLIB::ZLIB)

add_executable(pdi_where src/main.cpp)
target_include_directories(pdi_where PRIVATE "${gen}")
target_link_libraries(pdi_where PRIVATE pdi_where_core)
add_dependencies(pdi_where pdi_where_git)

if(PDI_WHERE_LTO)
    check_ipo_supported(RESULT ipo OUTPUT ipo_msg LANGUAGES CXX)
    if(NOT ipo)
        message(STATUS "LTO not supported: ${ipo_msg}")
    endif()
endif()
foreach(t pdi_where_core pdi_where)
    set_target_properties(${t} PROPERTIES CXX_EXTENSIONS OFF)
    target_compile_options(${t} PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Wshadow>
        $<$<BOOL:${PDI_WHERE_WERROR}>:-Werror>)
    if(PDI_WHERE_LTO AND ipo)
        set_target_properties(${t} PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE ON
                                              INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
    endif()
endforeach()

# The binary looks for device/xcv80.tsv next to itself first, so a build tree runs as is.
configure_file(device/xcv80.tsv "${CMAKE_CURRENT_BINARY_DIR}/device/xcv80.tsv" COPYONLY)

install(TARGETS pdi_where RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES device/xcv80.tsv scripts/gen_device_table.tcl DESTINATION "${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME}")
install(FILES README.md LICENSE DESTINATION "${CMAKE_INSTALL_DOCDIR}")

if(PDI_WHERE_TESTS)
    enable_testing()
    set(fx "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME selftest COMMAND pdi_where --selftest)
    add_test(NAME version COMMAND pdi_where --version)
    set_tests_properties(version PROPERTIES PASS_REGULAR_EXPRESSION "^pdi_where ${PROJECT_VERSION}")
    add_test(NAME usage COMMAND pdi_where)
    set_tests_properties(usage PROPERTIES PASS_REGULAR_EXPRESSION "^usage: pdi_where")
    add_test(NAME xdc_fixtures COMMAND pdi_where --xdc ${fx}/slash-service.xdc --xdc ${fx}/slash-compute.xdc
                                                 --json "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    set_tests_properties(xdc_fixtures PROPERTIES PASS_REGULAR_EXPRESSION "LICENSE: no SMAP words")
    if(PDI_WHERE_BUILDS)
        add_test(NAME real COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_real.sh")
        set_tests_properties(real PROPERTIES
            ENVIRONMENT "BUILDS=${PDI_WHERE_BUILDS};PDI_WHERE=$<TARGET_FILE:pdi_where>"
            SKIP_RETURN_CODE 77
            TIMEOUT 600)
    endif()
    add_custom_target(check
        COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure -C $<CONFIG>
        DEPENDS pdi_where
        USES_TERMINAL)
endif()

set(CPACK_GENERATOR TGZ)
set(CPACK_PACKAGE_VENDOR "ARH Laboratory")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
include(CPack)
