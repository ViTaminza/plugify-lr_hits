# lr_hits — Plugify C++ port of the Pisex LR module "[LR] ExStats Hits"
# (the original LR_ExStatsHits module, Metamod plugin lr_hits).
#
# Pure s2sdk (docs/PLUGIN_STANDARD.md §7): no pisexcompat and no guard library. The "player_hurt"
# hook, the map gate and the client/binding checks live in src/plugin.cpp; the per-slot counters,
# the SteamID normalization and the `_hits` SQL are the engine-free src/hits_core.*.
#
#   lr_hits_core       engine-free core (the per-slot hit counters, the `_hits` SQL; also used by
#                      tests/hits_tests.cpp)
#   plugify-lr_hits    the plugin shared library (entry "bin/lr_hits")

set(LR_HITS_VERSION "1.1.0")

# --- core ---------------------------------------------------------------------
add_library(lr_hits_core STATIC src/hits_core.cpp)
target_include_directories(lr_hits_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# plgdb is the database layer and the only dependency of the core: the counters, the SQL builders
# and the SteamID normalization are engine-free, so the tests link this target alone.
target_link_libraries(lr_hits_core PUBLIC plgdb)
target_compile_options(lr_hits_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(lr_hits_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- generated levels_ranks bindings -------------------------------------------
# The __levels_ranks_* globals have to be compiled into every consumer plugin; plugins/levels_ranks
# builds them as the object library `lr_bindings` (it sorts before this directory), this is the
# fallback for a build of this directory alone.
if(NOT TARGET lr_bindings AND EXISTS
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include/levels_ranks/levels_ranks.cpp")
    add_library(lr_bindings OBJECT
            "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include/levels_ranks/levels_ranks.cpp")
    target_include_directories(lr_bindings SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(lr_bindings PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
endif()

# --- plugin -------------------------------------------------------------------
add_library(plugify-lr_hits SHARED
        src/plugin.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:lr_bindings>
)
target_include_directories(plugify-lr_hits PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated lr/plg/s2sdk headers and lr_api.hpp must not fail -Werror on
# our sources.
target_include_directories(plugify-lr_hits SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include"
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-lr_hits PRIVATE
        plugify_sdk
        lr_hits_core
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-lr_hits PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-lr_hits PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="lr_hits"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-lr_hits PROPERTIES OUTPUT_NAME lr_hits)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-lr_hits PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/lr_hits.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/lr_hits.pplugin"
)

# --- tests ---------------------------------------------------------------------
# Engine-free tests (the counters, the SteamID normalization and the SQL against SQLite and the test
# MySQL of ./test.sh); build/tests so ../test.sh runs them. Nothing here links pisexcompat,
# plugify_sdk or an engine binding: the test links lr_hits_core (and transitively plgdb) alone.
add_executable(lr_hits_tests tests/hits_tests.cpp)
target_link_libraries(lr_hits_tests PRIVATE lr_hits_core -static-libstdc++ -static-libgcc)
target_compile_options(lr_hits_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(lr_hits_tests PRIVATE _GLIBCXX_USE_CXX11_ABI=1)
set_target_properties(lr_hits_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

# Engine test of the real "player_hurt" callback: src/plugin.cpp is compiled into the test together
# with the generated binding objects, whose __s2sdk_*/__levels_ranks_* globals are null until the
# test assigns counting fakes. It drives the plugin through its own Plugify_* entry points and
# proves that "attacker" is read with s2sdk::GetEventPlayerSlot (the fake GetEventInt answers a
# deliberately different value), that slot 0 and -1/world are handled, that a missing binding is a
# no-op and that OnPluginEnd removes every registration. Nothing here links pisexcompat.
add_executable(lr_hits_event_tests
        tests/event_tests.cpp
        src/plugin.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:lr_bindings>
)
target_include_directories(lr_hits_event_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated lr/plg/s2sdk headers and lr_api.hpp must not fail -Werror on
# our sources (src/plugin.cpp is one of them).
target_include_directories(lr_hits_event_tests SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include"
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(lr_hits_event_tests PRIVATE
        plugify_sdk
        lr_hits_core
        -static-libstdc++ -static-libgcc
)
target_compile_options(lr_hits_event_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(lr_hits_event_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="lr_hits"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(lr_hits_event_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
