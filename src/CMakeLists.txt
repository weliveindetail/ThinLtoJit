cmake_minimum_required(VERSION 3.2)

project(ThinLtoJit VERSION 1.0 LANGUAGES CXX)

message(STATUS "System: ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_VERSION}")
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "Windows")
    set(DISABLE_RTTI_FLAG /GR-)
else()
    set(DISABLE_RTTI_FLAG -fno-rtti)
endif()

message("")
message("Dependencies")
add_executable(ThinLtoJit
    main.cpp
    BitcodeCacheEntry.h
    Errors.h
    Errors.cpp
    SimpleOrcJit.h
    SimpleOrcJit.cpp
    ThinLtoJit.h
    ThinLtoJit.cpp
)

set_target_properties(ThinLtoJit PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# add LLVM libraries, set LLVM_* variables
message(STATUS "LLVM_DIR: ${LLVM_DIR}")
find_package(LLVM 6.0 REQUIRED PATHS ${LLVM_DIR})

string(REPLACE "." ";" SEMVER_LIST "${LLVM_PACKAGE_VERSION}")
list(GET SEMVER_LIST 0 LLVM_PACKAGE_VERSION_MAJOR)
list(GET SEMVER_LIST 1 LLVM_PACKAGE_VERSION_MINOR)
set(LLVM_PACKAGE_VERSION_SHORT "${LLVM_PACKAGE_VERSION_MAJOR}.${LLVM_PACKAGE_VERSION_MINOR}")

message(STATUS "LLVM_PACKAGE_VERSION: ${LLVM_PACKAGE_VERSION}")
message(STATUS "LLVM_PACKAGE_VERSION_SHORT: ${LLVM_PACKAGE_VERSION_SHORT}")

# Build compatible to LLVM
if(NOT LLVM_ENABLE_RTTI)
    target_compile_options(ThinLtoJit PRIVATE ${DISABLE_RTTI_FLAG})
endif()

# LLVM definitions
separate_arguments(LLVM_DEFINITIONS)
target_compile_definitions(ThinLtoJit PRIVATE ${LLVM_DEFINITIONS})

# Find path to Clang libraries
if(EXISTS ${LLVM_BUILD_MAIN_SRC_DIR}/tools/clang)
    set(CLANG_SRC_DIR_PREFIX tools) # sources in-tree (default build)
elseif(EXISTS ${LLVM_BUILD_MAIN_SRC_DIR}/../clang)
    set(CLANG_SRC_DIR_PREFIX ..) # sources out-of-tree (ENABLE_PROJECTS build) 
else()
    message(FATAL_ERROR "Cannot find Clang sources")
endif()
set(CLANG_BUILD_MAIN_SRC_DIR "${LLVM_BUILD_MAIN_SRC_DIR}/${CLANG_SRC_DIR_PREFIX}/clang")

# Find path to LLD libraries
if(EXISTS ${LLVM_BUILD_MAIN_SRC_DIR}/tools/lld)
    set(LLD_SRC_DIR_PREFIX tools) # sources in-tree (default build)
elseif(EXISTS ${LLVM_BUILD_MAIN_SRC_DIR}/../lld)
    set(LLD_SRC_DIR_PREFIX ..) # sources out-of-tree (ENABLE_PROJECTS build) 
else()
    message(FATAL_ERROR "Cannot find LLD sources")
endif()
set(LLD_BUILD_MAIN_SRC_DIR "${LLVM_BUILD_MAIN_SRC_DIR}/${LLD_SRC_DIR_PREFIX}/lld")

message(STATUS "LLVM Source Directory: ${LLVM_BUILD_MAIN_SRC_DIR}")
message(STATUS "Clang Source Directory: ${CLANG_BUILD_MAIN_SRC_DIR}")
message(STATUS "LLD Source Directory: ${LLD_BUILD_MAIN_SRC_DIR}")
message(STATUS "LLVM Common Build Directory: ${LLVM_BUILD_BINARY_DIR}")

target_include_directories(ThinLtoJit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LLVM_INCLUDE_DIRS}
    ${LLD_BUILD_MAIN_SRC_DIR}/..
    ${LLD_BUILD_MAIN_SRC_DIR}/include
)

set(CMAKE_MODULE_PATH
    "${CMAKE_MODULE_PATH}"
    "${LLVM_BUILD_MAIN_SRC_DIR}/cmake/modules/"
    "${LLD_BUILD_MAIN_SRC_DIR}/cmake/modules/"
)

# link to shared libLLVM for fast development buils
add_library(libLLVM SHARED IMPORTED)
set(libLLVM_DIR "${LLVM_BUILD_BINARY_DIR}/lib")
set(libLLVM_NAME "libLLVM-${LLVM_PACKAGE_VERSION_SHORT}.so")
set_property(TARGET libLLVM PROPERTY IMPORTED_LOCATION "${libLLVM_DIR}/${libLLVM_NAME}")

target_link_libraries(ThinLtoJit PRIVATE libLLVM)

message("")
message("ThinLtoJit")

get_target_property(ALL_INCLUDE_DIRECTORIES ThinLtoJit INCLUDE_DIRECTORIES)
message(STATUS "Include directories: ${ALL_INCLUDE_DIRECTORIES}")

get_target_property(ALL_LINK_LIBRARIES ThinLtoJit LINK_LIBRARIES)
message(STATUS "Link libraries: ${ALL_LINK_LIBRARIES}")

get_target_property(ALL_COMPILE_OPTIONS ThinLtoJit COMPILE_OPTIONS)
message(STATUS "Compile options: ${ALL_COMPILE_OPTIONS}")

get_target_property(ALL_COMPILE_DEFINITIONS ThinLtoJit COMPILE_DEFINITIONS)
message(STATUS "Compile definitions: ${ALL_COMPILE_DEFINITIONS}")

message(STATUS "Other flags: ${CMAKE_CXX_FLAGS}")
message(STATUS "Other flags Debug: ${CMAKE_CXX_FLAGS_DEBUG}")
message(STATUS "Other flags Release: ${CMAKE_CXX_FLAGS_RELEASE}")
