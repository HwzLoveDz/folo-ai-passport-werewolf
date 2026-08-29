cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Product firmware only: compile main plus declared transitive dependencies.
# This keeps unrelated ESP-IDF/LVGL demos and optional services out of the
# build graph; every required capability must be explicit in component CMake.
idf_build_set_property(MINIMAL_BUILD ON)
project(FoloToy-AI-Passport-Werewolf)
