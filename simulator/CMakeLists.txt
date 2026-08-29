cmake_minimum_required(VERSION 3.16)
project(werewolf_ui_simulator LANGUAGES C CXX ASM)

get_filename_component(WEREWOLF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(LVGL_ROOT "${WEREWOLF_ROOT}/managed_components/lvgl__lvgl")
if(NOT EXISTS "${LVGL_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "LVGL managed component is missing. Configure the ESP-IDF project once first.")
endif()

set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h"
    CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory("${LVGL_ROOT}" "${CMAKE_BINARY_DIR}/lvgl")

add_executable(werewolf-ui-sim
    main.c
    png_writer.c
    "${WEREWOLF_ROOT}/main/werewolf_ui.c"
    "${WEREWOLF_ROOT}/main/werewolf_ui_text.c"
    "${WEREWOLF_ROOT}/main/fonts/kode_mono/ui_font_kode_regular_11.c"
    "${WEREWOLF_ROOT}/main/fonts/kode_mono/ui_font_kode_regular_13.c"
    "${WEREWOLF_ROOT}/main/fonts/kode_mono/ui_font_kode_bold_13.c"
    "${WEREWOLF_ROOT}/main/fonts/kode_mono/ui_font_kode_bold_15.c"
    "${WEREWOLF_ROOT}/main/fonts/kode_mono/ui_font_kode_bold_21.c"
)
target_include_directories(werewolf-ui-sim PRIVATE
    "${WEREWOLF_ROOT}/main"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_options(werewolf-ui-sim PRIVATE
    -Wall -Wextra -Werror
)
target_link_libraries(werewolf-ui-sim PRIVATE lvgl m)
