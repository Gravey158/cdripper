cmake_minimum_required(VERSION 3.16)
project(cdripper CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(CDIO     REQUIRED libcdio)
pkg_check_modules(CDDA     REQUIRED libcdio_cdda)
pkg_check_modules(PARANOIA REQUIRED libcdio_paranoia)
pkg_check_modules(DISCID   REQUIRED libdiscid)
find_package(CURL REQUIRED)

# nlohmann/json: bevorzugt CMake-Paket, sonst System-Header (Debian: nlohmann-json3-dev)
find_package(nlohmann_json QUIET)

set(CORE_SRC engine.cpp pipeline.cpp cli.cpp main.cpp)

# Qt optional — ohne Qt wird ein reines CLI-Binary gebaut.
find_package(Qt6 QUIET COMPONENTS Widgets)
if(NOT Qt6_FOUND)
  find_package(Qt5 QUIET COMPONENTS Widgets)
endif()

set(QT_LIBS "")
if(Qt6_FOUND OR Qt5_FOUND)
  set(CMAKE_AUTOMOC ON)
  list(APPEND CORE_SRC gui.cpp)
  if(Qt6_FOUND)
    set(QT_LIBS Qt6::Widgets)
  else()
    set(QT_LIBS Qt5::Widgets)
  endif()
  message(STATUS "Qt gefunden — GUI + CLI werden gebaut.")
else()
  message(WARNING "Kein Qt gefunden — es wird NUR das CLI-Binary gebaut. "
                  "Für die GUI: 'sudo apt install qt6-base-dev' (oder qtbase5-dev).")
endif()

add_executable(cdripper ${CORE_SRC})

if(Qt6_FOUND OR Qt5_FOUND)
  target_compile_definitions(cdripper PRIVATE HAVE_QT=1)
  target_link_libraries(cdripper PRIVATE ${QT_LIBS})
endif()

target_include_directories(cdripper PRIVATE
  ${CDIO_INCLUDE_DIRS} ${CDDA_INCLUDE_DIRS}
  ${PARANOIA_INCLUDE_DIRS} ${DISCID_INCLUDE_DIRS})
target_link_libraries(cdripper PRIVATE
  ${CDIO_LIBRARIES} ${CDDA_LIBRARIES} ${PARANOIA_LIBRARIES}
  ${DISCID_LIBRARIES} CURL::libcurl pthread)

if(nlohmann_json_FOUND)
  target_link_libraries(cdripper PRIVATE nlohmann_json::nlohmann_json)
endif()

install(TARGETS cdripper RUNTIME DESTINATION bin)

# ── Unit-Tests (T1) ────────────────────────────────────────────────────────────
# Baut nicht beim Default-Build mit; explizit: cmake --build build --target
# cdripper_tests && ./build/cdripper_tests   (oder ctest).
enable_testing()
add_executable(cdripper_tests EXCLUDE_FROM_ALL tests/tests.cpp engine.cpp)
target_include_directories(cdripper_tests PRIVATE
  ${CDIO_INCLUDE_DIRS} ${CDDA_INCLUDE_DIRS}
  ${PARANOIA_INCLUDE_DIRS} ${DISCID_INCLUDE_DIRS})
target_link_libraries(cdripper_tests PRIVATE
  ${CDIO_LIBRARIES} ${CDDA_LIBRARIES} ${PARANOIA_LIBRARIES}
  ${DISCID_LIBRARIES} CURL::libcurl pthread)
if(nlohmann_json_FOUND)
  target_link_libraries(cdripper_tests PRIVATE nlohmann_json::nlohmann_json)
endif()
add_test(NAME unit COMMAND cdripper_tests)
