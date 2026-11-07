cmake_minimum_required(VERSION 3.21)
project(StrataHistorianOdbc VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(WIN32)
  add_compile_definitions(NOMINMAX)  # windows.h otherwise defines min and max as macros
endif()

add_library(strata_core STATIC
  src/core/timeutil.cpp src/core/json.cpp src/core/catalog.cpp src/core/sql.cpp src/core/planner.cpp
  src/backend/fixture_backend.cpp src/backend/piwebapi_backend.cpp src/backend/winhttp_transport.cpp src/backend/loopback_transport.cpp
  src/odbc/config.cpp)
if(MSVC)
  target_compile_options(strata_core PRIVATE /W4 /permissive- /utf-8)
  # Static CRT: no VC++ redistributable prerequisite on gateway nodes.
  set_property(TARGET strata_core PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  target_compile_options(strata_core PRIVATE -Wall -Wextra)
endif()
find_package(Threads REQUIRED)
target_link_libraries(strata_core PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(strata_core PUBLIC winhttp)
endif()

# Driver manager headers/libs: Windows SDK on Windows; unixODBC via ODBC_PREFIX elsewhere (tests only).
set(ODBC_PREFIX "" CACHE PATH "unixODBC prefix for non-Windows test builds")

add_library(strataodbc SHARED src/odbc/driver.cpp src/odbc/setup_win.cpp)
target_link_libraries(strataodbc PRIVATE strata_core)
if(WIN32)
  enable_language(RC)
  target_sources(strataodbc PRIVATE src/odbc/strataodbc.def src/odbc/version.rc)
  target_link_libraries(strataodbc PRIVATE odbccp32 user32)
  if(MSVC)
    target_link_libraries(strataodbc PRIVATE legacy_stdio_definitions)  # odbccp32.lib needs it
    set_property(TARGET strataodbc PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
elseif(ODBC_PREFIX)
  target_include_directories(strataodbc PRIVATE ${ODBC_PREFIX}/include)
  target_link_libraries(strataodbc PRIVATE ${ODBC_PREFIX}/lib/libodbcinst${CMAKE_SHARED_LIBRARY_SUFFIX})
  target_compile_definitions(strataodbc PRIVATE SIZEOF_LONG_INT=8 HAVE_LONG_LONG=1)
endif()
set_target_properties(strataodbc PROPERTIES PREFIX "" OUTPUT_NAME strataodbc)

option(STRATA_BUILD_TESTS "Build tests" ON)
if(STRATA_BUILD_TESTS)
  enable_testing()
  add_executable(core_tests tests/core_tests.cpp)
  target_link_libraries(core_tests PRIVATE strata_core)
  add_executable(strata_explain tools/strata_explain.cpp)
  target_link_libraries(strata_explain PRIVATE strata_core)
  if(MSVC)
    set_property(TARGET core_tests strata_explain PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
  add_test(NAME core_tests COMMAND core_tests)

  add_executable(odbc_smoke tests/odbc_smoke.cpp)
  if(WIN32)
    target_link_libraries(odbc_smoke PRIVATE odbc32)
    if(MSVC)
      set_property(TARGET odbc_smoke PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
  elseif(ODBC_PREFIX)
    target_include_directories(odbc_smoke PRIVATE ${ODBC_PREFIX}/include)
    target_link_libraries(odbc_smoke PRIVATE ${ODBC_PREFIX}/lib/libodbc${CMAKE_SHARED_LIBRARY_SUFFIX} Threads::Threads)
    target_compile_definitions(odbc_smoke PRIVATE SIZEOF_LONG_INT=8 HAVE_LONG_LONG=1)
  endif()
endif()
