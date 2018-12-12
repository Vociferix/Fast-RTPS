# Copyright 2016 Proyectos y Sistemas de Mantenimiento SL (eProsima).
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 2.8.12)

if(NOT CMAKE_VERSION VERSION_LESS 3.0)
    cmake_policy(SET CMP0048 NEW)
endif()

project(AllocationTest)

# Find requirements
if(NOT fastcdr_FOUND)
    find_package(fastcdr REQUIRED)
endif()

if(NOT fastrtps_FOUND)
    find_package(fastrtps REQUIRED)
endif()

# Set C++11
include(CheckCXXCompilerFlag)
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_COMPILER_IS_CLANG OR
        CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    check_cxx_compiler_flag(-std=c++11 SUPPORTS_CXX11)
    if(SUPPORTS_CXX11)
        add_compile_options(-std=c++11)
    else()
        message(FATAL_ERROR "Compiler doesn't support C++11")
    endif()
endif()

message(STATUS "Configuring AllocationTest...")
file(GLOB ALLOCTEST_EXAMPLE_SOURCES_CXX "*.cxx")
file(GLOB ALLOCTEST_EXAMPLE_SOURCES_CPP "*.cpp")

file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/test_xml_profiles.xml
    DESTINATION ${CMAKE_CURRENT_BINARY_DIR}
    )
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/test.sh
    DESTINATION ${CMAKE_CURRENT_BINARY_DIR}
    )

add_executable(AllocationTest ${ALLOCTEST_EXAMPLE_SOURCES_CXX} ${ALLOCTEST_EXAMPLE_SOURCES_CPP})
target_link_libraries(AllocationTest fastrtps fastcdr)
install(TARGETS AllocationTest
    RUNTIME DESTINATION test/profiling/allocations/${BIN_INSTALL_DIR})