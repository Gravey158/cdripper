cmake_minimum_required(VERSION 3.16)

# Version aus engine.h ableiten, damit CMake/CPack die gleiche VERSION sehen
# wie die Runtime (statt sie zwei Stellen pflegen zu müssen).
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/engine.h" _engine_h)
string(REGEX MATCH "VERSION = \"([0-9]+\\.[0-9]+\\.[0-9]+)\"" _ "${_engine_h}")
set(CDRIPPER_VERSION "${CMAKE_MATCH_1}")
if(NOT CDRIPPER_VERSION)
  set(CDRIPPER_VERSION "0.0.0")
endif()

project(cdripper VERSION ${CDRIPPER_VERSION} LANGUAGES CXX)

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

if(WIN32)
  # Subsystem:WINDOWS damit beim GUI-Doppelklick kein cmd-Fenster mit aufgeht.
  # Im --cli-Modus läuft cdripper trotzdem mit echo'd-stdout — Windows
  # redirects das korrekt wenn von cmd.exe/powershell aus gestartet.
  add_executable(cdripper WIN32 ${CORE_SRC})
else()
  add_executable(cdripper ${CORE_SRC})
endif()

if(Qt6_FOUND OR Qt5_FOUND)
  target_compile_definitions(cdripper PRIVATE HAVE_QT=1)
  target_link_libraries(cdripper PRIVATE ${QT_LIBS})
endif()

target_include_directories(cdripper PRIVATE
  ${CDIO_INCLUDE_DIRS} ${CDDA_INCLUDE_DIRS}
  ${PARANOIA_INCLUDE_DIRS} ${DISCID_INCLUDE_DIRS})
target_link_libraries(cdripper PRIVATE
  ${CDIO_LIBRARIES} ${CDDA_LIBRARIES} ${PARANOIA_LIBRARIES}
  ${DISCID_LIBRARIES} CURL::libcurl)
# Threads portabel — pthread auf POSIX, kein-op auf Windows (CRT bringt's mit).
find_package(Threads REQUIRED)
target_link_libraries(cdripper PRIVATE Threads::Threads)

if(nlohmann_json_FOUND)
  target_link_libraries(cdripper PRIVATE nlohmann_json::nlohmann_json)
endif()

# ── macOS: Sparkle Auto-Update (optional, nur wenn Framework gefetcht) ────────
# fetch-sparkle.sh muss VOR cmake gelaufen sein, sonst fällt cdripper auf
# einen Stub-Sparkle zurück (cdripper_sparkle_init = no-op). Sparkle =
# Objective-C++ → sparkle_bridge.mm aktiviert OBJCXX-Language nur auf APPLE.
if(APPLE)
  set(SPARKLE_FRAMEWORK "${CMAKE_CURRENT_SOURCE_DIR}/mac/Sparkle.framework")
  if(EXISTS "${SPARKLE_FRAMEWORK}")
    enable_language(OBJCXX)
    target_sources(cdripper PRIVATE mac/sparkle_bridge.mm)
    target_link_options(cdripper PRIVATE
      "-F${CMAKE_CURRENT_SOURCE_DIR}/mac"
      "-Wl,-rpath,@loader_path/../Frameworks")
    target_link_libraries(cdripper PRIVATE "-framework Sparkle")
    set_source_files_properties(mac/sparkle_bridge.mm PROPERTIES
      COMPILE_FLAGS "-F${CMAKE_CURRENT_SOURCE_DIR}/mac -fobjc-arc")
    message(STATUS "Sparkle gefunden — Auto-Update aktiv.")
    set(CDRIPPER_HAVE_SPARKLE TRUE)
  else()
    # Stub-File mit no-op-Funktionen, damit der extern "C"-Aufruf in
    # main.cpp linkt auch ohne Sparkle (z.B. CI ohne fetch-sparkle.sh).
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/sparkle_stub.cpp"
      "extern \"C\" void cdripper_sparkle_init() {}\n"
      "extern \"C\" void cdripper_sparkle_check_for_updates() {}\n")
    target_sources(cdripper PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/sparkle_stub.cpp")
    message(STATUS "Sparkle nicht gefunden — Auto-Update stubbed (run scripts/fetch-sparkle.sh).")
    set(CDRIPPER_HAVE_SPARKLE FALSE)
  endif()
endif()

# ── macOS .app-Bundle (Property + Icon-Resource VOR install) ──────────────────
# Auf Linux bleibt's ein normales /bin/cdripper. Beim Mac-Bundle landet das
# Binary unter cdripper.app/Contents/MacOS/cdripper; Doppelklick startet ohne
# args → cdripper sieht keine, fällt in GUI-Modus (main.cpp). CLI weiterhin
# über cdripper.app/Contents/MacOS/cdripper bzw. den symlink, den Homebrew
# Cask setzt.
if(APPLE)
  set(MACOSX_ICON_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/mac/cdripper.icns")
  set_target_properties(cdripper PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/mac/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME "CD Ripper"
    MACOSX_BUNDLE_BUNDLE_VERSION "${CDRIPPER_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${CDRIPPER_VERSION}"
    MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.gravey158.cdripper"
    MACOSX_BUNDLE_ICON_FILE "cdripper.icns"
  )
  target_sources(cdripper PRIVATE "${MACOSX_ICON_SOURCE}")
  set_source_files_properties("${MACOSX_ICON_SOURCE}" PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources")
endif()

# Install — APPLE = .app im DMG-Root; sonst /bin/cdripper.
if(APPLE)
  install(TARGETS cdripper BUNDLE DESTINATION .)
else()
  install(TARGETS cdripper RUNTIME DESTINATION bin)
endif()

# CPack DragNDrop → cdripper-VERSION-macos-ARCH.dmg
if(APPLE)
  set(CPACK_GENERATOR "DragNDrop")
  set(CPACK_PACKAGE_NAME "cdripper")
  set(CPACK_PACKAGE_VENDOR "Gravey158")
  set(CPACK_PACKAGE_VERSION "${CDRIPPER_VERSION}")
  set(CPACK_PACKAGE_FILE_NAME "cdripper-${CDRIPPER_VERSION}-macos-${CMAKE_SYSTEM_PROCESSOR}")
  set(CPACK_DMG_VOLUME_NAME "CD Ripper ${CDRIPPER_VERSION}")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Audio CD ripper with MusicBrainz tagging")
  include(CPack)
endif()

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
  ${DISCID_LIBRARIES} CURL::libcurl Threads::Threads)
if(nlohmann_json_FOUND)
  target_link_libraries(cdripper_tests PRIVATE nlohmann_json::nlohmann_json)
endif()
add_test(NAME unit COMMAND cdripper_tests)
