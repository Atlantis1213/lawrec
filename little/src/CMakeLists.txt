cmake_minimum_required(VERSION 3.10)
project(lawrec C CXX)

set(CMAKE_INSTALL_PREFIX "/app/lawrec")
set(CMAKE_INSTALL_LIBDIR "/usr/lib")
set(CMAKE_INSTALL_BINDIR "/usr/bin")

option(DEMO_BUILD "Build demo code" OFF)
option(LAWREC_BUILD_RTSP "Build standalone RTSP helper from buildroot package" OFF)
option(LAWREC_BUILD_SERVICE "Build service from buildroot package" OFF)

add_subdirectory(ui)
add_subdirectory(xiaodemo)

if(LAWREC_BUILD_SERVICE)
    add_subdirectory(service)
endif()

if(LAWREC_BUILD_RTSP)
    add_subdirectory(rtsp)
endif()

message(STATUS "CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
message(STATUS "CMAKE_C_FLAGS=${CMAKE_C_FLAGS}")
message(STATUS "CMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}")
