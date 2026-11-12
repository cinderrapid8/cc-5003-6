cmake_minimum_required(VERSION 3.21)
project(delay_broadcast LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# Vendored Qt 6 sysroot (Debian layout) and locally extracted ALSA packages
# can be supplied without touching system directories.
if(DEFINED LOCAL_QT_SYSROOT)
    list(PREPEND CMAKE_PREFIX_PATH "${LOCAL_QT_SYSROOT}/usr")
    list(PREPEND CMAKE_FIND_ROOT_PATH "${LOCAL_QT_SYSROOT}")
    set(ALSA_HINT "${LOCAL_ALSA_PREFIX}")
endif()

find_package(Threads REQUIRED)

# ALSA is optional: when absent, the audible path is the WAV renderer and
# delay-play reports the missing device clearly.
find_library(ASOUND_LIBRARY
    NAMES asound
    HINTS "${LOCAL_ALSA_PREFIX}/lib/aarch64-linux-gnu"
          "${LOCAL_ALSA_PREFIX}/lib/x86_64-linux-gnu"
          "${LOCAL_ALSA_PREFIX}/lib")
find_path(ASOUND_INCLUDE_DIR
    NAMES alsa/asoundlib.h
    HINTS "${LOCAL_ALSA_PREFIX}/include")

set(CORE_SOURCES
    src/logger.cpp
    src/wav_file.cpp
    src/pads.cpp
    src/delay_engine.cpp
    src/script_runner.cpp)

add_library(delay_core STATIC ${CORE_SOURCES})
target_include_directories(delay_core PUBLIC src)
target_link_libraries(delay_core PUBLIC Threads::Threads)

# Test suite (no Qt dependency).
add_executable(delay-tests
    tests/test_main.cpp
    tests/test_framework.cpp
    tests/engine_tests.cpp
    tests/replay_model_tests.cpp)
target_include_directories(delay-tests PRIVATE src tests)
target_link_libraries(delay-tests PRIVATE delay_core)

# Standalone automated acceptance entry for ring-buffer regression and the
# independent replacement reference model, including fault-injection mutants.
add_executable(ring-acceptance tests/ring_acceptance_main.cpp)
target_include_directories(ring-acceptance PRIVATE src tests)
target_link_libraries(ring-acceptance PRIVATE delay_core)

enable_testing()
add_test(NAME engine-suite COMMAND delay-tests)
add_test(NAME ring-acceptance-suite COMMAND ring-acceptance)

# Offline simulator / scenario renderer.
add_executable(delay-sim tools/delay_sim.cpp)
target_link_libraries(delay-sim PRIVATE delay_core)

# WAV playback tool.
if(ASOUND_LIBRARY AND ASOUND_INCLUDE_DIR)
    add_executable(delay-play tools/delay_play.cpp src/audio_sink.cpp)
    target_include_directories(delay-play PRIVATE "${ASOUND_INCLUDE_DIR}")
    target_link_libraries(delay-play PRIVATE delay_core "${ASOUND_LIBRARY}")
    target_compile_definitions(delay-play PRIVATE DBS_HAVE_ALSA=1)
else()
    add_executable(delay-play tools/delay_play.cpp)
    target_link_libraries(delay-play PRIVATE delay_core)
endif()

# Qt widget interface (skipped when Qt is unavailable).
find_package(Qt6 QUIET COMPONENTS Widgets Concurrent)
if(Qt6_FOUND)
    qt_standard_project_setup()
    add_executable(delay-gui
        src/gui_main.cpp
        src/engine_controller.cpp
        src/timeline_widget.cpp
        src/main_window.cpp)
    find_library(DBUS_LIBRARY NAMES dbus-1
        HINTS "${LOCAL_QT_SYSROOT}/usr/lib/aarch64-linux-gnu"
              "${LOCAL_QT_SYSROOT}/lib/aarch64-linux-gnu")
    target_link_libraries(delay-gui PRIVATE delay_core Qt6::Widgets
                                            Qt6::Concurrent)
    if(DBUS_LIBRARY)
        target_link_libraries(delay-gui PRIVATE "${DBUS_LIBRARY}")
    endif()
    message(STATUS "Qt6 found: building delay-gui")

    # Offscreen smoke driver (not installed); invokes every MainWindow slot.
    add_executable(gui-smoke
        tools/gui_smoke.cpp
        src/engine_controller.cpp
        src/timeline_widget.cpp
        src/main_window.cpp)
    target_link_libraries(gui-smoke PRIVATE delay_core Qt6::Widgets
                                             Qt6::Concurrent)
    if(DBUS_LIBRARY)
        target_link_libraries(gui-smoke PRIVATE "${DBUS_LIBRARY}")
    endif()
else()
    message(WARNING "Qt6 not found: delay-gui will not be built")
endif()
