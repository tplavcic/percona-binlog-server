# Copyright (c) 2026 Percona and/or its affiliates.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License, version 2.0, for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

# Property-based tests (Hegel, https://hegel.dev/cpp) - enabled with
# -DWITH_PROPERTY_TESTS=ON.
#
# Hegel is located in this order:
#  1. an installed package: pass -Dhegel_DIR=<prefix>/lib/cmake/hegel
#     (extra/qa/setup_pbt_env.sh installs one into $WORKDIR/hegel-install)
#  2. otherwise, when PBS_HEGEL_FETCH=ON (default), it is fetched from
#     GitHub at configure time (needs network access)

set(PBS_HEGEL_VERSION "0.13.1" CACHE STRING "hegel-cpp version for property tests")
option(PBS_HEGEL_FETCH "Fetch hegel-cpp from GitHub if no installed package is found" ON)

find_package(hegel ${PBS_HEGEL_VERSION} CONFIG QUIET)
if(hegel_FOUND)
  message(STATUS "Property tests: using installed hegel ${hegel_VERSION} (${hegel_DIR})")
elseif(PBS_HEGEL_FETCH)
  message(STATUS "Property tests: fetching hegel-cpp v${PBS_HEGEL_VERSION}")
  include(FetchContent)
  set(HEGEL_REFLECTION OFF CACHE BOOL "" FORCE)
  set(HEGEL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(pbs_hegel_system_arg "")
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.25)
    # treat hegel headers as system headers (no -Werror noise from them)
    set(pbs_hegel_system_arg SYSTEM)
  endif()
  FetchContent_Declare(hegel
    GIT_REPOSITORY https://github.com/hegeldev/hegel-cpp.git
    GIT_TAG v${PBS_HEGEL_VERSION}
    ${pbs_hegel_system_arg}
  )
  FetchContent_MakeAvailable(hegel)
  if(NOT TARGET hegel::hegel)
    add_library(hegel::hegel ALIAS hegel)
  endif()
else()
  message(FATAL_ERROR
    "Property tests: hegel-cpp not found. Run extra/qa/setup_pbt_env.sh and "
    "pass -Dhegel_DIR=<workdir>/hegel-install/lib/cmake/hegel, or enable "
    "PBS_HEGEL_FETCH.")
endif()

find_package(Boost REQUIRED COMPONENTS unit_test_framework)

# pbs_add_property_test(<name> SOURCES <files...> LIBRARIES <targets...>)
#
# Creates the <name> executable, registers it with CTest under the
# "property" label and makes it runnable on its own:
#   ./<name>                          # all properties of the executable
#   ./<name> --run_test=<Property>    # a single property
#   ./<name> --list_content           # list the properties
function(pbs_add_property_test name)
  cmake_parse_arguments(arg "" "" "SOURCES;LIBRARIES" ${ARGN})
  add_executable(${name} ${arg_SOURCES})
  target_include_directories(${name}
    PRIVATE
      "${PROJECT_SOURCE_DIR}/src"
      "${CMAKE_CURRENT_SOURCE_DIR}"
  )
  target_link_libraries(${name}
    PRIVATE
      binlog_server_compiler_flags
      ${arg_LIBRARIES}
      hegel::hegel
      Boost::unit_test_framework
  )
  # Hegel generator parameters are designated-initializer structs where
  # leaving fields out is the intended usage
  target_compile_options(${name} PRIVATE
    "$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Wno-missing-field-initializers>"
  )
  set_target_properties(${name} PROPERTIES
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
  )
  add_test(NAME ${name} COMMAND ${name} --no_color_output)
  set_tests_properties(${name} PROPERTIES LABELS "property")
endfunction()

pbs_add_property_test(gtid_set_property_test
  SOURCES gtid_set_property_test.cpp
  LIBRARIES binsrv::lib_gtids binsrv::lib_util
)

pbs_add_property_test(byte_span_codec_property_test
  SOURCES byte_span_codec_property_test.cpp
  LIBRARIES binsrv::lib_util
)

pbs_add_property_test(cipher_context_property_test
  SOURCES cipher_context_property_test.cpp
  # OpenSSL::Crypto: the test uses OpenSSL's EVP API directly as its oracle
  LIBRARIES binsrv::lib_opensslpp binsrv::lib_util OpenSSL::Crypto
)

pbs_add_property_test(composite_binlog_name_property_test
  SOURCES composite_binlog_name_property_test.cpp
  LIBRARIES binsrv::lib_events binsrv::lib_util
)

pbs_add_property_test(event_property_test
  SOURCES event_property_test.cpp
  LIBRARIES binsrv::lib_events binsrv::lib_gtids binsrv::lib_util ZLIB::ZLIB
)

pbs_add_property_test(reader_context_property_test
  SOURCES reader_context_property_test.cpp
  LIBRARIES binsrv::lib_events binsrv::lib_gtids binsrv::lib_util ZLIB::ZLIB
)

pbs_add_property_test(rewriter_property_test
  SOURCES rewriter_property_test.cpp
  LIBRARIES binsrv::lib_events binsrv::lib_gtids binsrv::lib_util ZLIB::ZLIB
)
