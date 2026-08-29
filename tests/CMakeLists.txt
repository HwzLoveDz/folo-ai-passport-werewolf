cmake_minimum_required(VERSION 3.16)
project(werewolf_host_tests C)

enable_testing()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

function(ww_add_host_test target test_source implementation_source)
    add_executable(${target} ${test_source} ${implementation_source})
    target_include_directories(${target} PRIVATE ../main)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Werror -Wpedantic -Wconversion -Wshadow)
    endif()
    add_test(NAME ${target} COMMAND ${target})
endfunction()

ww_add_host_test(test_werewolf_game
    test_werewolf_game.c
    ../main/werewolf_game.c)

ww_add_host_test(test_werewolf_protocol
    test_werewolf_protocol.c
    ../main/werewolf_protocol.c)
target_sources(test_werewolf_protocol PRIVATE ../main/werewolf_net.c)

ww_add_host_test(test_werewolf_lobby
    test_werewolf_lobby.c
    ../main/werewolf_lobby.c)

ww_add_host_test(test_werewolf_ui_text
    test_werewolf_ui_text.c
    ../main/werewolf_ui_text.c)

ww_add_host_test(test_werewolf_nickname
    test_werewolf_nickname.c
    ../main/werewolf_nickname.c)

ww_add_host_test(test_werewolf_identity_record
    test_werewolf_identity_record.c
    ../main/werewolf_identity_record.c)
target_sources(test_werewolf_identity_record PRIVATE
    ../main/werewolf_nickname.c)

ww_add_host_test(test_werewolf_messages
    test_werewolf_messages.c
    ../main/werewolf_messages.c)
target_sources(test_werewolf_messages PRIVATE ../main/werewolf_nickname.c)

ww_add_host_test(test_werewolf_pairing
    test_werewolf_pairing.c
    ../main/werewolf_pairing.c)

ww_add_host_test(test_werewolf_room_directory
    test_werewolf_room_directory.c
    ../main/werewolf_room_directory.c)

ww_add_host_test(test_werewolf_termination
    test_werewolf_termination.c
    ../main/werewolf_termination.c)
