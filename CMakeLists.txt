cmake_minimum_required(VERSION 3.20)
project(iap2tab5_sim VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# -----------------------------------------------------------------------------
# LVGL
#
# Uses a local checkout when -DLVGL_SOURCE_DIR=/path/to/lvgl is given,
# otherwise downloads the pinned release once into build/_deps.
# -----------------------------------------------------------------------------
include(FetchContent)

set(LVGL_SOURCE_DIR "" CACHE PATH "Optional path to a local LVGL checkout")
if(LVGL_SOURCE_DIR)
    set(FETCHCONTENT_SOURCE_DIR_LVGL "${LVGL_SOURCE_DIR}" CACHE PATH "" FORCE)
endif()

set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(LV_BUILD_SET_CONFIG_OPTS ON CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG OFF CACHE BOOL "" FORCE)
set(LV_BUILD_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    lvgl
    URL https://github.com/lvgl/lvgl/archive/refs/tags/v9.6.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(lvgl)

# -----------------------------------------------------------------------------
# SDL2 (desktop window, mouse/touch and keyboard input)
# -----------------------------------------------------------------------------
find_package(SDL2 REQUIRED)
target_include_directories(lvgl PUBLIC ${SDL2_INCLUDE_DIRS})
if(TARGET SDL2::SDL2)
    set(SDL2_LINK_TARGET SDL2::SDL2)
else()
    set(SDL2_LINK_TARGET ${SDL2_LIBRARIES})
endif()

# -----------------------------------------------------------------------------
# Application
# -----------------------------------------------------------------------------
set(UI_SOURCES
    app/backend/screen_idle.c
    app/backend/fast_scroll.c
    app/backend/cover_gradient.c
    app/backend/cover_gradient_math.c
    app/backend/mock_player.c
    app/backend/media_index.c
    app/backend/ble_remote.c
    app/ui/boot.c
    app/ui/ui.c
    app/ui/theme.c
    app/ui/fonts/bootstrap_icons_24.c
    app/ui/fonts/bootstrap_icons_32.c
    app/ui/fonts/bootstrap_icons_40.c
    app/ui/fonts/bootstrap_icons_48.c
    app/ui/fonts/boot_pixel_32.c
    app/ui/fonts/inter_14.c
    app/ui/fonts/inter_16.c
    app/ui/fonts/inter_18.c
    app/ui/fonts/inter_20.c
    app/ui/fonts/inter_22.c
    app/ui/fonts/inter_24.c
    app/ui/fonts/inter_28.c
    app/ui/fonts/inter_32.c
    app/ui/fonts/inter_36.c
    app/ui/fonts/inter_40.c
    app/ui/fonts/inter_tabular_18.c
    app/ui/fonts/inter_tabular_20.c
    app/ui/fonts/inter_tabular_24.c
    app/ui/fonts/inter_tabular_28.c
    app/ui/fonts/inter_tabular_36.c
)

add_executable(tab5_sim
    sim/main.c
    sim/screenshot.c
    third_party/cjson/cJSON.c
    ${UI_SOURCES}
)

target_include_directories(tab5_sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/app/backend
    ${CMAKE_CURRENT_SOURCE_DIR}/app/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/sim
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cjson
)

target_compile_definitions(tab5_sim PRIVATE
    MOCK_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/mock"
)

target_link_libraries(tab5_sim PRIVATE lvgl::lvgl ${SDL2_LINK_TARGET} m)

if(NOT MSVC)
    target_compile_options(tab5_sim PRIVATE -Wall -Wextra)
endif()

option(IAP2TAB5_BUILD_UI_TESTS "Build headless LVGL pointer integration tests" OFF)
if(IAP2TAB5_BUILD_UI_TESTS)
    enable_testing()
    add_executable(album_touch_test tests/album_touch_test.c
        third_party/cjson/cJSON.c ${UI_SOURCES})
    target_include_directories(album_touch_test PRIVATE
        $<TARGET_PROPERTY:tab5_sim,INCLUDE_DIRECTORIES>)
    target_compile_definitions(album_touch_test PRIVATE
        $<TARGET_PROPERTY:tab5_sim,COMPILE_DEFINITIONS>)
    target_link_libraries(album_touch_test PRIVATE lvgl::lvgl ${SDL2_LINK_TARGET} m)
    foreach(theme RANGE 0 2)
        add_test(NAME album_touch_theme_${theme} COMMAND album_touch_test ${theme})
    endforeach()
    add_executable(screen_off_test tests/screen_off_test.c
        third_party/cjson/cJSON.c ${UI_SOURCES})
    target_include_directories(screen_off_test PRIVATE
        $<TARGET_PROPERTY:tab5_sim,INCLUDE_DIRECTORIES>)
    target_compile_definitions(screen_off_test PRIVATE
        $<TARGET_PROPERTY:tab5_sim,COMPILE_DEFINITIONS>)
    target_link_libraries(screen_off_test PRIVATE lvgl::lvgl ${SDL2_LINK_TARGET} m)
    add_test(NAME screen_off_input COMMAND screen_off_test)
    foreach(theme RANGE 1 2)
        add_test(NAME screen_settings_theme_${theme} COMMAND screen_off_test ${theme})
    endforeach()
    set(NP_TEST_SOURCES ${UI_SOURCES})
    list(REMOVE_ITEM NP_TEST_SOURCES app/ui/ui.c)
    add_executable(now_playing_update_test tests/now_playing_update_test.c
        third_party/cjson/cJSON.c ${NP_TEST_SOURCES})
    target_include_directories(now_playing_update_test PRIVATE
        $<TARGET_PROPERTY:tab5_sim,INCLUDE_DIRECTORIES>)
    target_compile_definitions(now_playing_update_test PRIVATE
        $<TARGET_PROPERTY:tab5_sim,COMPILE_DEFINITIONS>)
    target_link_libraries(now_playing_update_test PRIVATE lvgl::lvgl ${SDL2_LINK_TARGET} m)
    foreach(theme RANGE 0 2)
        add_test(NAME now_playing_updates_theme_${theme} COMMAND now_playing_update_test ${theme})
    endforeach()
    add_executable(navigation_test tests/navigation_test.c
        third_party/cjson/cJSON.c ${NP_TEST_SOURCES})
    target_include_directories(navigation_test PRIVATE
        $<TARGET_PROPERTY:tab5_sim,INCLUDE_DIRECTORIES>)
    target_compile_definitions(navigation_test PRIVATE
        $<TARGET_PROPERTY:tab5_sim,COMPILE_DEFINITIONS>)
    target_link_libraries(navigation_test PRIVATE lvgl::lvgl ${SDL2_LINK_TARGET} m)
    foreach(theme RANGE 0 2)
        add_test(NAME navigation_theme_${theme} COMMAND navigation_test ${theme})
    endforeach()
    add_executable(notification_test tests/notification_test.c
        third_party/cjson/cJSON.c ${NP_TEST_SOURCES})
    target_include_directories(notification_test PRIVATE
        $<TARGET_PROPERTY:tab5_sim,INCLUDE_DIRECTORIES>)
    target_compile_definitions(notification_test PRIVATE
        $<TARGET_PROPERTY:tab5_sim,COMPILE_DEFINITIONS>)
    target_link_libraries(notification_test PRIVATE lvgl::lvgl ${SDL2_LINK_TARGET} m)
    foreach(theme RANGE 0 2)
        add_test(NAME notifications_theme_${theme} COMMAND notification_test ${theme})
    endforeach()
    add_executable(afc_ui_test tests/afc_ui_test.c
        third_party/cjson/cJSON.c ${NP_TEST_SOURCES})
    target_include_directories(afc_ui_test PRIVATE
        $<TARGET_PROPERTY:tab5_sim,INCLUDE_DIRECTORIES>)
    target_compile_definitions(afc_ui_test PRIVATE
        $<TARGET_PROPERTY:tab5_sim,COMPILE_DEFINITIONS>)
    target_link_libraries(afc_ui_test PRIVATE lvgl::lvgl ${SDL2_LINK_TARGET} m)
    foreach(theme RANGE 0 2)
        add_test(NAME afc_controls_theme_${theme} COMMAND afc_ui_test ${theme})
    endforeach()

endif()

# Convenience: `cmake --build build --target run`
add_custom_target(run
    COMMAND $<TARGET_FILE:tab5_sim>
    DEPENDS tab5_sim
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
