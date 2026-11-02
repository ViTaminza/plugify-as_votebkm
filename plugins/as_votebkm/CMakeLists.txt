# as_votebkm — Plugify port of the Pisex admin_system module "[AS] VoteBKM"
# (player votes for ban / kick / mute / gag).
#
#   as_votebkm_core       engine-free config/command/path/vote-math/phrase logic (also used by
#                         tests/as_votebkm_core_tests.cpp)
#   plugify-as_votebkm    the plugin shared library (entry "bin/as_votebkm" -> bin/libas_votebkm.so)
#
# Pure s2sdk: no pisexcompat, no s2guard, no shared wrapper library. hudmenu is an optional
# plugin dependency (not a wrapper) used only for the player's own "!menu" rendering choice.
# (docs/PLUGIN_STANDARD.md §7). The guards of handoff/weaponskins/PITFALLS.md live in
# src/plugin.cpp itself - the B1/B2 map gate, the C2 client checks, the update-scheduler result
# (no s2sdk timers), the C10/C11 menu/command ownership and the A7 teardown. Like every
# admin_system module the .so compiles the binding globals the plugify module fills at load time:
#   * s2sdk (__s2sdk_*),
#   * the generated admin_system consumer bindings (__admin_system_*) plus the hand-written
#     consumer wrapper plugins/admin_system/include/admin_api.hpp.

set(AS_VOTEBKM_VERSION "1.1.0")

# --- core ---------------------------------------------------------------------
add_library(as_votebkm_core STATIC src/votebkm_core.cpp)
target_include_directories(as_votebkm_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(as_votebkm_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(as_votebkm_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- optional hudmenu bindings ------------------------------------------------
# The player's own "!menu" choice (and the per-player menu type) is read through hudmenu's
# GetClientMenuType. This is a legitimate optional plugin dependency, not a wrapper library: the
# .so exports __hudmenu_* slots, the manifest declares hudmenu optional, and without it the menu
# type falls back to the same "hud if registered, else button" mapping pisexcompat used.
if(TARGET hudmenu_bindings)
    set(AS_VOTEBKM_HUDMENU_BINDINGS $<TARGET_OBJECTS:hudmenu_bindings>)
else()
    add_library(as_votebkm_hudmenu_bindings_obj OBJECT
            "${CMAKE_SOURCE_DIR}/sdk/include/hudmenu/hudmenu.cpp")
    target_include_directories(as_votebkm_hudmenu_bindings_obj SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(as_votebkm_hudmenu_bindings_obj PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
    set(AS_VOTEBKM_HUDMENU_BINDINGS $<TARGET_OBJECTS:as_votebkm_hudmenu_bindings_obj>)
endif()

# --- generated admin_system bindings -----------------------------------------
# Every consumer plugin needs the __admin_system_* globals compiled into its own library. The
# admin_system directory provides admin_system_bindings_obj and sorts before as_votebkm, but this
# directory must also configure on its own; the fallback target below therefore carries a unique
# name (a generic one would collide with the object library admin_system creates later).
if(TARGET admin_system_bindings_obj)
    set(AS_VOTEBKM_ADMIN_SYSTEM_BINDINGS_TARGET admin_system_bindings_obj)
else()
    add_library(as_votebkm_admin_system_bindings_obj OBJECT
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include/admin_system/admin_system.cpp")
    target_include_directories(as_votebkm_admin_system_bindings_obj SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(as_votebkm_admin_system_bindings_obj PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
    set(AS_VOTEBKM_ADMIN_SYSTEM_BINDINGS_TARGET as_votebkm_admin_system_bindings_obj)
endif()

# --- plugin -------------------------------------------------------------------
add_library(plugify-as_votebkm SHARED
        src/plugin.cpp
        # Binding globals (__s2sdk_* / __admin_system_*) filled by the plugify module from the
        # plugins declared as dependencies.
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:${AS_VOTEBKM_ADMIN_SYSTEM_BINDINGS_TARGET}>
        ${AS_VOTEBKM_HUDMENU_BINDINGS}
)
target_include_directories(plugify-as_votebkm PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: the generated admin_system/s2sdk/plg headers and admin_api.hpp are not ours to fix;
# their warnings must not fail -Werror on our sources.
target_include_directories(plugify-as_votebkm SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/admin_system/include"
        "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-as_votebkm PRIVATE
        as_votebkm_core
        plugify_sdk
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-as_votebkm PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-as_votebkm PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="as_votebkm"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-as_votebkm PROPERTIES OUTPUT_NAME as_votebkm)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-as_votebkm PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/as_votebkm.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/as_votebkm.pplugin"
)

# --- tests ---------------------------------------------------------------------
# as_votebkm_core_tests: engine-free core (KV1 grammar, votebkm.ini defaults/overrides, command
# spellings, config/phrase path order, ceil vote arithmetic, colour tags, ServerLang chain).
# as_votebkm_tests: the plugin TU is compiled into the test and every __s2sdk_* / __admin_system_*
# binding slot is a recording fake: the delayed result with a reused target slot, target
# disconnect, lost rights/core, map change and unload, bots, commands/menu ownership, menu id 0 and
# signed slots. Nothing here links pisexcompat.
#
# The top-level CMakeLists declares the PLUGIFY_PORT_TESTS option *after* the plugin directories,
# so on a first configure it is not defined here yet: default it to the same ON. An explicit
# -DPLUGIFY_PORT_TESTS=OFF (the exported standalone build) is already in the cache and still wins.
if(NOT DEFINED PLUGIFY_PORT_TESTS)
    set(PLUGIFY_PORT_TESTS ON)
endif()
if(PLUGIFY_PORT_TESTS)
    add_executable(as_votebkm_core_tests tests/as_votebkm_core_tests.cpp)
    target_link_libraries(as_votebkm_core_tests PRIVATE as_votebkm_core -static-libstdc++ -static-libgcc)
    target_compile_options(as_votebkm_core_tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(as_votebkm_core_tests PRIVATE
            _GLIBCXX_USE_CXX11_ABI=1
            AS_VOTEBKM_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    set_target_properties(as_votebkm_core_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

    add_executable(as_votebkm_tests
            tests/as_votebkm_tests.cpp
            $<TARGET_OBJECTS:s2sdk_bindings>
            $<TARGET_OBJECTS:${AS_VOTEBKM_ADMIN_SYSTEM_BINDINGS_TARGET}>
            ${AS_VOTEBKM_HUDMENU_BINDINGS}
    )
    target_include_directories(as_votebkm_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_include_directories(as_votebkm_tests SYSTEM PRIVATE
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include"
            "${CMAKE_SOURCE_DIR}/plugins/admin_system/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_link_libraries(as_votebkm_tests PRIVATE
            as_votebkm_core
            plugify_sdk
            -static-libstdc++ -static-libgcc)
    target_compile_options(as_votebkm_tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(as_votebkm_tests PRIVATE
            _GLIBCXX_USE_CXX11_ABI=1
            PLUGIFY_MODULE="as_votebkm"
            PLUGIFY_IS_DEBUG=0
            PLUGIFY_IS_RELEASE=1
            AS_VOTEBKM_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    set_target_properties(as_votebkm_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()
