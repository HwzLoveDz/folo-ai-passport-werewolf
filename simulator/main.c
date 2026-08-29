#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"

#include "png_writer.h"
#include "werewolf_ui.h"

#define DISPLAY_WIDTH  240U
#define DISPLAY_HEIGHT 320U
#define DRAW_ROWS       48U
#define PREVIEW_VERIFY_CODE UINT32_C(414978)

static uint16_t s_framebuffer[DISPLAY_WIDTH * DISPLAY_HEIGHT];
static uint8_t s_draw_buffer[DISPLAY_WIDTH * DRAW_ROWS * 2U];

static void display_flush(lv_display_t *display, const lv_area_t *area,
                          uint8_t *pixels)
{
    const unsigned source_width = (unsigned)(area->x2 - area->x1 + 1);
    const uint16_t *source = (const uint16_t *)pixels;

    for (int32_t y = area->y1; y <= area->y2; ++y) {
        if (y < 0 || y >= (int32_t)DISPLAY_HEIGHT) {
            continue;
        }
        for (int32_t x = area->x1; x <= area->x2; ++x) {
            if (x < 0 || x >= (int32_t)DISPLAY_WIDTH) {
                continue;
            }
            size_t source_index = (size_t)(y - area->y1) * source_width +
                                  (size_t)(x - area->x1);
            s_framebuffer[(size_t)y * DISPLAY_WIDTH + (size_t)x] =
                source[source_index];
        }
    }
    lv_display_flush_ready(display);
}

static void copy_text(char *destination, size_t size, const char *source)
{
    if (size == 0U) {
        return;
    }
    (void)snprintf(destination, size, "%s", source);
}

static void populate_players(werewolf_ui_model_t *model)
{
    static const char *const names[WEREWOLF_UI_PLAYER_COUNT] = {
        "HOST", "NOVA", "YOU", "ORBIT", "PIXEL", "SIGMA", "MOTE",
    };
    static const werewolf_ui_role_t roles[WEREWOLF_UI_PLAYER_COUNT] = {
        WEREWOLF_UI_ROLE_WOLF,
        WEREWOLF_UI_ROLE_WOLF,
        WEREWOLF_UI_ROLE_SEER,
        WEREWOLF_UI_ROLE_GUARD,
        WEREWOLF_UI_ROLE_VILLAGER,
        WEREWOLF_UI_ROLE_VILLAGER,
        WEREWOLF_UI_ROLE_VILLAGER,
    };

    for (unsigned i = 0; i < WEREWOLF_UI_PLAYER_COUNT; ++i) {
        werewolf_ui_player_t *player = &model->players[i];
        player->seat = (uint8_t)i;
        player->occupied = true;
        player->ready = true;
        player->alive = true;
        player->publicly_alive = true;
        player->eligible = i != model->local_seat;
        player->role = roles[i];
        copy_text(player->name, sizeof(player->name), names[i]);
    }
}

static void clear_players(werewolf_ui_model_t *model)
{
    for (unsigned i = 0U; i < WEREWOLF_UI_PLAYER_COUNT; ++i) {
        model->players[i].occupied = false;
        model->players[i].ready = false;
        model->players[i].name[0] = '\0';
    }
}

static void base_model(werewolf_ui_model_t *model)
{
    werewolf_ui_model_init(model);
    model->revision = 42U;
    model->private_epoch = 7U;
    model->connection = WEREWOLF_UI_CONNECTION_ONLINE;
    /* Deterministic production-model telemetry fixture. Firmware replaces this
     * with authenticated ESP-NOW receive RSSI and defaults to NO_SAMPLE. */
    model->signal = WEREWOLF_UI_SIGNAL_GOOD;
    model->battery_state = WEREWOLF_UI_BATTERY_FRESH;
    model->battery_soc = 76U;
    model->input_enabled = true;
    copy_text(model->local_name, sizeof(model->local_name), "Harvey");
    model->local_seat = 2U;
    model->selected_seat = 3U;
    model->round = 2U;
    model->guide_seconds = 38U;
    model->votes_received = 3U;
    model->votes_expected = 7U;
    copy_text(model->room_code, sizeof(model->room_code), "R7C21A9");
    populate_players(model);
}

static void configure_named_lobby(werewolf_ui_model_t *model, bool host,
                                  bool host_ready, bool guest_ready)
{
    clear_players(model);
    model->page = WEREWOLF_UI_PAGE_LOBBY;
    model->public_phase = WEREWOLF_UI_PUBLIC_PHASE_LOBBY;
    model->mode = host ? WEREWOLF_UI_MODE_CREATE : WEREWOLF_UI_MODE_JOIN;
    model->is_host = host;
    model->local_seat = host ? 0U : 1U;
    model->players[0].occupied = true;
    model->players[0].ready = host_ready;
    model->players[0].alive = true;
    model->players[0].publicly_alive = true;
    copy_text(model->players[0].name, sizeof(model->players[0].name),
              "Harvey");
    model->players[1].occupied = true;
    model->players[1].ready = guest_ready;
    model->players[1].alive = true;
    model->players[1].publicly_alive = true;
    copy_text(model->players[1].name, sizeof(model->players[1].name),
              "GG Bond");
    model->local_ready = model->players[model->local_seat].ready;
    model->selected_seat = model->local_seat;
    model->has_verify_code = true;
    model->verify_code = PREVIEW_VERIFY_CODE;
}

static void configure_player_detail(werewolf_ui_model_t *model)
{
    configure_named_lobby(model, true, true, true);
    model->page = WEREWOLF_UI_PAGE_PLAYER_ACTION;
    model->selected_seat = 1U;
}

static bool configure_state(werewolf_ui_model_t *model, const char *state)
{
    base_model(model);

    if (strcmp(state, "mode-create") == 0) {
        model->page = WEREWOLF_UI_PAGE_MODE;
        model->mode = WEREWOLF_UI_MODE_CREATE;
        model->connection = WEREWOLF_UI_CONNECTION_RADIO_OFF;
        model->public_phase = WEREWOLF_UI_PUBLIC_PHASE_MODE;
        model->local_seat = WEREWOLF_UI_NO_SEAT;
        clear_players(model);
    } else if (strcmp(state, "mode-join") == 0) {
        model->page = WEREWOLF_UI_PAGE_MODE;
        model->mode = WEREWOLF_UI_MODE_JOIN;
        model->connection = WEREWOLF_UI_CONNECTION_RADIO_OFF;
        model->public_phase = WEREWOLF_UI_PUBLIC_PHASE_MODE;
        model->local_seat = WEREWOLF_UI_NO_SEAT;
        clear_players(model);
    } else if (strcmp(state, "connection-scanning") == 0 ||
               strcmp(state, "connection-pairing") == 0) {
        model->page = WEREWOLF_UI_PAGE_LOBBY;
        model->mode = WEREWOLF_UI_MODE_JOIN;
        model->public_phase = WEREWOLF_UI_PUBLIC_PHASE_LOBBY;
        model->connection = strcmp(state, "connection-scanning") == 0
                                ? WEREWOLF_UI_CONNECTION_SCANNING
                                : WEREWOLF_UI_CONNECTION_PAIRING;
        model->signal = WEREWOLF_UI_SIGNAL_NO_SAMPLE;
        model->local_seat = WEREWOLF_UI_NO_SEAT;
        model->input_enabled = false;
        copy_text(model->room_code, sizeof(model->room_code), "SCAN");
        clear_players(model);
        if (model->connection == WEREWOLF_UI_CONNECTION_PAIRING) {
            model->players[0].occupied = true;
            /* Pairing beacons expose occupancy, not the lobby ready mask. */
            model->players[0].ready = false;
            copy_text(model->players[0].name,
                      sizeof(model->players[0].name), "HOST");
            model->players[1].occupied = true;
            model->players[1].ready = false;
            copy_text(model->players[1].name,
                      sizeof(model->players[1].name), "PLAYER2");
        }
    } else if (strcmp(state, "room-list-empty") == 0 ||
               strcmp(state, "room-list") == 0 ||
               strcmp(state, "room-list-multiple") == 0) {
        model->page = WEREWOLF_UI_PAGE_ROOM_LIST;
        model->mode = WEREWOLF_UI_MODE_JOIN;
        model->public_phase = WEREWOLF_UI_PUBLIC_PHASE_LOBBY;
        model->connection = WEREWOLF_UI_CONNECTION_SCANNING;
        model->signal = WEREWOLF_UI_SIGNAL_NO_SAMPLE;
        model->local_seat = WEREWOLF_UI_NO_SEAT;
        clear_players(model);
        if (strcmp(state, "room-list-empty") != 0) {
            static const char *const room_codes[] = {
                "R12AB34", "R44F010", "R91CC20", "RA0B1C2", "RF00D77",
            };
            for (unsigned i = 0U;
                 i < sizeof(room_codes) / sizeof(room_codes[0]); ++i) {
                model->rooms[i].visible = true;
                model->rooms[i].token = (uint32_t)i + 1U;
                model->rooms[i].occupied_count = (uint8_t)(i + 1U);
                copy_text(model->rooms[i].code,
                          sizeof(model->rooms[i].code), room_codes[i]);
            }
            model->selected_room_token = 1U;
        }
    } else if (strcmp(state, "lobby-host-self-wait") == 0 ||
               strcmp(state, "lobby-host") == 0) {
        configure_named_lobby(model, true, false, false);
    } else if (strcmp(state, "lobby-host-self-ready") == 0) {
        configure_named_lobby(model, true, true, false);
    } else if (strcmp(state, "lobby-host-guest-focus") == 0) {
        configure_named_lobby(model, true, false, false);
    } else if (strcmp(state, "lobby-host-all-ready") == 0 ||
               strcmp(state, "lobby-ready") == 0) {
        model->page = WEREWOLF_UI_PAGE_LOBBY;
        model->public_phase = WEREWOLF_UI_PUBLIC_PHASE_LOBBY;
        model->is_host = true;
        model->local_seat = 0U;
        model->selected_seat = 0U;
        model->can_start = true;
        model->local_ready = true;
    } else if (strcmp(state, "lobby-guest-wait") == 0 ||
               strcmp(state, "lobby-guest-ready") == 0) {
        configure_named_lobby(
            model, false, true,
            strcmp(state, "lobby-guest-ready") == 0);
    } else if (strcmp(state, "player-detail") == 0 ||
               strcmp(state, "player-detail-back") == 0 ||
               strcmp(state, "player-detail-kick") == 0) {
        configure_player_detail(model);
    } else if (strcmp(state, "player-kicking") == 0) {
        configure_player_detail(model);
        model->kick_seat = 1U;
        model->player_kicking = true;
    } else if (strcmp(state, "lobby-host-exit-back") == 0 ||
               strcmp(state, "lobby-host-exit-close") == 0 ||
               strcmp(state, "lobby-host-exit-returned") == 0 ||
               strcmp(state, "lobby-host-exit-return-reference") == 0 ||
               strcmp(state, "lobby-host-closing") == 0) {
        configure_named_lobby(model, true, true, true);
        model->signal = WEREWOLF_UI_SIGNAL_STRONG;
        model->battery_soc = 100U;
        model->room_close_prompt =
            strcmp(state, "lobby-host-closing") != 0 &&
            strcmp(state, "lobby-host-exit-return-reference") != 0;
        model->room_closing =
            strcmp(state, "lobby-host-closing") == 0;
        model->input_enabled = !model->room_closing;
    } else if (strcmp(state, "lobby-nickname-limit") == 0 ||
               strcmp(state, "lobby-nickname-exact") == 0) {
        configure_named_lobby(model, true, true, true);
        copy_text(model->players[1].name,
                  sizeof(model->players[1].name),
                  strcmp(state, "lobby-nickname-limit") == 0
                      ? "ABCDEFGHIJKL" : "ABCDEFGHIJ");
    } else if (strcmp(state, "room-closed-guest") == 0 ||
               strcmp(state, "room-closed-from-wolf") == 0 ||
               strcmp(state, "room-closed-from-seer") == 0 ||
               strcmp(state, "kicked-guest") == 0 ||
               strcmp(state, "kicked-from-wolf") == 0 ||
               strcmp(state, "kicked-from-seer") == 0) {
        model->page = WEREWOLF_UI_PAGE_ROOM_CLOSED;
        model->public_phase = WEREWOLF_UI_PUBLIC_PHASE_LOBBY;
        model->connection = WEREWOLF_UI_CONNECTION_DISCONNECTED;
        model->signal = WEREWOLF_UI_SIGNAL_DISCONNECTED;
        model->is_host = false;
        model->local_seat = WEREWOLF_UI_NO_SEAT;
        model->input_enabled = true;
        if (strcmp(state, "room-closed-from-wolf") == 0 ||
            strcmp(state, "kicked-from-wolf") == 0) {
            model->local_role = WEREWOLF_UI_ROLE_WOLF;
        } else if (strcmp(state, "room-closed-from-seer") == 0 ||
                   strcmp(state, "kicked-from-seer") == 0) {
            model->local_role = WEREWOLF_UI_ROLE_SEER;
        }
        if (strcmp(state, "kicked-guest") == 0 ||
            strcmp(state, "kicked-from-wolf") == 0 ||
            strcmp(state, "kicked-from-seer") == 0) {
            copy_text(model->headline, sizeof(model->headline),
                      "REMOVED FROM ROOM");
            copy_text(model->detail, sizeof(model->detail),
                      "THE HOST REMOVED YOU\nFROM THIS ROOM.");
        }
        clear_players(model);
    } else if (strcmp(state, "role-sealed-wolf") == 0 ||
               strcmp(state, "role-revealed") == 0 ||
               strcmp(state, "role-release") == 0 ||
               strcmp(state, "role-long-unarmed") == 0 ||
               strcmp(state, "role-heartbeat") == 0 ||
               strcmp(state, "role-private-epoch-changed") == 0 ||
               strcmp(state, "role-heartbeat-release") == 0 ||
               strcmp(state, "role-private-epoch-release") == 0) {
        model->page = WEREWOLF_UI_PAGE_ROLE;
        model->public_phase = WEREWOLF_UI_PUBLIC_PHASE_ROLE_CHECK;
        model->game_started = true;
        model->local_role = WEREWOLF_UI_ROLE_WOLF;
        copy_text(model->private_detail, sizeof(model->private_detail),
                  "Your teammate is SEAT 2.");
    } else if (strcmp(state, "role-sealed-seer") == 0) {
        model->page = WEREWOLF_UI_PAGE_ROLE;
        model->public_phase = WEREWOLF_UI_PUBLIC_PHASE_ROLE_CHECK;
        model->game_started = true;
        model->local_role = WEREWOLF_UI_ROLE_SEER;
        copy_text(model->private_detail, sizeof(model->private_detail),
                  "Inspect one living seat each night.");
    } else if (strcmp(state, "night-select") == 0) {
        model->page = WEREWOLF_UI_PAGE_NIGHT_SELECT;
        model->public_phase = WEREWOLF_UI_PUBLIC_PHASE_NIGHT;
        model->game_started = true;
    } else if (strcmp(state, "night-confirm") == 0) {
        model->page = WEREWOLF_UI_PAGE_NIGHT_CONFIRM;
        model->public_phase = WEREWOLF_UI_PUBLIC_PHASE_NIGHT;
        model->game_started = true;
    } else if (strcmp(state, "night-waiting") == 0) {
        model->page = WEREWOLF_UI_PAGE_NIGHT_SELECT;
        model->public_phase = WEREWOLF_UI_PUBLIC_PHASE_NIGHT;
        model->game_started = true;
        model->input_enabled = false;
    } else if (strcmp(state, "night-known-dead") == 0) {
        model->page = WEREWOLF_UI_PAGE_NIGHT_SELECT;
        model->public_phase = WEREWOLF_UI_PUBLIC_PHASE_NIGHT;
        model->game_started = true;
        model->players[6].alive = false;
        model->players[6].publicly_alive = false;
    } else if (strcmp(state, "private-sealed-wolf") == 0 ||
               strcmp(state, "private-sealed-good") == 0 ||
               strcmp(state, "private-good") == 0 ||
               strcmp(state, "private-sealed-good-no-pending") == 0 ||
               strcmp(state, "private-good-no-pending") == 0 ||
               strcmp(state, "private-sealed-good-no-known-dead") == 0 ||
               strcmp(state, "private-sealed-good-local-pending") == 0 ||
               strcmp(state, "private-sealed-good-no-local-pending") == 0 ||
               strcmp(state, "private-no-result") == 0 ||
               strcmp(state, "private-waiting") == 0) {
        model->page = WEREWOLF_UI_PAGE_PRIVATE_RESULT;
        model->public_phase = WEREWOLF_UI_PUBLIC_PHASE_NIGHT;
        model->game_started = true;
        if (strcmp(state, "private-sealed-good-no-known-dead") != 0) {
            model->players[6].alive = false;
            model->players[6].publicly_alive = false;
        }
        /* Resolution may already have updated the authoritative alive mask,
         * but the header may only add the new death after the public DAWN
         * gate.  Previously disclosed deaths remain visible. */
        if (strcmp(state, "private-sealed-good-local-pending") == 0) {
            model->players[model->local_seat].alive = false;
        } else if (strcmp(state, "private-sealed-good-no-pending") != 0 &&
                   strcmp(state, "private-good-no-pending") != 0 &&
                   strcmp(state, "private-sealed-good-no-known-dead") != 0 &&
                   strcmp(state,
                          "private-sealed-good-no-local-pending") != 0) {
            model->players[5].alive = false;
        }
        if (strcmp(state, "private-no-result") == 0 ||
            strcmp(state, "private-waiting") == 0) {
            model->private_seat = WEREWOLF_UI_NO_SEAT;
            model->private_faction = WEREWOLF_UI_FACTION_UNKNOWN;
        } else {
            model->private_seat = 4U;
            model->private_faction = strcmp(state, "private-sealed-wolf") == 0
                                         ? WEREWOLF_UI_FACTION_WOLVES
                                         : WEREWOLF_UI_FACTION_GOOD;
        }
        if (strcmp(state, "private-waiting") == 0) {
            model->waiting_for_players = true;
            model->input_enabled = false;
        }
    } else if (strcmp(state, "day-result") == 0) {
        model->page = WEREWOLF_UI_PAGE_DAY_RESULT;
        model->public_phase = WEREWOLF_UI_PUBLIC_PHASE_DAWN;
        model->game_started = true;
        model->players[6].alive = false;
        model->players[6].publicly_alive = false;
        model->players[5].alive = false;
        model->players[5].publicly_alive = false;
        copy_text(model->headline, sizeof(model->headline), "DAWN REPORT");
        copy_text(model->detail, sizeof(model->detail),
                  "SEAT 6 WAS FOUND OUT. ROLE REMAINS HIDDEN.");
    } else if (strcmp(state, "speaking") == 0) {
        model->page = WEREWOLF_UI_PAGE_SPEAKING;
        model->public_phase = WEREWOLF_UI_PUBLIC_PHASE_DISCUSSION;
        model->game_started = true;
        model->speaker_seat = 2U;
        copy_text(model->headline, sizeof(model->headline), "YOUR TURN");
        copy_text(model->detail, sizeof(model->detail),
                  "Speak to the table. Audio is not recorded.");
    } else if (strcmp(state, "vote-select") == 0) {
        model->page = WEREWOLF_UI_PAGE_VOTE_SELECT;
        model->public_phase = WEREWOLF_UI_PUBLIC_PHASE_VOTE;
        model->game_started = true;
    } else if (strcmp(state, "vote-confirm") == 0) {
        model->page = WEREWOLF_UI_PAGE_VOTE_CONFIRM;
        model->public_phase = WEREWOLF_UI_PUBLIC_PHASE_VOTE;
        model->game_started = true;
    } else if (strcmp(state, "eliminated") == 0) {
        model->page = WEREWOLF_UI_PAGE_ELIMINATED;
        model->public_phase = WEREWOLF_UI_PUBLIC_PHASE_EXILE;
        model->game_started = true;
        model->affected_seat = 5U;
        model->players[5].alive = false;
        model->players[5].publicly_alive = false;
        copy_text(model->headline, sizeof(model->headline), "VOTE RESOLVED");
        copy_text(model->detail, sizeof(model->detail),
                  "SEAT 6 LEAVES THE TABLE. ROLE STAYS HIDDEN.");
    } else if (strcmp(state, "game-over") == 0) {
        model->page = WEREWOLF_UI_PAGE_GAME_OVER;
        model->public_phase = WEREWOLF_UI_PUBLIC_PHASE_GAME_OVER;
        model->game_started = true;
        model->winner = WEREWOLF_UI_WINNER_GOOD;
        model->players[0].alive = false;
        model->players[0].publicly_alive = false;
        model->players[1].alive = false;
        model->players[1].publicly_alive = false;
    } else if (strcmp(state, "status-self-dead") == 0) {
        model->page = WEREWOLF_UI_PAGE_ELIMINATED;
        model->public_phase = WEREWOLF_UI_PUBLIC_PHASE_EXILE;
        model->game_started = true;
        model->affected_seat = model->local_seat;
        model->players[model->local_seat].alive = false;
        model->players[model->local_seat].publicly_alive = false;
        copy_text(model->headline, sizeof(model->headline), "YOU ARE OUT");
        copy_text(model->detail, sizeof(model->detail),
                  "YOUR SEAT REMAINS IN THE PUBLIC TABLE STATE.");
    } else if (strcmp(state, "status-self-host-dead") == 0) {
        model->page = WEREWOLF_UI_PAGE_ELIMINATED;
        model->public_phase = WEREWOLF_UI_PUBLIC_PHASE_EXILE;
        model->game_started = true;
        model->is_host = true;
        model->local_seat = 0U;
        model->affected_seat = 0U;
        model->players[0].alive = false;
        model->players[0].publicly_alive = false;
        copy_text(model->headline, sizeof(model->headline), "YOU ARE OUT");
        copy_text(model->detail, sizeof(model->detail),
                  "HOST CONTROL CONTINUES. YOUR SEAT IS PUBLICLY OUT.");
    } else if (strcmp(state, "reconnecting") == 0) {
        model->page = WEREWOLF_UI_PAGE_LOBBY;
        model->public_phase = WEREWOLF_UI_PUBLIC_PHASE_LOBBY;
        model->connection = WEREWOLF_UI_CONNECTION_RECONNECTING;
        model->signal = WEREWOLF_UI_SIGNAL_STALE;
        model->input_enabled = false;
    } else if (strcmp(state, "connection-disconnected") == 0) {
        model->page = WEREWOLF_UI_PAGE_LOBBY;
        model->public_phase = WEREWOLF_UI_PUBLIC_PHASE_LOBBY;
        model->connection = WEREWOLF_UI_CONNECTION_DISCONNECTED;
        model->signal = WEREWOLF_UI_SIGNAL_DISCONNECTED;
        model->input_enabled = false;
    } else if (strcmp(state, "connection-host-lost") == 0) {
        /* Mirrors clear_session() followed by show_error(HOST_LOST). */
        model->page = WEREWOLF_UI_PAGE_ERROR;
        model->public_phase = WEREWOLF_UI_PUBLIC_PHASE_ERROR;
        model->connection = WEREWOLF_UI_CONNECTION_HOST_LOST;
        model->signal = WEREWOLF_UI_SIGNAL_DISCONNECTED;
        model->error = WEREWOLF_UI_ERROR_HOST_LOST;
        model->input_enabled = false;
        model->recoverable = false;
        model->game_started = false;
        clear_players(model);
        copy_text(model->detail, sizeof(model->detail),
                  "HOST HEARTBEAT LOST. NO HOST MIGRATION.");
    } else if (strcmp(state, "status-link-online") == 0 ||
               strcmp(state, "status-link-scanning") == 0 ||
               strcmp(state, "status-link-pairing") == 0) {
        model->page = WEREWOLF_UI_PAGE_LOBBY;
        model->public_phase = WEREWOLF_UI_PUBLIC_PHASE_LOBBY;
        model->is_host = true;
        model->local_seat = 0U;
        model->local_ready = true;
        model->connection = strcmp(state, "status-link-online") == 0
                                ? WEREWOLF_UI_CONNECTION_ONLINE
                                : (strcmp(state, "status-link-scanning") == 0
                                       ? WEREWOLF_UI_CONNECTION_SCANNING
                                       : WEREWOLF_UI_CONNECTION_PAIRING);
        if (model->connection != WEREWOLF_UI_CONNECTION_ONLINE) {
            model->signal = WEREWOLF_UI_SIGNAL_NO_SAMPLE;
        }
    } else if (strcmp(state, "status-signal-none") == 0 ||
               strcmp(state, "status-signal-weak") == 0 ||
               strcmp(state, "status-signal-fair") == 0 ||
               strcmp(state, "status-signal-good") == 0 ||
               strcmp(state, "status-signal-strong") == 0 ||
               strcmp(state, "status-signal-stale") == 0 ||
               strcmp(state, "status-signal-disconnected") == 0 ||
               strcmp(state,
                      "status-signal-disconnected-cached-strong") == 0) {
        model->page = WEREWOLF_UI_PAGE_LOBBY;
        model->public_phase = WEREWOLF_UI_PUBLIC_PHASE_LOBBY;
        model->is_host = true;
        model->local_seat = 0U;
        model->local_ready = true;
        if (strcmp(state, "status-signal-none") == 0) {
            model->signal = WEREWOLF_UI_SIGNAL_NO_SAMPLE;
        } else if (strcmp(state, "status-signal-weak") == 0) {
            model->signal = WEREWOLF_UI_SIGNAL_WEAK;
        } else if (strcmp(state, "status-signal-fair") == 0) {
            model->signal = WEREWOLF_UI_SIGNAL_FAIR;
        } else if (strcmp(state, "status-signal-good") == 0) {
            model->signal = WEREWOLF_UI_SIGNAL_GOOD;
        } else if (strcmp(state, "status-signal-strong") == 0) {
            model->signal = WEREWOLF_UI_SIGNAL_STRONG;
        } else if (strcmp(state, "status-signal-stale") == 0) {
            model->signal = WEREWOLF_UI_SIGNAL_STALE;
        } else {
            model->connection = WEREWOLF_UI_CONNECTION_DISCONNECTED;
            model->input_enabled = false;
            model->signal =
                strcmp(state, "status-signal-disconnected") == 0
                    ? WEREWOLF_UI_SIGNAL_DISCONNECTED
                    : WEREWOLF_UI_SIGNAL_STRONG;
        }
    } else if (strcmp(state, "error-recoverable") == 0 ||
               strcmp(state, "error-connection") == 0 ||
               strcmp(state, "error-protocol") == 0 ||
               strcmp(state, "error-hardware") == 0 ||
               strcmp(state, "error") == 0 ||
               strcmp(state, "error-host-clean") == 0 ||
               strcmp(state, "error-host-close-stale") == 0) {
        model->page = WEREWOLF_UI_PAGE_ERROR;
        model->public_phase = WEREWOLF_UI_PUBLIC_PHASE_ERROR;
        model->connection = WEREWOLF_UI_CONNECTION_DISCONNECTED;
        model->error = strcmp(state, "error-protocol") == 0
                           ? WEREWOLF_UI_ERROR_PROTOCOL
                           : (strcmp(state, "error-hardware") == 0
                                  ? WEREWOLF_UI_ERROR_HARDWARE
                                  : WEREWOLF_UI_ERROR_TIMEOUT);
        model->recoverable = true;
        if (strcmp(state, "error") != 0) {
            model->is_host = true;
        }
        if (strcmp(state, "error-host-close-stale") == 0) {
            model->room_close_prompt = true;
            model->room_closing = true;
        }
        copy_text(model->detail, sizeof(model->detail),
                  "HOST DID NOT ANSWER. GAME STATE IS UNCHANGED.");
    } else if (strcmp(state, "status-battery-unavailable") == 0 ||
               strcmp(state, "status-battery-low") == 0 ||
               strcmp(state, "status-battery-critical") == 0 ||
               strcmp(state, "status-battery-stale") == 0 ||
               strcmp(state, "status-battery-full") == 0 ||
               strcmp(state, "status-battery-zero") == 0) {
        model->page = WEREWOLF_UI_PAGE_LOBBY;
        model->public_phase = WEREWOLF_UI_PUBLIC_PHASE_LOBBY;
        model->is_host = true;
        model->local_seat = 0U;
        model->local_ready = true;
        if (strcmp(state, "status-battery-unavailable") == 0) {
            model->battery_state = WEREWOLF_UI_BATTERY_UNAVAILABLE;
            model->battery_soc = 0U;
        } else if (strcmp(state, "status-battery-low") == 0) {
            model->battery_soc = 15U;
        } else if (strcmp(state, "status-battery-critical") == 0) {
            model->battery_soc = 6U;
        } else if (strcmp(state, "status-battery-stale") == 0) {
            model->battery_state = WEREWOLF_UI_BATTERY_STALE;
        } else if (strcmp(state, "status-battery-full") == 0) {
            model->battery_soc = 100U;
        } else if (strcmp(state, "status-battery-zero") == 0) {
            model->battery_soc = 0U;
        }
    } else {
        return false;
    }
    return true;
}

static bool apply_state_events(werewolf_ui_model_t *model, const char *state)
{
    werewolf_ui_action_t action = {0};
    bool room_closed_state =
        strcmp(state, "room-closed-guest") == 0 ||
        strcmp(state, "room-closed-from-wolf") == 0 ||
        strcmp(state, "room-closed-from-seer") == 0 ||
        strcmp(state, "kicked-guest") == 0 ||
        strcmp(state, "kicked-from-wolf") == 0 ||
        strcmp(state, "kicked-from-seer") == 0;

    if (strcmp(state, "lobby-host-self-wait") == 0 ||
        strcmp(state, "lobby-host") == 0 ||
        strcmp(state, "lobby-host-self-ready") == 0) {
        bool expected_ready =
            strcmp(state, "lobby-host-self-ready") != 0;

        if (!werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                    WEREWOLF_UI_KEY_EVENT_CLICK, &action) ||
            action.type != WEREWOLF_UI_ACTION_TOGGLE_READY ||
            action.seat != 0U ||
            !werewolf_ui_action_matches_model(&action, model) ||
            werewolf_ui_take_feedback() !=
                (expected_ready ? WEREWOLF_UI_FEEDBACK_READY_ON
                                : WEREWOLF_UI_FEEDBACK_READY_OFF) ||
            werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                   WEREWOLF_UI_KEY_EVENT_LONG, &action)) {
            fprintf(stderr, "Host self READY interaction mismatch: %s\n",
                    state);
            return false;
        }
        if (!werewolf_ui_handle_key(WEREWOLF_UI_KEY_DOWN,
                                    WEREWOLF_UI_KEY_EVENT_LONG, &action) ||
            action.type != WEREWOLF_UI_ACTION_REQUEST_CLOSE_ROOM ||
            !werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr, "Host LONG DOWN exit mismatch: %s\n", state);
            return false;
        }
        ++model->revision;
        if (!werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr,
                    "Host close request was dropped by a lobby refresh\n");
            return false;
        }
        model->room_close_prompt = true;
        if (werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr,
                    "Host close request crossed into an open prompt\n");
            return false;
        }
        model->room_close_prompt = false;
        --model->revision;
    } else if (strcmp(state, "lobby-host-guest-focus") == 0) {
        if (werewolf_ui_handle_key(WEREWOLF_UI_KEY_DOWN,
                                   WEREWOLF_UI_KEY_EVENT_CLICK, &action) ||
            werewolf_ui_take_feedback() != WEREWOLF_UI_FEEDBACK_MOVE ||
            werewolf_ui_handle_key(WEREWOLF_UI_KEY_UP,
                                   WEREWOLF_UI_KEY_EVENT_CLICK, &action) ||
            werewolf_ui_take_feedback() != WEREWOLF_UI_FEEDBACK_MOVE ||
            werewolf_ui_handle_key(WEREWOLF_UI_KEY_DOWN,
                                   WEREWOLF_UI_KEY_EVENT_CLICK, &action) ||
            werewolf_ui_take_feedback() != WEREWOLF_UI_FEEDBACK_MOVE ||
            !werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                    WEREWOLF_UI_KEY_EVENT_CLICK, &action) ||
            action.type != WEREWOLF_UI_ACTION_OPEN_PLAYER_ACTION ||
            action.seat != 1U ||
            !werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr, "Host lobby guest-focus interaction mismatch\n");
            return false;
        }
        ++model->revision;
        if (!werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr,
                    "Host player OPEN was dropped by a lobby refresh\n");
            return false;
        }
        model->page = WEREWOLF_UI_PAGE_PLAYER_ACTION;
        if (werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr, "Host player OPEN crossed its source page\n");
            return false;
        }
        model->page = WEREWOLF_UI_PAGE_LOBBY;
        --model->revision;
        werewolf_ui_cancel_pending_action();
        if (!werewolf_ui_handle_key(WEREWOLF_UI_KEY_DOWN,
                                    WEREWOLF_UI_KEY_EVENT_LONG, &action) ||
            action.type != WEREWOLF_UI_ACTION_REQUEST_CLOSE_ROOM ||
            !werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr, "Focused Host LONG DOWN exit mismatch\n");
            return false;
        }
    } else if (strcmp(state, "player-detail") == 0 ||
               strcmp(state, "player-detail-back") == 0) {
        if (!model->has_verify_code ||
            model->verify_code != PREVIEW_VERIFY_CODE ||
            werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                   WEREWOLF_UI_KEY_EVENT_LONG, &action) ||
            !werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                    WEREWOLF_UI_KEY_EVENT_CLICK, &action) ||
            action.type != WEREWOLF_UI_ACTION_CLOSE_PLAYER_ACTION ||
            action.seat != 1U ||
            !werewolf_ui_action_matches_model(&action, model) ||
            werewolf_ui_take_feedback() != WEREWOLF_UI_FEEDBACK_SELECT) {
            fprintf(stderr, "Player detail default BACK mismatch\n");
            return false;
        }
        ++model->revision;
        if (!werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr,
                    "Player detail BACK was dropped by a profile refresh\n");
            return false;
        }
        model->page = WEREWOLF_UI_PAGE_LOBBY;
        if (werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr, "Player detail BACK crossed its source page\n");
            return false;
        }
        model->page = WEREWOLF_UI_PAGE_PLAYER_ACTION;
        model->selected_seat = 0U;
        if (werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr, "Player detail BACK crossed its target seat\n");
            return false;
        }
        model->selected_seat = 1U;
        model->player_kicking = true;
        if (werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr, "Player detail BACK crossed its progress state\n");
            return false;
        }
        model->player_kicking = false;
        --model->revision;
        werewolf_ui_cancel_pending_action();
        if (!werewolf_ui_handle_key(WEREWOLF_UI_KEY_DOWN,
                                    WEREWOLF_UI_KEY_EVENT_LONG, &action) ||
            action.type != WEREWOLF_UI_ACTION_CLOSE_PLAYER_ACTION ||
            action.seat != 1U ||
            !werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr, "Player detail LONG DOWN back mismatch\n");
            return false;
        }
        werewolf_ui_cancel_pending_action();
    } else if (strcmp(state, "player-detail-kick") == 0) {
        if (!model->has_verify_code ||
            model->verify_code != PREVIEW_VERIFY_CODE ||
            werewolf_ui_handle_key(WEREWOLF_UI_KEY_DOWN,
                                   WEREWOLF_UI_KEY_EVENT_CLICK, &action) ||
            werewolf_ui_take_feedback() != WEREWOLF_UI_FEEDBACK_MOVE ||
            werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                   WEREWOLF_UI_KEY_EVENT_LONG, &action) ||
            !werewolf_ui_handle_key(WEREWOLF_UI_KEY_DOWN,
                                    WEREWOLF_UI_KEY_EVENT_LONG, &action) ||
            action.type != WEREWOLF_UI_ACTION_CLOSE_PLAYER_ACTION ||
            action.seat != 1U ||
            !werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr, "Player detail KICK-focus back mismatch\n");
            return false;
        }
        werewolf_ui_cancel_pending_action();
        if (werewolf_ui_handle_key(WEREWOLF_UI_KEY_DOWN,
                                   WEREWOLF_UI_KEY_EVENT_CLICK, &action) ||
            werewolf_ui_take_feedback() != WEREWOLF_UI_FEEDBACK_MOVE ||
            !werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                    WEREWOLF_UI_KEY_EVENT_CLICK, &action) ||
            action.type != WEREWOLF_UI_ACTION_REQUEST_KICK_PLAYER ||
            action.seat != 1U ||
            !werewolf_ui_action_matches_model(&action, model) ||
            werewolf_ui_take_feedback() != WEREWOLF_UI_FEEDBACK_SELECT) {
            fprintf(stderr, "Direct player KICK interaction mismatch\n");
            return false;
        }
        ++model->revision;
        if (!werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr,
                    "Direct player KICK was dropped by a profile refresh\n");
            return false;
        }
        model->page = WEREWOLF_UI_PAGE_LOBBY;
        if (werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr, "Direct player KICK crossed its source page\n");
            return false;
        }
        model->page = WEREWOLF_UI_PAGE_PLAYER_ACTION;
        model->selected_seat = 0U;
        if (werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr, "Direct player KICK crossed its target seat\n");
            return false;
        }
        model->selected_seat = 1U;
        model->player_kicking = true;
        if (werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr, "Direct player KICK crossed its progress state\n");
            return false;
        }
        model->player_kicking = false;
        --model->revision;
        werewolf_ui_cancel_pending_action();
        if (werewolf_ui_handle_key(WEREWOLF_UI_KEY_DOWN,
                                   WEREWOLF_UI_KEY_EVENT_CLICK, &action) ||
            werewolf_ui_take_feedback() != WEREWOLF_UI_FEEDBACK_MOVE) {
            fprintf(stderr, "Unable to restore direct KICK preview focus\n");
            return false;
        }
    } else if (strcmp(state, "lobby-host-all-ready") == 0 ||
               strcmp(state, "lobby-ready") == 0) {
        if (!werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                    WEREWOLF_UI_KEY_EVENT_CLICK, &action) ||
            action.type != WEREWOLF_UI_ACTION_TOGGLE_READY ||
            action.seat != 0U ||
            !werewolf_ui_action_matches_model(&action, model) ||
            werewolf_ui_take_feedback() !=
                WEREWOLF_UI_FEEDBACK_READY_OFF ||
            !werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                    WEREWOLF_UI_KEY_EVENT_LONG, &action) ||
            action.type != WEREWOLF_UI_ACTION_START_GAME ||
            !werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr, "All-ready Host START interaction mismatch\n");
            return false;
        }
        werewolf_ui_cancel_pending_action();
    } else if (strcmp(state, "player-kicking") == 0 ||
               strcmp(state, "lobby-host-closing") == 0) {
        static const werewolf_ui_key_t keys[] = {
            WEREWOLF_UI_KEY_UP,
            WEREWOLF_UI_KEY_DOWN,
            WEREWOLF_UI_KEY_OK,
        };
        static const werewolf_ui_key_event_t events[] = {
            WEREWOLF_UI_KEY_EVENT_PRESS,
            WEREWOLF_UI_KEY_EVENT_RELEASE,
            WEREWOLF_UI_KEY_EVENT_CLICK,
            WEREWOLF_UI_KEY_EVENT_LONG,
        };

        for (size_t key_index = 0U;
             key_index < sizeof(keys) / sizeof(keys[0]); ++key_index) {
            for (size_t event_index = 0U;
                 event_index < sizeof(events) / sizeof(events[0]);
                 ++event_index) {
                if (werewolf_ui_handle_key(keys[key_index],
                                           events[event_index], &action)) {
                    fprintf(stderr,
                            "Locked progress state accepted input: %s\n",
                            state);
                    return false;
                }
            }
        }
    } else if (strcmp(state, "lobby-host-exit-back") == 0) {
        werewolf_ui_action_t blocked_action = {0};

        if (!werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                    WEREWOLF_UI_KEY_EVENT_CLICK, &action) ||
            action.type != WEREWOLF_UI_ACTION_CANCEL_CLOSE_ROOM ||
            !werewolf_ui_action_matches_model(&action, model) ||
            werewolf_ui_take_feedback() !=
                WEREWOLF_UI_FEEDBACK_SELECT) {
            fprintf(stderr, "Host close short-OK BACK mismatch\n");
            return false;
        }
        if (werewolf_ui_handle_key(WEREWOLF_UI_KEY_UP,
                                   WEREWOLF_UI_KEY_EVENT_CLICK,
                                   &blocked_action) ||
            werewolf_ui_take_feedback() != WEREWOLF_UI_FEEDBACK_NONE) {
            fprintf(stderr,
                    "Host close accepted duplicate input before model ACK\n");
            return false;
        }
        ++model->revision;
        if (!werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr,
                    "Host close BACK was dropped by a lobby refresh\n");
            return false;
        }
        model->room_close_prompt = false;
        ++model->revision;
        werewolf_ui_set_model(model);
        if (!werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                    WEREWOLF_UI_KEY_EVENT_CLICK, &action) ||
            action.type != WEREWOLF_UI_ACTION_TOGGLE_READY ||
            action.seat != 0U ||
            !werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr,
                    "Host lobby stayed latched after short-OK BACK\n");
            return false;
        }
        (void)werewolf_ui_take_feedback();

        model->room_close_prompt = true;
        ++model->revision;
        werewolf_ui_set_model(model);
        if (!werewolf_ui_handle_key(WEREWOLF_UI_KEY_DOWN,
                                    WEREWOLF_UI_KEY_EVENT_LONG, &action) ||
            action.type != WEREWOLF_UI_ACTION_CANCEL_CLOSE_ROOM ||
            !werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr, "Host close LONG DOWN back mismatch\n");
            return false;
        }
        ++model->revision;
        if (!werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr,
                    "Host close LONG DOWN was dropped by a lobby refresh\n");
            return false;
        }
        model->room_close_prompt = false;
        ++model->revision;
        werewolf_ui_set_model(model);
        if (!werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                    WEREWOLF_UI_KEY_EVENT_CLICK, &action) ||
            action.type != WEREWOLF_UI_ACTION_TOGGLE_READY ||
            action.seat != 0U ||
            !werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr,
                    "Host lobby stayed latched after LONG DOWN BACK\n");
            return false;
        }
        (void)werewolf_ui_take_feedback();

        /* Restore the candidate frame to the default BACK-focused modal. */
        model->room_close_prompt = true;
        ++model->revision;
        werewolf_ui_set_model(model);

        /* Queue saturation rolls back only the local action gate and leaves
         * the prompt open. BACK must be immediately retryable. */
        if (!werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                    WEREWOLF_UI_KEY_EVENT_CLICK, &action) ||
            action.type != WEREWOLF_UI_ACTION_CANCEL_CLOSE_ROOM) {
            fprintf(stderr, "Host close BACK rollback setup mismatch\n");
            return false;
        }
        werewolf_ui_cancel_pending_action();
        if (!werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                    WEREWOLF_UI_KEY_EVENT_CLICK, &action) ||
            action.type != WEREWOLF_UI_ACTION_CANCEL_CLOSE_ROOM ||
            !werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr, "Host close BACK rollback retry mismatch\n");
            return false;
        }
        werewolf_ui_cancel_pending_action();
    } else if (strcmp(state, "lobby-host-exit-returned") == 0) {
        if (!werewolf_ui_handle_key(WEREWOLF_UI_KEY_DOWN,
                                    WEREWOLF_UI_KEY_EVENT_LONG, &action) ||
            action.type != WEREWOLF_UI_ACTION_CANCEL_CLOSE_ROOM ||
            !werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr,
                    "Host close returned-frame cancel mismatch\n");
            return false;
        }
        model->room_close_prompt = false;
        ++model->revision;
        werewolf_ui_set_model(model);
    } else if (strcmp(state, "lobby-host-exit-close") == 0) {
        (void)werewolf_ui_handle_key(WEREWOLF_UI_KEY_DOWN,
                                     WEREWOLF_UI_KEY_EVENT_CLICK, &action);
        if (werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                   WEREWOLF_UI_KEY_EVENT_CLICK, &action)) {
            fprintf(stderr, "Short OK must not close a room\n");
            return false;
        }
        if (!werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                    WEREWOLF_UI_KEY_EVENT_LONG, &action) ||
            action.type != WEREWOLF_UI_ACTION_CONFIRM_CLOSE_ROOM ||
            !werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr, "Host close confirmation mismatch\n");
            return false;
        }
        ++model->revision;
        if (!werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr,
                    "Host close confirmation was dropped by a lobby refresh\n");
            return false;
        }
        model->room_close_prompt = false;
        model->room_closing = true;
        model->input_enabled = false;
        ++model->revision;
        werewolf_ui_set_model(model);
        if (werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr,
                    "Host close confirmation crossed into progress state\n");
            return false;
        }

        /* Restore the CLOSE-focused candidate after exercising the real
         * prompt-to-progress model transition. */
        model->room_closing = false;
        model->input_enabled = true;
        model->room_close_prompt = true;
        ++model->revision;
        werewolf_ui_set_model(model);
        (void)werewolf_ui_handle_key(WEREWOLF_UI_KEY_DOWN,
                                     WEREWOLF_UI_KEY_EVENT_CLICK, &action);
    } else if (room_closed_state) {
        static const werewolf_ui_key_t ignored_keys[] = {
            WEREWOLF_UI_KEY_UP,
            WEREWOLF_UI_KEY_DOWN,
        };
        static const werewolf_ui_key_event_t events[] = {
            WEREWOLF_UI_KEY_EVENT_PRESS,
            WEREWOLF_UI_KEY_EVENT_RELEASE,
            WEREWOLF_UI_KEY_EVENT_CLICK,
            WEREWOLF_UI_KEY_EVENT_LONG,
        };

        for (size_t key_index = 0U;
             key_index < sizeof(ignored_keys) / sizeof(ignored_keys[0]);
             ++key_index) {
            for (size_t event_index = 0U;
                 event_index < sizeof(events) / sizeof(events[0]);
                 ++event_index) {
                if (werewolf_ui_handle_key(ignored_keys[key_index],
                                           events[event_index], &action)) {
                    fprintf(stderr,
                            "Guest room-closed navigation emitted action\n");
                    return false;
                }
            }
        }
        if (werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                   WEREWOLF_UI_KEY_EVENT_PRESS, &action) ||
            werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                   WEREWOLF_UI_KEY_EVENT_RELEASE, &action) ||
            werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                   WEREWOLF_UI_KEY_EVENT_LONG, &action) ||
            !werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                    WEREWOLF_UI_KEY_EVENT_CLICK, &action) ||
            action.type != WEREWOLF_UI_ACTION_ACK_ROOM_CLOSED ||
            !werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr, "Guest room-closed only-OK action mismatch\n");
            return false;
        }
    } else if (strcmp(state, "room-list") == 0 ||
               strcmp(state, "room-list-multiple") == 0) {
        if (werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                   WEREWOLF_UI_KEY_EVENT_LONG, &action) ||
            !werewolf_ui_handle_key(WEREWOLF_UI_KEY_DOWN,
                                    WEREWOLF_UI_KEY_EVENT_CLICK, &action) ||
            action.type != WEREWOLF_UI_ACTION_SELECT_ROOM ||
            action.room_token != 2U ||
            !werewolf_ui_action_matches_model(&action, model) ||
            !werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                    WEREWOLF_UI_KEY_EVENT_CLICK, &action) ||
            action.type != WEREWOLF_UI_ACTION_JOIN_SELECTED_ROOM ||
            action.room_token != 2U ||
            !werewolf_ui_action_matches_model(&action, model) ||
            !werewolf_ui_handle_key(WEREWOLF_UI_KEY_DOWN,
                                    WEREWOLF_UI_KEY_EVENT_LONG, &action) ||
            action.type != WEREWOLF_UI_ACTION_LEAVE_GAME ||
            !werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr, "Room-list selection action mismatch\n");
            return false;
        }
    } else if (strcmp(state, "room-list-empty") == 0) {
        if (werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                   WEREWOLF_UI_KEY_EVENT_LONG, &action) ||
            !werewolf_ui_handle_key(WEREWOLF_UI_KEY_DOWN,
                                    WEREWOLF_UI_KEY_EVENT_LONG, &action) ||
            action.type != WEREWOLF_UI_ACTION_LEAVE_GAME ||
            !werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr, "Empty room-list back action mismatch\n");
            return false;
        }
    }

    if (strcmp(state, "connection-scanning") == 0 ||
        strcmp(state, "connection-pairing") == 0 ||
        strcmp(state, "lobby-guest-wait") == 0 ||
        strcmp(state, "lobby-guest-ready") == 0) {
        bool lobby = strcmp(state, "lobby-guest-wait") == 0 ||
                     strcmp(state, "lobby-guest-ready") == 0;
        bool toggled = !lobby || werewolf_ui_handle_key(
            WEREWOLF_UI_KEY_OK, WEREWOLF_UI_KEY_EVENT_CLICK, &action);
        werewolf_ui_feedback_t expected_feedback =
            strcmp(state, "lobby-guest-ready") == 0
                ? WEREWOLF_UI_FEEDBACK_READY_OFF
                : WEREWOLF_UI_FEEDBACK_READY_ON;

        if ((lobby &&
             (!model->has_verify_code ||
              model->verify_code != PREVIEW_VERIFY_CODE)) ||
            !toggled ||
            (lobby &&
             (action.type != WEREWOLF_UI_ACTION_TOGGLE_READY ||
              action.seat != model->local_seat ||
              !werewolf_ui_action_matches_model(&action, model) ||
              werewolf_ui_take_feedback() != expected_feedback)) ||
            werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                   WEREWOLF_UI_KEY_EVENT_LONG, &action) ||
            !werewolf_ui_handle_key(WEREWOLF_UI_KEY_DOWN,
                                    WEREWOLF_UI_KEY_EVENT_LONG, &action) ||
            action.type != WEREWOLF_UI_ACTION_LEAVE_GAME ||
            !werewolf_ui_action_matches_model(&action, model) ||
            werewolf_ui_take_feedback() !=
                WEREWOLF_UI_FEEDBACK_CONFIRMED) {
            fprintf(stderr, "Join cancellation action mismatch: %s\n", state);
            return false;
        }
    }

    if (strcmp(state, "error-recoverable") == 0) {
        if (!werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                    WEREWOLF_UI_KEY_EVENT_CLICK, &action) ||
            action.type != WEREWOLF_UI_ACTION_RETRY ||
            !werewolf_ui_action_matches_model(&action, model) ||
            werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                   WEREWOLF_UI_KEY_EVENT_LONG, &action) ||
            !werewolf_ui_handle_key(WEREWOLF_UI_KEY_DOWN,
                                    WEREWOLF_UI_KEY_EVENT_LONG, &action) ||
            action.type != WEREWOLF_UI_ACTION_LEAVE_GAME ||
            !werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr, "Recoverable error input contract mismatch\n");
            return false;
        }
    }

    if (strcmp(state, "role-revealed") == 0 ||
        strcmp(state, "role-release") == 0) {
        (void)werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                     WEREWOLF_UI_KEY_EVENT_PRESS, &action);
        (void)werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                     WEREWOLF_UI_KEY_EVENT_LONG, &action);
        if (werewolf_ui_take_feedback() !=
            WEREWOLF_UI_FEEDBACK_PRIVATE_REVEAL) {
            fprintf(stderr, "Role reveal feedback mismatch: %s\n", state);
            return false;
        }
    }
    if (strcmp(state, "role-release") == 0) {
        (void)werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                     WEREWOLF_UI_KEY_EVENT_RELEASE, &action);
        if (werewolf_ui_take_feedback() !=
            WEREWOLF_UI_FEEDBACK_PRIVATE_SEAL) {
            fprintf(stderr, "Role seal feedback mismatch\n");
            return false;
        }
    } else if (strcmp(state, "role-long-unarmed") == 0) {
        (void)werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                     WEREWOLF_UI_KEY_EVENT_LONG, &action);
    } else if (strcmp(state, "role-heartbeat") == 0) {
        (void)werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                     WEREWOLF_UI_KEY_EVENT_PRESS, &action);
        ++model->revision;
        werewolf_ui_set_model(model);
        (void)werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                     WEREWOLF_UI_KEY_EVENT_LONG, &action);
    } else if (strcmp(state, "role-private-epoch-changed") == 0) {
        (void)werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                     WEREWOLF_UI_KEY_EVENT_PRESS, &action);
        ++model->revision;
        ++model->private_epoch;
        werewolf_ui_set_model(model);
        (void)werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                     WEREWOLF_UI_KEY_EVENT_LONG, &action);
    } else if (strcmp(state, "private-good") == 0 ||
               strcmp(state, "private-good-no-pending") == 0 ||
               strcmp(state, "private-no-result") == 0) {
        (void)werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                     WEREWOLF_UI_KEY_EVENT_PRESS, &action);
        (void)werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                     WEREWOLF_UI_KEY_EVENT_LONG, &action);
        if (werewolf_ui_take_feedback() != WEREWOLF_UI_FEEDBACK_NONE) {
            fprintf(stderr, "Private result must stay silent\n");
            return false;
        }
    } else if (strcmp(state, "private-waiting") == 0) {
        if (werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                   WEREWOLF_UI_KEY_EVENT_PRESS, &action) ||
            werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                   WEREWOLF_UI_KEY_EVENT_LONG, &action)) {
            fprintf(stderr, "Waiting private page accepted input\n");
            return false;
        }
    } else if (strcmp(state, "night-confirm") == 0 ||
               strcmp(state, "vote-confirm") == 0) {
        werewolf_ui_action_type_t expected_type =
            strcmp(state, "vote-confirm") == 0
                ? WEREWOLF_UI_ACTION_SUBMIT_VOTE
                : WEREWOLF_UI_ACTION_SUBMIT_NIGHT_TARGET;

        if (werewolf_ui_handle_key(WEREWOLF_UI_KEY_DOWN,
                                   WEREWOLF_UI_KEY_EVENT_LONG, &action) ||
            werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                   WEREWOLF_UI_KEY_EVENT_CLICK, &action) ||
            !werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                    WEREWOLF_UI_KEY_EVENT_LONG, &action) ||
            action.type != expected_type ||
            !werewolf_ui_action_matches_model(&action, model)) {
            fprintf(stderr, "Target confirmation input mismatch: %s\n",
                    state);
            return false;
        }
        werewolf_ui_cancel_pending_action();
        if (werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                   WEREWOLF_UI_KEY_EVENT_CLICK, &action)) {
            fprintf(stderr, "Unable to restore confirmation preview: %s\n",
                    state);
            return false;
        }
    } else if (strcmp(state, "night-select") == 0) {
        (void)werewolf_ui_handle_key(WEREWOLF_UI_KEY_UP,
                                     WEREWOLF_UI_KEY_EVENT_CLICK, &action);
        if (werewolf_ui_take_feedback() != WEREWOLF_UI_FEEDBACK_NONE) {
            fprintf(stderr, "Night navigation must stay silent\n");
            return false;
        }
        (void)werewolf_ui_handle_key(WEREWOLF_UI_KEY_DOWN,
                                     WEREWOLF_UI_KEY_EVENT_CLICK, &action);
        if (werewolf_ui_take_feedback() != WEREWOLF_UI_FEEDBACK_NONE) {
            fprintf(stderr, "Night navigation must stay silent\n");
            return false;
        }
    } else if (strcmp(state, "vote-select") == 0) {
        (void)werewolf_ui_handle_key(WEREWOLF_UI_KEY_UP,
                                     WEREWOLF_UI_KEY_EVENT_CLICK, &action);
        if (werewolf_ui_take_feedback() != WEREWOLF_UI_FEEDBACK_MOVE) {
            fprintf(stderr, "Vote navigation feedback mismatch\n");
            return false;
        }
        (void)werewolf_ui_handle_key(WEREWOLF_UI_KEY_DOWN,
                                     WEREWOLF_UI_KEY_EVENT_CLICK, &action);
        if (werewolf_ui_take_feedback() != WEREWOLF_UI_FEEDBACK_MOVE) {
            fprintf(stderr, "Vote navigation feedback mismatch\n");
            return false;
        }
    } else if (strcmp(state, "role-heartbeat-release") == 0 ||
               strcmp(state, "role-private-epoch-release") == 0) {
        (void)werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                     WEREWOLF_UI_KEY_EVENT_PRESS, &action);
        (void)werewolf_ui_handle_key(WEREWOLF_UI_KEY_OK,
                                     WEREWOLF_UI_KEY_EVENT_LONG, &action);
        bool emitted = werewolf_ui_handle_key(
            WEREWOLF_UI_KEY_OK, WEREWOLF_UI_KEY_EVENT_RELEASE, &action);
        ++model->revision;
        if (strcmp(state, "role-private-epoch-release") == 0) {
            ++model->private_epoch;
        }
        bool matches = werewolf_ui_action_matches_model(&action, model);
        bool expected = strcmp(state, "role-heartbeat-release") == 0;
        if (!emitted || matches != expected) {
            fprintf(stderr,
                    "Private action context check failed: state=%s "
                    "emitted=%d matches=%d expected=%d\n",
                    state, emitted, matches, expected);
            return false;
        }
        werewolf_ui_set_model(model);
    }
    return true;
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s --state <state> --output <preview.png>\n",
            program);
}

int main(int argc, char **argv)
{
    const char *state = "mode-create";
    const char *output = "werewolf-preview.png";

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--state") == 0 && i + 1 < argc) {
            state = argv[++i];
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else {
            print_usage(argv[0]);
            return 2;
        }
    }

    werewolf_ui_model_t model;
    if (!configure_state(&model, state)) {
        fprintf(stderr, "Unknown preview state: %s\n", state);
        print_usage(argv[0]);
        return 2;
    }

    lv_init();
    lv_display_t *display = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    if (!display) {
        fprintf(stderr, "Unable to create LVGL display\n");
        return 1;
    }
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, display_flush);
    lv_display_set_buffers(display, s_draw_buffer, NULL, sizeof(s_draw_buffer),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_default(display);

    if (!werewolf_ui_create(&model)) {
        fprintf(stderr, "Unable to create production Werewolf UI\n");
        return 1;
    }
    if (!apply_state_events(&model, state)) {
        return 1;
    }
    lv_obj_update_layout(werewolf_ui_screen());
    lv_refr_now(display);

    if (png_write_rgb565(output, s_framebuffer,
                         DISPLAY_WIDTH, DISPLAY_HEIGHT) != 0) {
        fprintf(stderr, "Unable to write preview: %s\n", output);
        return 1;
    }
    printf("rendered state=%s output=%s\n", state, output);
    werewolf_ui_destroy();
    return 0;
}
