set(WEAPONSKINS_VERSION "1.0.1")
configure_file(WeaponSkins.pplugin.in WeaponSkins.pplugin @ONLY)

add_library(weaponskins_core STATIC src/config.cpp src/storage.cpp src/econ.cpp)
target_include_directories(weaponskins_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(weaponskins_core SYSTEM PUBLIC "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(weaponskins_core PUBLIC plgdb pisexcompat)
target_compile_options(weaponskins_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(weaponskins_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

add_library(weaponskins_engine STATIC src/engine.cpp)
target_include_directories(weaponskins_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(weaponskins_engine SYSTEM PUBLIC "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(weaponskins_engine PUBLIC weaponskins_core polyhook_bindings permissions_bindings)
target_compile_options(weaponskins_engine PRIVATE -Wall -Wextra -Werror)

add_library(weaponskins_runtime STATIC src/runtime.cpp)
target_include_directories(weaponskins_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(weaponskins_runtime PUBLIC weaponskins_engine)
target_compile_options(weaponskins_runtime PRIVATE -Wall -Wextra -Werror)

add_library(plugify-WeaponSkins SHARED src/plugin.cpp)
target_link_libraries(plugify-WeaponSkins PRIVATE weaponskins_runtime -static-libstdc++ -static-libgcc)
target_compile_options(plugify-WeaponSkins PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-WeaponSkins PRIVATE
    _GLIBCXX_USE_CXX11_ABI=1 PLUGIFY_MODULE="WeaponSkins" PLUGIFY_IS_DEBUG=0 PLUGIFY_IS_RELEASE=1)
set_target_properties(plugify-WeaponSkins PROPERTIES OUTPUT_NAME WeaponSkins)
target_link_options(plugify-WeaponSkins PRIVATE "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")

foreach(_ws_part config storage econ engine runtime)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/${_ws_part}_tests.cpp")
        add_executable(weaponskins_${_ws_part}_tests tests/${_ws_part}_tests.cpp tests/framework_stubs.cpp)
        target_link_libraries(weaponskins_${_ws_part}_tests PRIVATE
            weaponskins_runtime -static-libstdc++ -static-libgcc)
        target_compile_options(weaponskins_${_ws_part}_tests PRIVATE -Wall -Wextra -Werror)
        target_compile_definitions(weaponskins_${_ws_part}_tests PRIVATE
            WS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
            WS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
            WEAPONSKINS_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
        set_target_properties(weaponskins_${_ws_part}_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    endif()
endforeach()
add_custom_target(weaponskins_tests DEPENDS weaponskins_config_tests weaponskins_storage_tests
    weaponskins_econ_tests weaponskins_engine_tests weaponskins_runtime_tests weaponskins_real_econ_tests)

add_executable(weaponskins_real_econ_tests tests/real_econ_tests.cpp tests/framework_stubs.cpp)
target_link_libraries(weaponskins_real_econ_tests PRIVATE weaponskins_core -static-libstdc++ -static-libgcc)
target_compile_options(weaponskins_real_econ_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(weaponskins_real_econ_tests PRIVATE
    WS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
set(WS_FULL_ITEMS "" CACHE FILEPATH "Optional complete items_game.txt for the real-econ test")
if(WS_FULL_ITEMS)
    target_compile_definitions(weaponskins_real_econ_tests PRIVATE WS_FULL_ITEMS="${WS_FULL_ITEMS}")
endif()
set_target_properties(weaponskins_real_econ_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
