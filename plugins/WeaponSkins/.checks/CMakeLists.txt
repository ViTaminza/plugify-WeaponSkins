cmake_minimum_required(VERSION 3.20)
project(weaponskins_core_checks LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
get_filename_component(WS_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
get_filename_component(WS_PLUGIN "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
add_library(ws_core STATIC "${WS_PLUGIN}/src/config.cpp" "${WS_PLUGIN}/src/storage.cpp" "${WS_PLUGIN}/src/econ.cpp")
target_include_directories(ws_core PUBLIC "${WS_PLUGIN}/src" "${WS_REPO}/libs/plgdb/include" "${WS_REPO}/libs/pisexcompat/include")
target_include_directories(ws_core SYSTEM PUBLIC "${WS_REPO}/sdk/include")
target_compile_options(ws_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(ws_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
target_link_libraries(ws_core PUBLIC
    "${WS_REPO}/build/libs/plgdb/libplgdb.a" "${WS_REPO}/build/libs/pisexcompat/libpisexcompat.a"
    "/usr/lib/x86_64-linux-gnu/libmariadbclient.a" "/usr/lib/x86_64-linux-gnu/libsqlite3.a"
    "/usr/lib/x86_64-linux-gnu/libssl.a" "/usr/lib/x86_64-linux-gnu/libcrypto.a" "/usr/lib/x86_64-linux-gnu/libz.a"
    pthread dl m)
foreach(WS_PART config storage econ real_econ)
    add_executable(weaponskins_${WS_PART}_tests "${WS_PLUGIN}/tests/${WS_PART}_tests.cpp" "${WS_PLUGIN}/tests/framework_stubs.cpp"
        "${WS_REPO}/build/CMakeFiles/s2sdk_bindings.dir/sdk/include/s2sdk/s2sdk.cpp.o"
        "${WS_REPO}/build/CMakeFiles/hudmenu_bindings.dir/sdk/include/hudmenu/hudmenu.cpp.o")
    target_link_libraries(weaponskins_${WS_PART}_tests PRIVATE ws_core -static-libstdc++ -static-libgcc)
    target_compile_options(weaponskins_${WS_PART}_tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(weaponskins_${WS_PART}_tests PRIVATE WS_FIXTURES_DIR="${WS_PLUGIN}/tests/fixtures"
        WS_SOURCE_DIR="${WS_PLUGIN}" WEAPONSKINS_TEST_DIR="${WS_PLUGIN}/tests/fixtures"
        WS_FULL_ITEMS="${WS_REPO}/handoff/weaponskins/analysis/implementation-20261008/items_game.full.txt")
endforeach()

add_library(ws_engine STATIC "${WS_PLUGIN}/src/engine.cpp")
target_link_libraries(ws_engine PUBLIC ws_core)
target_compile_options(ws_engine PRIVATE -Wall -Wextra -Werror)
foreach(WS_PART engine runtime)
    add_executable(weaponskins_${WS_PART}_tests "${WS_PLUGIN}/tests/${WS_PART}_tests.cpp" "${WS_PLUGIN}/tests/framework_stubs.cpp"
        "${WS_REPO}/build/CMakeFiles/s2sdk_bindings.dir/sdk/include/s2sdk/s2sdk.cpp.o"
        "${WS_REPO}/build/CMakeFiles/hudmenu_bindings.dir/sdk/include/hudmenu/hudmenu.cpp.o"
        "${WS_REPO}/build/CMakeFiles/polyhook_bindings.dir/sdk/include/polyhook/polyhook.cpp.o"
        "${WS_REPO}/build/CMakeFiles/permissions_bindings.dir/sdk/include/permissions/permissions.cpp.o")
    target_link_libraries(weaponskins_${WS_PART}_tests PRIVATE ws_engine -static-libstdc++ -static-libgcc)
    target_compile_options(weaponskins_${WS_PART}_tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(weaponskins_${WS_PART}_tests PRIVATE WS_FIXTURES_DIR="${WS_PLUGIN}/tests/fixtures"
        WS_SOURCE_DIR="${WS_PLUGIN}" WEAPONSKINS_TEST_DIR="${WS_PLUGIN}/tests/fixtures")
endforeach()
