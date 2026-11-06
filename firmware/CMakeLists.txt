cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(iap2tab5)

# LVGL 9.6's PPA draw unit calls esp_cache_get_line_size_by_addr(), which only
# exists in IDF v5.5+. Declare it for the managed lvgl component (the
# implementation lives in main/lvgl_ppa_idf_compat.c).
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${lvgl_lib} PRIVATE
    -include "${CMAKE_CURRENT_SOURCE_DIR}/main/lvgl_ppa_idf_compat.h")
