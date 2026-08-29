#include "werewolf_ui.h"

#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(ui_font_kode_regular_11);
LV_FONT_DECLARE(ui_font_kode_regular_13);
LV_FONT_DECLARE(ui_font_kode_bold_13);
LV_FONT_DECLARE(ui_font_kode_bold_15);
LV_FONT_DECLARE(ui_font_kode_bold_21);

/* ECLIPSE LEDGER / 月蚀村志. Werewolf owns this visual language. The
 * upstream/demo firmware remains a BSP reference only; no upstream UI asset,
 * primitive or screen state enters this module. A cropped eclipse is the one
 * atmospheric motif, while every color and mark below carries game state. */
#define WW_UI_BG          0x09090BU
#define WW_UI_PANEL       0x101217U
#define WW_UI_PANEL_WARM  0x17110FU
#define WW_UI_PANEL_ALT   0x1A1412U
#define WW_UI_FOCUS       0xD0A154U
#define WW_UI_FOCUS_TEXT  0x09090BU
#define WW_UI_STRUCTURE   0x3A2A25U
#define WW_UI_TEXT        0xE8DEC8U
#define WW_UI_SUCCESS     0x86A68CU
#define WW_UI_WARNING     0xB77735U
#define WW_UI_ERROR       0xA83F3BU
#define WW_UI_MUTED       0x817B75U
#define WW_UI_ERROR_PANEL 0x241315U
#define WW_UI_MOON_DIM    0x2A2525U

#define CELL_COUNT WEREWOLF_UI_PLAYER_COUNT
#define HEADER_RIGHT 232
#define BATTERY_SEGMENT_WIDTH 8
#define BATTERY_SEGMENT_HEIGHT 5
#define BATTERY_SEGMENT_GAP 3
#define HEADER_BATTERY_WIDTH                                              \
    ((int)WEREWOLF_UI_BATTERY_SEGMENTS * BATTERY_SEGMENT_WIDTH +         \
     ((int)WEREWOLF_UI_BATTERY_SEGMENTS - 1) * BATTERY_SEGMENT_GAP)
#define HEADER_LINK_X 8
#define HEADER_LINK_Y 25
#define HEADER_SIGNAL_X 30
#define HEADER_SIGNAL_BASE_Y 36
#define HEADER_SIGNAL_SIZE 4
#define HEADER_SIGNAL_GAP 3
#define HEADER_SIGNAL_WIDTH                                               \
    ((int)WEREWOLF_UI_SIGNAL_SEGMENTS * HEADER_SIGNAL_SIZE +             \
     ((int)WEREWOLF_UI_SIGNAL_SEGMENTS - 1) * HEADER_SIGNAL_GAP)
#define HEADER_PLAYER_Y 24
#define HEADER_PLAYER_WIDTH_UNIT 10
#define HEADER_PLAYER_HEIGHT 12
#define HEADER_PLAYER_GAP 3
#define HEADER_PLAYER_WIDTH                                               \
    ((int)CELL_COUNT * HEADER_PLAYER_WIDTH_UNIT +                         \
     ((int)CELL_COUNT - 1) * HEADER_PLAYER_GAP)
#define HEADER_PLAYER_START_X (HEADER_RIGHT - HEADER_PLAYER_WIDTH)

_Static_assert(HEADER_PLAYER_START_X + HEADER_PLAYER_WIDTH == HEADER_RIGHT,
               "player strip must share the battery right edge");
_Static_assert(HEADER_RIGHT - HEADER_BATTERY_WIDTH + HEADER_BATTERY_WIDTH ==
                   HEADER_RIGHT,
               "battery strip must end at HEADER_RIGHT");
_Static_assert(HEADER_SIGNAL_X + HEADER_SIGNAL_WIDTH < HEADER_PLAYER_START_X,
               "signal and player strips must not overlap");

static lv_obj_t *s_screen;
static lv_obj_t *s_panel;
static lv_obj_t *s_moon_disc;
static lv_obj_t *s_moon_cutout;
static lv_obj_t *s_moon_horizon;
static lv_obj_t *s_title;
static lv_obj_t *s_subtitle;
static lv_obj_t *s_detail;
static lv_obj_t *s_timer;
static lv_obj_t *s_footer;
static lv_obj_t *s_banner;
static lv_obj_t *s_banner_label;
static lv_obj_t *s_header_phase;
static lv_obj_t *s_header_link_nodes[2];
static lv_obj_t *s_header_link_bridges[3];
static lv_obj_t *s_header_signal_segments[WEREWOLF_UI_SIGNAL_SEGMENTS];
static lv_obj_t *s_header_player_slots[CELL_COUNT];
static lv_obj_t *s_header_player_fills[CELL_COUNT];
static lv_obj_t *s_header_player_cuts[CELL_COUNT];
static lv_obj_t *s_header_battery_segments[WEREWOLF_UI_BATTERY_SEGMENTS];
static lv_obj_t *s_cells[CELL_COUNT];
static lv_obj_t *s_cell_bars[CELL_COUNT];
static lv_obj_t *s_cell_labels[CELL_COUNT];
static lv_obj_t *s_lobby_seat_labels[CELL_COUNT];
static lv_obj_t *s_lobby_identity_badges[CELL_COUNT];
static lv_obj_t *s_lobby_identity_labels[CELL_COUNT];
static lv_obj_t *s_lobby_name_labels[CELL_COUNT];
static lv_obj_t *s_lobby_state_labels[CELL_COUNT];
static lv_obj_t *s_modal_overlay;
static lv_obj_t *s_modal_dialog;
static lv_obj_t *s_modal_rail;
static lv_obj_t *s_modal_title;
static lv_obj_t *s_modal_status;
static lv_obj_t *s_modal_body;
static lv_obj_t *s_modal_options[2];
static lv_obj_t *s_modal_option_bars[2];
static lv_obj_t *s_modal_option_labels[2];

static werewolf_ui_model_t s_model;
static werewolf_ui_mode_t s_mode_cursor;
static uint8_t s_target_cursor = WEREWOLF_UI_NO_SEAT;
static uint8_t s_lobby_cursor = WEREWOLF_UI_NO_SEAT;
static uint32_t s_room_cursor_token;
static bool s_player_action_focus_kick;
static bool s_confirm_pending;
static bool s_private_revealed;
static bool s_private_press_armed;
static werewolf_ui_page_t s_private_press_page;
static uint32_t s_private_press_epoch;
static bool s_action_latched;
static bool s_close_focus_close;
static werewolf_ui_feedback_t s_feedback;

static const werewolf_ui_player_t *player_by_seat(uint8_t seat);

static const char *role_name(werewolf_ui_role_t role)
{
    switch (role) {
    case WEREWOLF_UI_ROLE_WOLF:
        return "WOLF";
    case WEREWOLF_UI_ROLE_SEER:
        return "SEER";
    case WEREWOLF_UI_ROLE_GUARD:
        return "GUARD";
    case WEREWOLF_UI_ROLE_VILLAGER:
        return "VILLAGER";
    default:
        return "UNKNOWN";
    }
}

static const char *faction_name(werewolf_ui_faction_t faction)
{
    switch (faction) {
    case WEREWOLF_UI_FACTION_GOOD:
        return "FACTION  GOOD";
    case WEREWOLF_UI_FACTION_WOLVES:
        return "FACTION  WOLF";
    default:
        return "RESULT UNAVAILABLE";
    }
}

static const char *error_name(werewolf_ui_error_t error)
{
    switch (error) {
    case WEREWOLF_UI_ERROR_ROOM_NOT_FOUND:
        return "ROOM NOT FOUND";
    case WEREWOLF_UI_ERROR_ROOM_FULL:
        return "ROOM IS FULL";
    case WEREWOLF_UI_ERROR_TIMEOUT:
        return "REQUEST TIMEOUT";
    case WEREWOLF_UI_ERROR_PROTOCOL:
        return "PROTOCOL ERROR";
    case WEREWOLF_UI_ERROR_HARDWARE:
        return "HARDWARE FAILURE";
    case WEREWOLF_UI_ERROR_HOST_LOST:
        return "HOST LOST  GAME ABORTED";
    default:
        return "UNKNOWN ERROR";
    }
}

static const char *error_title(werewolf_ui_error_t error)
{
    if (error == WEREWOLF_UI_ERROR_PROTOCOL) {
        return "PROTOCOL ERROR";
    }
    if (error == WEREWOLF_UI_ERROR_HARDWARE) {
        return "HARDWARE ERROR";
    }
    return "CONNECTION ERROR";
}

static const char *winner_name(werewolf_ui_winner_t winner)
{
    switch (winner) {
    case WEREWOLF_UI_WINNER_GOOD:
        return "GOOD TEAM WINS";
    case WEREWOLF_UI_WINNER_WOLVES:
        return "WOLVES WIN";
    case WEREWOLF_UI_WINNER_ABORTED:
        return "GAME ABORTED";
    default:
        return "GAME OVER";
    }
}

static const char *text_or(const char *text, const char *fallback)
{
    return text && text[0] ? text : fallback;
}

/* Game/protocol player IDs are zero based.  Only the rendered label is one
 * based, so actions can pass straight to the authoritative game core without
 * an error-prone conversion at every boundary. */
static unsigned seat_number(uint8_t seat)
{
    return (unsigned)seat + 1U;
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int width, int height,
                     uint32_t background, lv_opa_t opacity)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_radius(object, 0, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(background), 0);
    lv_obj_set_style_bg_opa(object, opacity, 0);
    return object;
}

static lv_obj_t *label_create(lv_obj_t *parent, const char *text,
                              const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static lv_obj_t *centered_label(lv_obj_t *parent, const char *text,
                                const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = label_create(parent, text, font, color);
    lv_obj_center(label);
    return label;
}

static lv_obj_t *screen_create(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_bg_color(screen, lv_color_hex(WW_UI_BG), 0);

    box(screen, 0, 0, 240, 44, WW_UI_BG, LV_OPA_COVER);
    box(screen, 8, 42, 224, 1, WW_UI_STRUCTURE, LV_OPA_80);

    s_header_phase = label_create(screen, "MODE SELECT",
                                  &ui_font_kode_bold_15, WW_UI_TEXT);
    lv_obj_set_pos(s_header_phase, 8, 4);
    lv_obj_set_size(s_header_phase, 158, 18);
    lv_obj_set_style_text_letter_space(s_header_phase, 0, 0);

    lv_obj_t *link_group = box(screen, HEADER_LINK_X, HEADER_LINK_Y,
                               17, 11, WW_UI_BG, LV_OPA_TRANSP);
    s_header_link_nodes[0] = box(link_group, 0, 3, 5, 5,
                                 WW_UI_STRUCTURE, LV_OPA_50);
    s_header_link_nodes[1] = box(link_group, 12, 3, 5, 5,
                                 WW_UI_STRUCTURE, LV_OPA_50);
    lv_obj_set_style_radius(s_header_link_nodes[0], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_radius(s_header_link_nodes[1], LV_RADIUS_CIRCLE, 0);
    for (unsigned i = 0U; i < 3U; ++i) {
        s_header_link_bridges[i] = box(link_group, 5 + (int)i * 2, 5,
                                       3, 1, WW_UI_STRUCTURE, LV_OPA_20);
    }

    for (unsigned i = 0U; i < WEREWOLF_UI_SIGNAL_SEGMENTS; ++i) {
        int height = 3 + (int)i * 2;
        s_header_signal_segments[i] = box(
            screen,
            HEADER_SIGNAL_X +
                (int)i * (HEADER_SIGNAL_SIZE + HEADER_SIGNAL_GAP),
            HEADER_SIGNAL_BASE_Y - height, HEADER_SIGNAL_SIZE, height,
            WW_UI_STRUCTURE, LV_OPA_20);
        lv_obj_set_style_radius(s_header_signal_segments[i], 2, 0);
    }

    for (unsigned i = 0U; i < CELL_COUNT; ++i) {
        int x = HEADER_PLAYER_START_X +
                (int)i * (HEADER_PLAYER_WIDTH_UNIT + HEADER_PLAYER_GAP);
        lv_obj_t *slot = box(screen, x, HEADER_PLAYER_Y,
                             HEADER_PLAYER_WIDTH_UNIT, HEADER_PLAYER_HEIGHT,
                             WW_UI_STRUCTURE, LV_OPA_20);
        lv_obj_set_style_radius(slot, 4, 0);
        lv_obj_set_style_border_width(slot, 1, 0);
        lv_obj_set_style_border_color(slot, lv_color_hex(WW_UI_STRUCTURE), 0);
        lv_obj_set_style_border_opa(slot, LV_OPA_70, 0);
        s_header_player_slots[i] = slot;
        s_header_player_fills[i] = box(slot, 2, 2, 6, 8,
                                       WW_UI_SUCCESS, LV_OPA_TRANSP);
        lv_obj_set_style_radius(s_header_player_fills[i], 2, 0);
        s_header_player_cuts[i] = box(slot, 2, 5, 6, 2,
                                      WW_UI_BG, LV_OPA_TRANSP);
    }

    lv_obj_t *battery_group = box(screen,
                                  HEADER_RIGHT - HEADER_BATTERY_WIDTH, 5,
                                  HEADER_BATTERY_WIDTH,
                                  BATTERY_SEGMENT_HEIGHT,
                                  WW_UI_BG, LV_OPA_TRANSP);
    for (unsigned i = 0; i < WEREWOLF_UI_BATTERY_SEGMENTS; ++i) {
        s_header_battery_segments[i] = box(
            battery_group,
            (int)i * (BATTERY_SEGMENT_WIDTH + BATTERY_SEGMENT_GAP), 0,
            BATTERY_SEGMENT_WIDTH, BATTERY_SEGMENT_HEIGHT,
            WW_UI_MUTED, LV_OPA_40);
        lv_obj_set_style_radius(s_header_battery_segments[i], 2, 0);
    }
    return screen;
}

static void header_box_style(lv_obj_t *object, uint32_t color,
                             lv_opa_t opacity)
{
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, opacity, 0);
}

static void update_header_connection(void)
{
    for (unsigned i = 0U; i < 2U; ++i) {
        header_box_style(s_header_link_nodes[i], WW_UI_STRUCTURE, LV_OPA_50);
    }
    for (unsigned i = 0U; i < 3U; ++i) {
        header_box_style(s_header_link_bridges[i], WW_UI_STRUCTURE,
                         LV_OPA_TRANSP);
    }

    switch (s_model.connection) {
    case WEREWOLF_UI_CONNECTION_SCANNING:
        header_box_style(s_header_link_nodes[0], WW_UI_FOCUS, LV_OPA_COVER);
        header_box_style(s_header_link_nodes[1], WW_UI_STRUCTURE, LV_OPA_40);
        header_box_style(s_header_link_bridges[0], WW_UI_FOCUS, LV_OPA_40);
        break;
    case WEREWOLF_UI_CONNECTION_PAIRING:
        for (unsigned i = 0U; i < 2U; ++i) {
            header_box_style(s_header_link_nodes[i], WW_UI_FOCUS,
                             LV_OPA_COVER);
        }
        header_box_style(s_header_link_bridges[0], WW_UI_FOCUS,
                         LV_OPA_COVER);
        header_box_style(s_header_link_bridges[1], WW_UI_FOCUS, LV_OPA_40);
        break;
    case WEREWOLF_UI_CONNECTION_ONLINE:
        for (unsigned i = 0U; i < 2U; ++i) {
            header_box_style(s_header_link_nodes[i], WW_UI_SUCCESS,
                             LV_OPA_COVER);
        }
        for (unsigned i = 0U; i < 3U; ++i) {
            header_box_style(s_header_link_bridges[i], WW_UI_SUCCESS,
                             LV_OPA_COVER);
        }
        break;
    case WEREWOLF_UI_CONNECTION_RECONNECTING:
        for (unsigned i = 0U; i < 2U; ++i) {
            header_box_style(s_header_link_nodes[i], WW_UI_WARNING,
                             LV_OPA_COVER);
        }
        header_box_style(s_header_link_bridges[0], WW_UI_WARNING,
                         LV_OPA_COVER);
        header_box_style(s_header_link_bridges[2], WW_UI_WARNING,
                         LV_OPA_COVER);
        break;
    case WEREWOLF_UI_CONNECTION_DISCONNECTED:
        for (unsigned i = 0U; i < 2U; ++i) {
            header_box_style(s_header_link_nodes[i], WW_UI_ERROR,
                             LV_OPA_COVER);
        }
        break;
    case WEREWOLF_UI_CONNECTION_HOST_LOST:
        header_box_style(s_header_link_nodes[0], WW_UI_MUTED, LV_OPA_40);
        header_box_style(s_header_link_nodes[1], WW_UI_ERROR, LV_OPA_COVER);
        break;
    case WEREWOLF_UI_CONNECTION_RADIO_OFF:
        /* A single dormant endpoint makes OFF structurally different from a
         * live radio whose two red endpoints have become disconnected. */
        header_box_style(s_header_link_nodes[1], WW_UI_STRUCTURE,
                         LV_OPA_TRANSP);
        break;
    default:
        break;
    }
}

static werewolf_ui_signal_t visible_header_signal(void)
{
    switch (s_model.connection) {
    case WEREWOLF_UI_CONNECTION_RECONNECTING:
        return WEREWOLF_UI_SIGNAL_STALE;
    case WEREWOLF_UI_CONNECTION_DISCONNECTED:
    case WEREWOLF_UI_CONNECTION_HOST_LOST:
        return WEREWOLF_UI_SIGNAL_DISCONNECTED;
    case WEREWOLF_UI_CONNECTION_RADIO_OFF:
    case WEREWOLF_UI_CONNECTION_SCANNING:
    case WEREWOLF_UI_CONNECTION_PAIRING:
        return WEREWOLF_UI_SIGNAL_NO_SAMPLE;
    case WEREWOLF_UI_CONNECTION_ONLINE:
    default:
        return s_model.signal;
    }
}

static void update_header_signal(void)
{
    werewolf_ui_signal_t signal = visible_header_signal();
    unsigned filled = werewolf_ui_signal_filled_segments(signal);
    uint32_t active_color =
        signal == WEREWOLF_UI_SIGNAL_WEAK ||
                signal == WEREWOLF_UI_SIGNAL_FAIR
            ? WW_UI_WARNING
            : WW_UI_SUCCESS;

    for (unsigned i = 0U; i < WEREWOLF_UI_SIGNAL_SEGMENTS; ++i) {
        uint32_t color = WW_UI_STRUCTURE;
        lv_opa_t opacity = LV_OPA_20;

        if (i < filled) {
            color = active_color;
            opacity = LV_OPA_COVER;
        } else if (signal == WEREWOLF_UI_SIGNAL_STALE) {
            color = WW_UI_WARNING;
            opacity = LV_OPA_20;
        } else if (signal == WEREWOLF_UI_SIGNAL_DISCONNECTED) {
            color = WW_UI_ERROR;
            opacity = LV_OPA_20;
        }
        header_box_style(s_header_signal_segments[i], color, opacity);
    }
}

static void update_header_players(void)
{
    for (unsigned i = 0U; i < CELL_COUNT; ++i) {
        const werewolf_ui_player_t *player = &s_model.players[i];
        werewolf_ui_player_indicator_t indicator =
            werewolf_ui_player_indicator(player->occupied, player->ready,
                                          s_model.game_started,
                                          player->publicly_alive);
        uint32_t fill_color = WW_UI_STRUCTURE;
        lv_opa_t fill_opacity = LV_OPA_TRANSP;
        int fill_y = 2;
        int fill_height = 8;
        bool cut = false;

        header_box_style(s_header_player_slots[i], WW_UI_STRUCTURE,
                         indicator == WEREWOLF_UI_PLAYER_INDICATOR_EMPTY
                             ? LV_OPA_20
                             : LV_OPA_40);
        switch (indicator) {
        case WEREWOLF_UI_PLAYER_INDICATOR_JOINED:
            fill_color = WW_UI_WARNING;
            fill_opacity = LV_OPA_COVER;
            fill_y = 8;
            fill_height = 2;
            break;
        case WEREWOLF_UI_PLAYER_INDICATOR_READY:
        case WEREWOLF_UI_PLAYER_INDICATOR_ALIVE:
            fill_color = WW_UI_SUCCESS;
            fill_opacity = LV_OPA_COVER;
            break;
        case WEREWOLF_UI_PLAYER_INDICATOR_DEAD:
            fill_color = WW_UI_ERROR;
            fill_opacity = LV_OPA_COVER;
            cut = true;
            break;
        case WEREWOLF_UI_PLAYER_INDICATOR_EMPTY:
        default:
            break;
        }
        lv_obj_set_pos(s_header_player_fills[i], 2, fill_y);
        lv_obj_set_size(s_header_player_fills[i], 6, fill_height);
        header_box_style(s_header_player_fills[i], fill_color, fill_opacity);
        header_box_style(s_header_player_cuts[i], WW_UI_BG,
                         cut ? LV_OPA_COVER : LV_OPA_TRANSP);

        bool local = s_model.local_seat < WEREWOLF_UI_PLAYER_COUNT &&
                     player->seat == s_model.local_seat;
        lv_obj_set_style_border_color(
            s_header_player_slots[i],
            lv_color_hex(local ? WW_UI_TEXT : WW_UI_STRUCTURE), 0);
        lv_obj_set_style_border_opa(s_header_player_slots[i],
                                    local ? LV_OPA_COVER : LV_OPA_70, 0);
    }
}

static void update_header(void)
{
    char phase[WEREWOLF_UI_PHASE_TEXT_MAX];

    if (!werewolf_ui_format_phase(phase, sizeof(phase),
                                  s_model.public_phase, s_model.round)) {
        (void)snprintf(phase, sizeof(phase), "STATUS ERR");
    }
    lv_label_set_text(s_header_phase, phase);
    lv_obj_set_style_text_color(
        s_header_phase,
        lv_color_hex(s_model.public_phase == WEREWOLF_UI_PUBLIC_PHASE_ERROR
                         ? WW_UI_ERROR
                         : WW_UI_TEXT),
        0);
    update_header_connection();
    update_header_signal();
    update_header_players();

    unsigned filled = werewolf_ui_battery_filled_segments(
        s_model.battery_state, s_model.battery_soc);
    bool available = s_model.battery_state !=
                         WEREWOLF_UI_BATTERY_UNAVAILABLE &&
                     s_model.battery_soc <= 100U;
    uint32_t battery_color = WW_UI_MUTED;
    if (available) {
        if (s_model.battery_soc <
            WEREWOLF_UI_BATTERY_CRITICAL_PERCENT) {
            battery_color = WW_UI_ERROR;
        } else if (s_model.battery_state == WEREWOLF_UI_BATTERY_STALE ||
                   s_model.battery_soc < WEREWOLF_UI_BATTERY_LOW_PERCENT) {
            battery_color = WW_UI_WARNING;
        } else {
            battery_color = WW_UI_SUCCESS;
        }
    }
    for (unsigned i = 0U; i < WEREWOLF_UI_BATTERY_SEGMENTS; ++i) {
        bool on = available && i < filled;
        lv_obj_set_style_bg_color(
            s_header_battery_segments[i],
            lv_color_hex(on ? battery_color
                            : (available ? WW_UI_STRUCTURE : WW_UI_MUTED)), 0);
        lv_obj_set_style_bg_opa(s_header_battery_segments[i],
                                on ? LV_OPA_COVER : LV_OPA_40, 0);
    }
}

static void hide_obj(lv_obj_t *obj)
{
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void show_obj(lv_obj_t *obj)
{
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *cell_create(lv_obj_t *parent)
{
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(cell, 4, 0);
    lv_obj_set_style_border_width(cell, 1, 0);
    lv_obj_set_style_border_color(cell, lv_color_hex(WW_UI_STRUCTURE), 0);
    lv_obj_set_style_border_opa(cell, LV_OPA_70, 0);
    lv_obj_set_style_border_side(cell, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_all(cell, 0, 0);
    lv_obj_set_style_bg_color(cell, lv_color_hex(WW_UI_PANEL_ALT), 0);
    lv_obj_set_style_bg_opa(cell, LV_OPA_50, 0);
    return cell;
}

static void label_place(lv_obj_t *label, const char *text,
                        int x, int y, int w, int h,
                        const lv_font_t *font, uint32_t color,
                        lv_text_align_t align)
{
    show_obj(label);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, w, h);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text);
}

static void cell_place(unsigned index, int x, int y, int w, int h,
                       const char *text, bool selected, bool enabled,
                       bool private_palette)
{
    if (index >= CELL_COUNT) {
        return;
    }

    lv_obj_t *cell = s_cells[index];
    lv_obj_t *label = s_cell_labels[index];
    uint32_t bg = WW_UI_PANEL_ALT;
    uint32_t fg = WW_UI_TEXT;
    uint32_t border = WW_UI_STRUCTURE;
    uint32_t bar = WW_UI_TEXT;
    lv_opa_t bg_opacity = LV_OPA_50;

    /* Private and public pages deliberately share the same geometry and
     * luminance. Only semantic state colors change. */
    (void)private_palette;

    show_obj(label);
    hide_obj(s_lobby_seat_labels[index]);
    hide_obj(s_lobby_identity_badges[index]);
    hide_obj(s_lobby_name_labels[index]);
    hide_obj(s_lobby_state_labels[index]);

    if (!enabled) {
        bg = WW_UI_BG;
        fg = WW_UI_MUTED;
        bg_opacity = LV_OPA_TRANSP;
    } else if (selected) {
        bg = WW_UI_FOCUS;
        fg = WW_UI_FOCUS_TEXT;
        border = WW_UI_FOCUS;
        bg_opacity = LV_OPA_COVER;
    }

    show_obj(cell);
    lv_obj_set_pos(cell, x, y);
    lv_obj_set_size(cell, w, h);
    lv_obj_set_style_radius(cell, h >= 36 ? 6 : 3, 0);
    lv_obj_set_style_bg_color(cell, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(cell, bg_opacity, 0);
    lv_obj_set_style_border_width(cell, 1, 0);
    lv_obj_set_style_border_color(cell, lv_color_hex(border), 0);
    lv_obj_set_style_border_opa(cell,
                                selected ? LV_OPA_COVER : LV_OPA_70, 0);
    lv_obj_set_style_border_side(
        cell, selected ? LV_BORDER_SIDE_FULL : LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_pos(s_cell_bars[index], 0, 3);
    lv_obj_set_size(s_cell_bars[index], 3, h - 6);
    lv_obj_set_style_radius(s_cell_bars[index], 2, 0);
    lv_obj_set_style_bg_color(s_cell_bars[index], lv_color_hex(bar), 0);
    lv_obj_set_style_bg_opa(s_cell_bars[index],
                            selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label,
                               h >= 40 ? &ui_font_kode_bold_15
                                       : &ui_font_kode_regular_13,
                               0);
    lv_obj_set_style_text_color(label, lv_color_hex(fg), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(label, w - 24);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 14, 0);
}

static void lobby_label_set(lv_obj_t *label, const char *text,
                            int x, int y, int width,
                            const lv_font_t *font, uint32_t color,
                            lv_text_align_t align)
{
    show_obj(label);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, 18);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(label, text);
}

static void lobby_cell_place(unsigned index, int y,
                             const werewolf_ui_player_t *player,
                             bool focused)
{
    char seat[sizeof("S255")];
    const char *identity;
    const char *name;
    const char *state;
    uint32_t state_color;
    lv_obj_t *cell;

    if (index >= CELL_COUNT || player == NULL) {
        return;
    }
    cell = s_cells[index];
    name = player->occupied ? text_or(player->name, "PLAYER") : "-- EMPTY --";
    if (!player->occupied) {
        state = "";
        state_color = WW_UI_MUTED;
    } else if (player->ready) {
        state = "READY";
        state_color = WW_UI_SUCCESS;
    } else {
        state = "WAIT";
        state_color = WW_UI_MUTED;
    }

    show_obj(cell);
    hide_obj(s_cell_labels[index]);
    lv_obj_set_pos(cell, 0, y);
    lv_obj_set_size(cell, 202, 20);
    lv_obj_set_style_radius(cell, focused ? 4 : 0, 0);
    lv_obj_set_style_bg_color(
        cell, lv_color_hex(focused ? WW_UI_FOCUS : WW_UI_PANEL_ALT), 0);
    lv_obj_set_style_bg_opa(
        cell, focused ? LV_OPA_COVER
                      : (player->occupied ? LV_OPA_40 : LV_OPA_TRANSP), 0);
    lv_obj_set_style_border_width(cell, 1, 0);
    lv_obj_set_style_border_color(
        cell, lv_color_hex(focused ? WW_UI_FOCUS : WW_UI_STRUCTURE), 0);
    lv_obj_set_style_border_opa(
        cell, focused ? LV_OPA_COVER : LV_OPA_70, 0);
    lv_obj_set_style_border_side(
        cell, focused ? LV_BORDER_SIDE_FULL : LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_pos(s_cell_bars[index], 0, 3);
    lv_obj_set_size(s_cell_bars[index], 3, 14);
    lv_obj_set_style_radius(s_cell_bars[index], 2, 0);
    lv_obj_set_style_bg_color(
        s_cell_bars[index],
        lv_color_hex(WW_UI_TEXT), 0);
    lv_obj_set_style_bg_opa(
        s_cell_bars[index], focused ? LV_OPA_COVER : LV_OPA_TRANSP, 0);

    (void)snprintf(seat, sizeof(seat), "S%u", seat_number(player->seat));
    lobby_label_set(s_lobby_seat_labels[index], seat, 12, 1, 20,
                    &ui_font_kode_regular_11,
                    focused ? WW_UI_FOCUS_TEXT : WW_UI_MUTED,
                    LV_TEXT_ALIGN_LEFT);

    if (player->occupied) {
        identity = player->seat == 0U
                       ? "H"
                       : (player->seat == s_model.local_seat ? "Y" : "G");
        show_obj(s_lobby_identity_badges[index]);
        lv_obj_set_pos(s_lobby_identity_badges[index], 35, 3);
        lv_obj_set_style_bg_color(s_lobby_identity_badges[index],
                                  lv_color_hex(WW_UI_BG), 0);
        lv_obj_set_style_bg_opa(s_lobby_identity_badges[index],
                                LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s_lobby_identity_badges[index],
                                      lv_color_hex(focused ? WW_UI_FOCUS_TEXT
                                                           : WW_UI_STRUCTURE),
                                      0);
        lv_obj_set_style_text_color(s_lobby_identity_labels[index],
                                    lv_color_hex(WW_UI_TEXT), 0);
        lv_label_set_text(s_lobby_identity_labels[index], identity);
    } else {
        hide_obj(s_lobby_identity_badges[index]);
    }

    lobby_label_set(s_lobby_name_labels[index], name, 54, 1, 80,
                    &ui_font_kode_regular_13,
                    focused ? WW_UI_FOCUS_TEXT
                            : (player->occupied ? WW_UI_TEXT : WW_UI_MUTED),
                    LV_TEXT_ALIGN_LEFT);
    lobby_label_set(s_lobby_state_labels[index], state, 138, 2, 56,
                    &ui_font_kode_regular_11,
                    focused ? WW_UI_FOCUS_TEXT : state_color,
                    LV_TEXT_ALIGN_RIGHT);
}

static void footer_set(const char *text, uint32_t color)
{
    lv_label_set_text(s_footer, text);
    lv_obj_set_style_text_color(s_footer, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(s_footer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_footer, 224);
    lv_obj_align(s_footer, LV_ALIGN_BOTTOM_MID, 0, -5);
}

static bool room_modal_active(void)
{
    return s_model.page == WEREWOLF_UI_PAGE_ROOM_CLOSED ||
           s_model.room_closing || s_model.leaving_room ||
           s_model.player_kicking ||
           (s_model.page == WEREWOLF_UI_PAGE_LOBBY && s_model.is_host &&
            s_model.room_close_prompt);
}

static void modal_option_set(unsigned index, int y, int height,
                             const char *text, bool selected,
                             bool dangerous)
{
    uint32_t fill = selected ? (dangerous ? WW_UI_ERROR : WW_UI_FOCUS)
                             : WW_UI_PANEL_ALT;
    uint32_t border = selected ? fill : WW_UI_STRUCTURE;
    uint32_t foreground = selected
                              ? (dangerous ? WW_UI_TEXT : WW_UI_FOCUS_TEXT)
                              : (dangerous ? WW_UI_ERROR : WW_UI_TEXT);

    if (index >= 2U) {
        return;
    }
    show_obj(s_modal_options[index]);
    lv_obj_set_pos(s_modal_options[index], 12, y);
    lv_obj_set_size(s_modal_options[index], 176, height);
    lv_obj_set_style_radius(s_modal_options[index], 5, 0);
    lv_obj_set_style_bg_color(s_modal_options[index],
                              lv_color_hex(fill), 0);
    lv_obj_set_style_bg_opa(s_modal_options[index],
                            selected ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_modal_options[index], 1, 0);
    lv_obj_set_style_border_color(s_modal_options[index],
                                  lv_color_hex(border), 0);
    lv_obj_set_style_border_opa(s_modal_options[index],
                                selected ? LV_OPA_COVER : LV_OPA_70, 0);
    lv_obj_set_pos(s_modal_option_bars[index], 0, 3);
    lv_obj_set_size(s_modal_option_bars[index], 3, height - 6);
    lv_obj_set_style_bg_color(s_modal_option_bars[index],
                              lv_color_hex(WW_UI_TEXT), 0);
    lv_obj_set_style_bg_opa(s_modal_option_bars[index],
                            selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    label_place(s_modal_option_labels[index], text, 13, 5,
                152, height - 6, &ui_font_kode_bold_13,
                foreground, LV_TEXT_ALIGN_LEFT);
}

static void render_room_modal(void)
{
    char status[48];
    const werewolf_ui_player_t *player;

    if (!room_modal_active()) {
        hide_obj(s_modal_overlay);
        return;
    }

    show_obj(s_modal_overlay);
    lv_obj_move_foreground(s_modal_overlay);
    hide_obj(s_modal_options[0]);
    hide_obj(s_modal_options[1]);
    lv_obj_set_style_bg_color(s_modal_dialog,
                              lv_color_hex(WW_UI_ERROR_PANEL), 0);
    lv_obj_set_style_border_color(s_modal_dialog,
                                  lv_color_hex(WW_UI_STRUCTURE), 0);
    lv_obj_set_style_bg_color(s_modal_rail, lv_color_hex(WW_UI_ERROR), 0);

    if (s_model.leaving_room) {
        lv_obj_set_style_bg_color(s_modal_rail,
                                  lv_color_hex(WW_UI_WARNING), 0);
        label_place(s_modal_title, "LEAVING ROOM", 12, 30, 176, 28,
                    &ui_font_kode_bold_21, WW_UI_WARNING,
                    LV_TEXT_ALIGN_CENTER);
        label_place(s_modal_status, "NOTIFYING HOST", 12, 62, 176, 18,
                    &ui_font_kode_bold_13, WW_UI_WARNING,
                    LV_TEXT_ALIGN_CENTER);
        label_place(s_modal_body, "ENDING SECURE LINK.\nPLEASE WAIT.",
                    16, 98, 168, 44, &ui_font_kode_regular_13,
                    WW_UI_TEXT, LV_TEXT_ALIGN_CENTER);
        footer_set("LEAVING ROOM  |  INPUT LOCKED", WW_UI_MUTED);
        return;
    }

    if (s_model.room_closing) {
        label_place(s_modal_title, "CLOSING ROOM", 12, 30, 176, 28,
                    &ui_font_kode_bold_21, WW_UI_ERROR,
                    LV_TEXT_ALIGN_CENTER);
        label_place(s_modal_status, "NOTIFYING GUESTS", 12, 62, 176, 18,
                    &ui_font_kode_bold_13, WW_UI_WARNING,
                    LV_TEXT_ALIGN_CENTER);
        label_place(s_modal_body, "ENDING SECURE LINKS.\nPLEASE WAIT.",
                    16, 98, 168, 44, &ui_font_kode_regular_13,
                    WW_UI_TEXT, LV_TEXT_ALIGN_CENTER);
        footer_set("CLOSING ROOM  |  INPUT LOCKED", WW_UI_MUTED);
        return;
    }

    if (s_model.player_kicking) {
        player = player_by_seat(s_model.kick_seat);
        if (player != NULL) {
            (void)snprintf(status, sizeof(status), "S%u  %.10s",
                           seat_number(player->seat),
                           text_or(player->name, "PLAYER"));
        } else if (s_model.kick_seat < WEREWOLF_UI_PLAYER_COUNT) {
            (void)snprintf(status, sizeof(status), "S%u",
                           seat_number(s_model.kick_seat));
        } else {
            (void)snprintf(status, sizeof(status), "PLAYER UNAVAILABLE");
        }
        label_place(s_modal_title, "REMOVING PLAYER", 12, 30, 176, 28,
                    &ui_font_kode_bold_21, WW_UI_ERROR,
                    LV_TEXT_ALIGN_CENTER);
        label_place(s_modal_status, status, 12, 62, 176, 18,
                    &ui_font_kode_bold_13, WW_UI_WARNING,
                    LV_TEXT_ALIGN_CENTER);
        label_place(s_modal_body, "WAITING FOR ACK", 16, 102, 168, 30,
                    &ui_font_kode_regular_13, WW_UI_TEXT,
                    LV_TEXT_ALIGN_CENTER);
        footer_set("REMOVING PLAYER  |  INPUT LOCKED", WW_UI_MUTED);
        return;
    }

    if (s_model.page == WEREWOLF_UI_PAGE_ROOM_CLOSED) {
        const char *closed_title = text_or(s_model.headline, "ROOM CLOSED");
        const char *closed_status =
            strcmp(closed_title, "REMOVED FROM ROOM") == 0
                ? "REMOVED BY HOST"
                : "CLOSED BY HOST";

        label_place(s_modal_title,
                    closed_title,
                    12, 22, 176, 28,
                    strlen(closed_title) > 13U ? &ui_font_kode_bold_15
                                               : &ui_font_kode_bold_21,
                    WW_UI_ERROR,
                    LV_TEXT_ALIGN_CENTER);
        label_place(s_modal_status, closed_status, 12, 54, 176, 18,
                    &ui_font_kode_bold_13, WW_UI_WARNING,
                    LV_TEXT_ALIGN_CENTER);
        label_place(s_modal_body,
                    text_or(s_model.detail,
                            "YOU WERE REMOVED\nFROM THIS ROOM."),
                    16, 84, 168, 42, &ui_font_kode_regular_13,
                    WW_UI_TEXT, LV_TEXT_ALIGN_CENTER);
        modal_option_set(0U, 144, 38, "OK", true, false);
        footer_set("OK RETURN TO MENU", WW_UI_TEXT);
        return;
    }

    label_place(s_modal_title, "CLOSE ROOM?", 12, 22, 176, 28,
                &ui_font_kode_bold_21, WW_UI_ERROR, LV_TEXT_ALIGN_CENTER);
    label_place(s_modal_status, "HOST ACTION", 12, 51, 176, 18,
                &ui_font_kode_bold_13, WW_UI_WARNING, LV_TEXT_ALIGN_CENTER);
    label_place(s_modal_body, "ALL PLAYERS WILL\nBE REMOVED.",
                16, 76, 168, 38, &ui_font_kode_regular_13,
                WW_UI_TEXT, LV_TEXT_ALIGN_CENTER);
    modal_option_set(0U, 120, 30, "BACK",
                     !s_close_focus_close, false);
    modal_option_set(1U, 158, 30, "CLOSE ROOM",
                     s_close_focus_close, true);
    footer_set(s_close_focus_close
                   ? "HOLD OK CLOSE  HOLD DN BACK"
                   : "UP/DN  OK BACK  HOLD DN",
               WW_UI_TEXT);
}

static void update_moon_motif(uint32_t panel_color)
{
    int disc_x = 148;
    int disc_y = -42;
    int disc_size = 100;
    int cut_x = 124;
    int cut_y = -50;
    int cut_size = 92;
    int horizon_x = 132;
    int horizon_y = 45;
    int horizon_width = 88;
    uint32_t disc_color = WW_UI_FOCUS;
    lv_opa_t disc_opacity = LV_OPA_20;
    lv_opa_t cut_opacity = LV_OPA_COVER;
    lv_opa_t horizon_opacity = LV_OPA_40;

    switch (s_model.page) {
    case WEREWOLF_UI_PAGE_ROLE:
    case WEREWOLF_UI_PAGE_PRIVATE_RESULT:
        disc_x = 51;
        disc_y = 43;
        disc_size = 112;
        cut_x = 39;
        cut_y = 31;
        cut_size = 104;
        disc_opacity = s_private_revealed ? LV_OPA_30 : LV_OPA_20;
        cut_opacity = s_private_revealed ? LV_OPA_TRANSP : LV_OPA_COVER;
        horizon_opacity = LV_OPA_TRANSP;
        break;
    case WEREWOLF_UI_PAGE_DAY_RESULT:
    case WEREWOLF_UI_PAGE_SPEAKING:
        disc_x = 136;
        disc_y = 150;
        disc_size = 110;
        cut_opacity = LV_OPA_TRANSP;
        horizon_x = 102;
        horizon_y = 184;
        horizon_width = 116;
        disc_color = WW_UI_WARNING;
        disc_opacity = LV_OPA_20;
        break;
    case WEREWOLF_UI_PAGE_ELIMINATED:
        disc_x = 51;
        disc_y = 45;
        disc_size = 112;
        cut_opacity = LV_OPA_TRANSP;
        horizon_x = 39;
        horizon_y = 100;
        horizon_width = 136;
        disc_color = WW_UI_ERROR;
        disc_opacity = LV_OPA_30;
        horizon_opacity = LV_OPA_70;
        break;
    case WEREWOLF_UI_PAGE_GAME_OVER:
        disc_x = 170;
        disc_y = -42;
        disc_size = 104;
        cut_opacity = LV_OPA_TRANSP;
        disc_opacity = LV_OPA_20;
        horizon_x = 148;
        horizon_y = 39;
        horizon_width = 74;
        break;
    case WEREWOLF_UI_PAGE_ERROR:
    case WEREWOLF_UI_PAGE_ROOM_CLOSED:
        disc_x = 51;
        disc_y = 45;
        disc_size = 112;
        cut_opacity = LV_OPA_TRANSP;
        disc_color = WW_UI_ERROR;
        disc_opacity = LV_OPA_20;
        horizon_opacity = LV_OPA_TRANSP;
        break;
    default:
        break;
    }

    lv_obj_set_pos(s_moon_disc, disc_x, disc_y);
    lv_obj_set_size(s_moon_disc, disc_size, disc_size);
    lv_obj_set_style_radius(s_moon_disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_moon_disc, lv_color_hex(disc_color), 0);
    lv_obj_set_style_bg_opa(s_moon_disc, disc_opacity, 0);

    lv_obj_set_pos(s_moon_cutout, cut_x, cut_y);
    lv_obj_set_size(s_moon_cutout, cut_size, cut_size);
    lv_obj_set_style_radius(s_moon_cutout, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_moon_cutout, lv_color_hex(panel_color), 0);
    lv_obj_set_style_bg_opa(s_moon_cutout, cut_opacity, 0);

    lv_obj_set_pos(s_moon_horizon, horizon_x, horizon_y);
    lv_obj_set_size(s_moon_horizon, horizon_width, 1);
    lv_obj_set_style_bg_color(s_moon_horizon,
                              lv_color_hex(disc_color), 0);
    lv_obj_set_style_bg_opa(s_moon_horizon, horizon_opacity, 0);
}

static void render_reset(uint32_t panel_color, uint32_t text_color)
{
    update_header();
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(panel_color), 0);
    update_moon_motif(panel_color);
    hide_obj(s_title);
    hide_obj(s_subtitle);
    hide_obj(s_detail);
    hide_obj(s_timer);
    hide_obj(s_banner);
    hide_obj(s_modal_overlay);

    /* Clearing hidden text is intentional: a sealed private screen must not
     * retain the previous secret in an invisible LVGL label. */
    lv_label_set_text(s_title, "");
    lv_label_set_text(s_subtitle, "");
    lv_label_set_text(s_detail, "");
    lv_label_set_text(s_timer, "");
    lv_obj_set_style_text_color(s_title, lv_color_hex(text_color), 0);
    lv_obj_set_style_text_color(s_subtitle, lv_color_hex(text_color), 0);
    lv_obj_set_style_text_color(s_detail, lv_color_hex(text_color), 0);
    lv_obj_set_style_text_color(s_timer, lv_color_hex(text_color), 0);
    for (unsigned i = 0; i < CELL_COUNT; ++i) {
        lv_label_set_text(s_cell_labels[i], "");
        hide_obj(s_cells[i]);
    }
    footer_set("UP/DN MOVE  |  OK SELECT", WW_UI_TEXT);
}

static const werewolf_ui_player_t *player_by_seat(uint8_t seat)
{
    for (unsigned i = 0; i < WEREWOLF_UI_PLAYER_COUNT; ++i) {
        if (s_model.players[i].occupied && s_model.players[i].seat == seat) {
            return &s_model.players[i];
        }
    }
    return NULL;
}

static bool lobby_guest_is_manageable(uint8_t seat)
{
    const werewolf_ui_player_t *player = player_by_seat(seat);

    return s_model.is_host && seat > 0U &&
           seat < WEREWOLF_UI_PLAYER_COUNT && player != NULL;
}

static bool lobby_seat_is_selectable(uint8_t seat)
{
    return s_model.is_host && seat < WEREWOLF_UI_PLAYER_COUNT &&
           player_by_seat(seat) != NULL;
}

static uint8_t lobby_first_seat(void)
{
    for (uint8_t seat = 0U; seat < WEREWOLF_UI_PLAYER_COUNT; ++seat) {
        if (lobby_seat_is_selectable(seat)) {
            return seat;
        }
    }
    return WEREWOLF_UI_NO_SEAT;
}

static void normalize_lobby_cursor(bool reset)
{
    if (!s_model.is_host) {
        s_lobby_cursor = WEREWOLF_UI_NO_SEAT;
        return;
    }
    if (reset && lobby_seat_is_selectable(s_model.selected_seat)) {
        s_lobby_cursor = s_model.selected_seat;
    }
    if (!lobby_seat_is_selectable(s_lobby_cursor)) {
        /* A departed kick target always returns focus to the Host, never to a
         * different guest. */
        s_lobby_cursor = lobby_seat_is_selectable(s_model.local_seat)
                             ? s_model.local_seat
                             : lobby_first_seat();
    }
}

static void move_lobby_cursor(int direction)
{
    if (!lobby_seat_is_selectable(s_lobby_cursor)) {
        normalize_lobby_cursor(false);
        return;
    }

    for (unsigned step = 1U; step < WEREWOLF_UI_PLAYER_COUNT; ++step) {
        int seat = (int)s_lobby_cursor + direction * (int)step;
        while (seat < 0) {
            seat += WEREWOLF_UI_PLAYER_COUNT;
        }
        while (seat >= WEREWOLF_UI_PLAYER_COUNT) {
            seat -= WEREWOLF_UI_PLAYER_COUNT;
        }
        if (lobby_seat_is_selectable((uint8_t)seat)) {
            s_lobby_cursor = (uint8_t)seat;
            return;
        }
    }
}

static bool seat_is_eligible(uint8_t seat)
{
    const werewolf_ui_player_t *player = player_by_seat(seat);
    return player && player->alive && player->eligible;
}

static uint8_t first_eligible_seat(void)
{
    for (unsigned i = 0; i < WEREWOLF_UI_PLAYER_COUNT; ++i) {
        const werewolf_ui_player_t *player = &s_model.players[i];
        if (player->occupied && player->alive && player->eligible) {
            return player->seat;
        }
    }
    return WEREWOLF_UI_NO_SEAT;
}

static void normalize_target_cursor(bool reset)
{
    if (reset && seat_is_eligible(s_model.selected_seat)) {
        s_target_cursor = s_model.selected_seat;
    }
    if (!seat_is_eligible(s_target_cursor)) {
        s_target_cursor = first_eligible_seat();
    }
}

static void move_target_cursor(int direction)
{
    int current = -1;
    for (unsigned i = 0; i < WEREWOLF_UI_PLAYER_COUNT; ++i) {
        if (s_model.players[i].occupied &&
            s_model.players[i].seat == s_target_cursor) {
            current = (int)i;
            break;
        }
    }

    for (unsigned step = 1; step <= WEREWOLF_UI_PLAYER_COUNT; ++step) {
        int index = current + direction * (int)step;
        while (index < 0) {
            index += WEREWOLF_UI_PLAYER_COUNT;
        }
        index %= WEREWOLF_UI_PLAYER_COUNT;
        const werewolf_ui_player_t *player = &s_model.players[index];
        if (player->occupied && player->alive && player->eligible) {
            s_target_cursor = player->seat;
            return;
        }
    }
}

static unsigned occupied_count(void)
{
    unsigned count = 0;
    for (unsigned i = 0; i < WEREWOLF_UI_PLAYER_COUNT; ++i) {
        count += s_model.players[i].occupied ? 1U : 0U;
    }
    return count;
}

static bool local_player_alive(void)
{
    const werewolf_ui_player_t *player = player_by_seat(s_model.local_seat);
    return player && player->alive;
}

static bool lobby_can_start(void)
{
    return s_model.is_host && s_model.can_start &&
           occupied_count() == WEREWOLF_UI_PLAYER_COUNT;
}

static const werewolf_ui_room_t *room_by_token(uint32_t token)
{
    if (token == 0U) {
        return NULL;
    }
    for (unsigned i = 0U; i < WEREWOLF_UI_ROOM_COUNT_MAX; ++i) {
        if (s_model.rooms[i].visible && s_model.rooms[i].token == token) {
            return &s_model.rooms[i];
        }
    }
    return NULL;
}

static uint32_t first_room_token(void)
{
    for (unsigned i = 0U; i < WEREWOLF_UI_ROOM_COUNT_MAX; ++i) {
        if (s_model.rooms[i].visible) {
            return s_model.rooms[i].token;
        }
    }
    return 0U;
}

static void normalize_room_cursor(bool reset)
{
    if (reset && room_by_token(s_model.selected_room_token) != NULL) {
        s_room_cursor_token = s_model.selected_room_token;
    }
    if (room_by_token(s_room_cursor_token) == NULL) {
        s_room_cursor_token = first_room_token();
    }
}

static void move_room_cursor(int direction)
{
    int current = -1;
    int visible_count = 0;
    unsigned visible_indices[WEREWOLF_UI_ROOM_COUNT_MAX];

    for (unsigned i = 0U; i < WEREWOLF_UI_ROOM_COUNT_MAX; ++i) {
        if (!s_model.rooms[i].visible) {
            continue;
        }
        if (s_model.rooms[i].token == s_room_cursor_token) {
            current = visible_count;
        }
        visible_indices[visible_count++] = i;
    }
    if (visible_count == 0) {
        s_room_cursor_token = 0U;
        return;
    }
    if (current < 0) {
        current = 0;
    } else {
        current = (current + direction + visible_count) % visible_count;
    }
    s_room_cursor_token = s_model.rooms[visible_indices[current]].token;
}

static void render_mode(void)
{
    char subtitle[40];

    render_reset(WW_UI_PANEL, WW_UI_TEXT);
    label_place(s_title, "THE VILLAGE", 10, 7, 182, 26,
                &ui_font_kode_bold_21, WW_UI_TEXT, LV_TEXT_ALIGN_LEFT);
    (void)snprintf(subtitle, sizeof(subtitle), "PLAYER  %.10s",
                   text_or(s_model.local_name, "UNNAMED"));
    label_place(s_subtitle, subtitle, 10, 36, 182, 20,
                &ui_font_kode_regular_13, WW_UI_SUCCESS, LV_TEXT_ALIGN_LEFT);
    cell_place(0, 10, 68, 182, 50, "CREATE ROOM",
               s_mode_cursor == WEREWOLF_UI_MODE_CREATE, true, false);
    cell_place(1, 10, 130, 182, 50, "JOIN ROOM",
               s_mode_cursor == WEREWOLF_UI_MODE_JOIN, true, false);
    label_place(s_detail, "7 DEVICES  OFFLINE PLAY", 10, 202, 182, 18,
                &ui_font_kode_regular_11, WW_UI_MUTED, LV_TEXT_ALIGN_LEFT);
    footer_set("UP/DN MODE  |  OK SELECT", WW_UI_TEXT);
}

static void render_room_list(void)
{
    char line[48];
    unsigned visible_indices[WEREWOLF_UI_ROOM_COUNT_MAX];
    unsigned visible_count = 0U;
    unsigned selected = 0U;

    render_reset(WW_UI_PANEL, WW_UI_TEXT);
    normalize_room_cursor(false);
    for (unsigned i = 0U; i < WEREWOLF_UI_ROOM_COUNT_MAX; ++i) {
        if (!s_model.rooms[i].visible) {
            continue;
        }
        if (s_model.rooms[i].token == s_room_cursor_token) {
            selected = visible_count;
        }
        visible_indices[visible_count++] = i;
    }

    label_place(s_title, "OPEN ROOMS", 10, 4, 182, 25,
                &ui_font_kode_bold_21, WW_UI_TEXT, LV_TEXT_ALIGN_LEFT);
    if (visible_count == 0U) {
        label_place(s_subtitle, "NO ROOMS YET", 10, 31, 182, 18,
                    &ui_font_kode_bold_13, WW_UI_WARNING,
                    LV_TEXT_ALIGN_LEFT);
        label_place(s_detail, "STILL SCANNING", 12, 100, 178, 40,
                    &ui_font_kode_regular_13, WW_UI_TEXT,
                    LV_TEXT_ALIGN_CENTER);
    } else {
        unsigned window = (selected / 4U) * 4U;
        (void)snprintf(line, sizeof(line), "%u ROOM%s  CHOOSE HOST",
                       visible_count, visible_count == 1U ? "" : "S");
        label_place(s_subtitle, line, 10, 31, 182, 18,
                    &ui_font_kode_regular_13, WW_UI_SUCCESS,
                    LV_TEXT_ALIGN_LEFT);
        for (unsigned row = 0U; row < 4U && window + row < visible_count;
             ++row) {
            const werewolf_ui_room_t *room =
                &s_model.rooms[visible_indices[window + row]];
            (void)snprintf(line, sizeof(line), "%-8.8s  %u/7",
                           text_or(room->code, "ROOM"),
                           (unsigned)room->occupied_count);
            cell_place(row, 10, 52 + (int)row * 43, 182, 36, line,
                       room->token == s_room_cursor_token, true, false);
        }
    }
    footer_set("UP/DN  OK JOIN  HOLD DN BACK", WW_UI_TEXT);
}

static void render_lobby(void)
{
    char line[64];
    char title[40];
    char verify[WEREWOLF_UI_VERIFY_TEXT_MAX];
    bool has_verify = s_model.has_verify_code &&
                      werewolf_ui_format_verify_code(
                          verify, sizeof(verify), s_model.verify_code);
    const werewolf_ui_player_t *focused =
        player_by_seat(s_lobby_cursor);
    render_reset(WW_UI_PANEL, WW_UI_TEXT);

    snprintf(title, sizeof(title), "ROOM %s",
             text_or(s_model.room_code, "------"));
    label_place(s_title, title, 8, 0, 194, 25,
                &ui_font_kode_bold_21, WW_UI_TEXT, LV_TEXT_ALIGN_LEFT);
    if (!s_model.is_host && has_verify) {
        label_place(s_subtitle, verify, 8, 25, 194, 18,
                    &ui_font_kode_bold_13, WW_UI_WARNING,
                    LV_TEXT_ALIGN_LEFT);
    } else if (lobby_can_start()) {
        label_place(s_subtitle, "ALL READY  HOLD OK START",
                    8, 25, 194, 18, &ui_font_kode_bold_13,
                    WW_UI_SUCCESS, LV_TEXT_ALIGN_LEFT);
    } else {
        snprintf(line, sizeof(line), "%u/7 SEATS  %s",
                 occupied_count(), s_model.is_host ? "HOST" : "GUEST");
        label_place(s_subtitle, line, 8, 25, 194, 18,
                    &ui_font_kode_regular_13, WW_UI_SUCCESS,
                    LV_TEXT_ALIGN_LEFT);
    }

    for (unsigned i = 0; i < WEREWOLF_UI_PLAYER_COUNT; ++i) {
        const werewolf_ui_player_t *player = &s_model.players[i];
        lobby_cell_place(i, 44 + (int)i * 22, player,
                         s_model.is_host &&
                             player->seat == s_lobby_cursor);
    }

    if (!s_model.is_host &&
        (s_model.connection == WEREWOLF_UI_CONNECTION_SCANNING ||
         s_model.connection == WEREWOLF_UI_CONNECTION_PAIRING)) {
        footer_set("SEARCHING  HOLD DN CANCEL", WW_UI_TEXT);
    } else if (s_action_latched) {
        footer_set("ACTION SENT  WAIT", WW_UI_TEXT);
    } else if (!s_model.input_enabled) {
        footer_set(s_model.is_host
                       ? "WAIT FOR LINK  HOLD DN EXIT"
                       : "SYNCING NAME  HOLD DN LEAVE",
                   WW_UI_TEXT);
    } else if (!s_model.is_host) {
        footer_set(s_model.local_ready
                       ? "OK WAIT  HOLD DN LEAVE"
                       : "OK READY  HOLD DN LEAVE",
                   WW_UI_TEXT);
    } else if (lobby_can_start()) {
        footer_set(focused != NULL &&
                           focused->seat == s_model.local_seat
                       ? "OK WAIT  HOLD OK START"
                       : "OK VIEW  HOLD OK START",
                   WW_UI_TEXT);
    } else if (focused != NULL && focused->seat != s_model.local_seat) {
        footer_set("UP/DN  OK VIEW  HOLD DN EXIT", WW_UI_TEXT);
    } else {
        footer_set(s_model.local_ready
                       ? "UP/DN  OK WAIT  HOLD DN EXIT"
                       : "UP/DN  OK READY  HOLD DN EXIT",
                   WW_UI_TEXT);
    }
}

static void render_player_action(void)
{
    char title[40];
    char status[40];
    char verify[WEREWOLF_UI_VERIFY_TEXT_MAX];
    const werewolf_ui_player_t *player =
        player_by_seat(s_model.selected_seat);
    bool can_kick = lobby_guest_is_manageable(s_model.selected_seat);
    bool has_verify = s_model.has_verify_code &&
                      werewolf_ui_format_verify_code(
                          verify, sizeof(verify), s_model.verify_code);

    render_reset(WW_UI_PANEL, WW_UI_TEXT);
    if (player == NULL || !can_kick) {
        label_place(s_title, "PLAYER LEFT", 0, 4, 202, 25,
                    &ui_font_kode_bold_21, WW_UI_ERROR,
                    LV_TEXT_ALIGN_CENTER);
        label_place(s_subtitle, "SELECTION EXPIRED", 0, 32, 202, 18,
                    &ui_font_kode_regular_13, WW_UI_WARNING,
                    LV_TEXT_ALIGN_CENTER);
        cell_place(0U, 10, 137, 182, 42, "BACK", true, true, false);
        footer_set("OK BACK  HOLD DN BACK", WW_UI_TEXT);
        return;
    }

    (void)snprintf(title, sizeof(title), "%.10s",
                   text_or(player->name, "UNNAMED"));
    (void)snprintf(status, sizeof(status), "S%u  %s",
                   seat_number(player->seat),
                   player->ready ? "READY" : "WAIT");
    label_place(s_title, title, 0, 2, 202, 25,
                &ui_font_kode_bold_21, WW_UI_TEXT,
                LV_TEXT_ALIGN_CENTER);
    label_place(s_subtitle, status, 0, 29, 202, 18,
                &ui_font_kode_regular_13,
                player->ready ? WW_UI_SUCCESS : WW_UI_MUTED,
                LV_TEXT_ALIGN_CENTER);
    label_place(s_timer, has_verify ? verify : "VERIFY ------",
                0, 62, 202, 30, &ui_font_kode_bold_21,
                has_verify ? WW_UI_WARNING : WW_UI_MUTED,
                LV_TEXT_ALIGN_CENTER);
    label_place(s_detail, "COMPARE WITH PLAYER DEVICE",
                12, 99, 178, 24, &ui_font_kode_regular_11,
                WW_UI_TEXT, LV_TEXT_ALIGN_CENTER);
    cell_place(0U, 10, 126, 182, 38, "BACK",
               !s_player_action_focus_kick, true, false);
    cell_place(1U, 10, 172, 182, 38, "KICK PLAYER",
               s_player_action_focus_kick, can_kick, false);
    if (s_player_action_focus_kick && can_kick) {
        lv_obj_set_style_bg_color(s_cells[1], lv_color_hex(WW_UI_ERROR), 0);
        lv_obj_set_style_border_color(s_cells[1], lv_color_hex(WW_UI_ERROR), 0);
        lv_obj_set_style_text_color(s_cell_labels[1],
                                    lv_color_hex(WW_UI_TEXT), 0);
    } else {
        lv_obj_set_style_text_color(s_cell_labels[1],
                                    lv_color_hex(WW_UI_ERROR), 0);
    }
    footer_set(s_action_latched
                   ? "ACTION SENT  WAIT"
                   : (s_player_action_focus_kick
                          ? "UP/DN  OK KICK  HOLD DN"
                          : "UP/DN  OK BACK  HOLD DN"),
               s_action_latched ? WW_UI_MUTED : WW_UI_TEXT);
}

static void render_private(void)
{
    char line[64];
    const lv_font_t *result_font = &ui_font_kode_bold_21;
    render_reset(WW_UI_BG, WW_UI_TEXT);
    label_place(s_title, "PRIVATE", 10, 4, 182, 25,
                &ui_font_kode_bold_21, WW_UI_TEXT, LV_TEXT_ALIGN_CENTER);

    if (s_model.waiting_for_players) {
        label_place(s_timer, "WAITING FOR PLAYERS", 0, 70, 202, 32,
                    &ui_font_kode_bold_15, WW_UI_FOCUS,
                    LV_TEXT_ALIGN_CENTER);
        label_place(s_detail, "RESULT CONFIRMED", 12, 119, 178, 32,
                    &ui_font_kode_regular_13, WW_UI_TEXT,
                    LV_TEXT_ALIGN_CENTER);
        footer_set("WAIT FOR ALL PLAYERS", WW_UI_TEXT);
        return;
    }

    if (!s_private_revealed) {
        label_place(s_timer, "PRIVATE SEALED", 0, 76, 202, 28,
                    &ui_font_kode_bold_21, WW_UI_FOCUS, LV_TEXT_ALIGN_CENTER);
        label_place(s_detail, "Keep the screen facing you.\nRelease OK to hide.",
                    12, 119, 178, 52, &ui_font_kode_regular_13,
                    WW_UI_TEXT, LV_TEXT_ALIGN_CENTER);
        footer_set("PRESS + HOLD OK TO REVEAL", WW_UI_TEXT);
        return;
    }

    if (s_model.page == WEREWOLF_UI_PAGE_ROLE) {
        snprintf(line, sizeof(line), "YOU ARE %s", role_name(s_model.local_role));
        label_place(s_detail,
                    text_or(s_model.private_detail, "Your role is private."),
                    10, 104, 182, 68, &ui_font_kode_regular_13,
                    WW_UI_TEXT, LV_TEXT_ALIGN_CENTER);
    } else {
        if (s_model.private_seat == WEREWOLF_UI_NO_SEAT ||
            s_model.private_faction == WEREWOLF_UI_FACTION_UNKNOWN) {
            snprintf(line, sizeof(line), "NO PRIVATE RESULT");
            result_font = &ui_font_kode_bold_15;
            label_place(s_detail, "THIS NIGHT",
                        10, 104, 182, 68, &ui_font_kode_regular_13,
                        WW_UI_TEXT, LV_TEXT_ALIGN_CENTER);
        } else {
            snprintf(line, sizeof(line), "SEAT %u",
                     seat_number(s_model.private_seat));
            label_place(s_detail, faction_name(s_model.private_faction),
                        10, 104, 182, 68, &ui_font_kode_regular_13,
                        WW_UI_TEXT, LV_TEXT_ALIGN_CENTER);
        }
    }
    label_place(s_timer, line, 0, 60, 202, 30,
                result_font, WW_UI_FOCUS, LV_TEXT_ALIGN_CENTER);
    footer_set("RELEASE OK TO SEAL", WW_UI_TEXT);
}

static void render_target(bool vote)
{
    char line[64];
    const bool pending = s_confirm_pending;
    render_reset(WW_UI_BG, WW_UI_TEXT);
    label_place(s_title, vote ? "SECRET VOTE" : "NIGHT ACTION",
                8, 0, 194, 25, &ui_font_kode_bold_21,
                WW_UI_TEXT, LV_TEXT_ALIGN_LEFT);

    if (vote) {
        snprintf(line, sizeof(line), "ROUND %u  %u/%u  GUIDE %uS",
                 s_model.round, s_model.votes_received,
                 s_model.votes_expected, s_model.guide_seconds);
    } else {
        snprintf(line, sizeof(line), "NIGHT %u  GUIDE %uS",
                 s_model.round, s_model.guide_seconds);
    }
    label_place(s_subtitle, line, 8, 25, 194, 18,
                &ui_font_kode_regular_13, WW_UI_FOCUS, LV_TEXT_ALIGN_LEFT);

    /* This renderer never branches on local_role. Night brightness, animation
     * (none), object layout and confirm cadence therefore stay role-neutral. */
    if (s_action_latched || !s_model.input_enabled || !local_player_alive()) {
        label_place(s_timer, s_action_latched ? "ACTION SENT" : "WAITING",
                    0, 74, 202, 30, &ui_font_kode_bold_21,
                    WW_UI_FOCUS, LV_TEXT_ALIGN_CENTER);
        label_place(s_detail, "PRIVATE ACTION WINDOW\nKeep this display private.",
                    10, 116, 182, 52, &ui_font_kode_regular_13,
                    WW_UI_TEXT, LV_TEXT_ALIGN_CENTER);
        footer_set("INPUT PAUSED", WW_UI_TEXT);
        return;
    }

    if (pending) {
        const werewolf_ui_player_t *target = player_by_seat(s_target_cursor);
        if (s_target_cursor == WEREWOLF_UI_NO_SEAT) {
            snprintf(line, sizeof(line), "--  NO TARGET");
        } else {
            snprintf(line, sizeof(line), "S%u  %s",
                     seat_number(s_target_cursor),
                     target ? text_or(target->name, "PLAYER") : "NO TARGET");
        }
        cell_place(0, 10, 64, 182, 58, line, true,
                   target != NULL, true);
        label_place(s_detail,
                    vote ? "This vote is secret.\nA click cannot submit it."
                         : "Check the seat.\nA click cannot submit it.",
                    12, 137, 178, 48, &ui_font_kode_regular_13,
                    WW_UI_TEXT, LV_TEXT_ALIGN_CENTER);
        footer_set("HOLD OK CONFIRM  HOLD DN BACK", WW_UI_TEXT);
        return;
    }

    for (unsigned i = 0; i < WEREWOLF_UI_PLAYER_COUNT; ++i) {
        const werewolf_ui_player_t *player = &s_model.players[i];
        if (player->occupied) {
            snprintf(line, sizeof(line), "S%u %-11.11s %s",
                     seat_number(player->seat),
                     text_or(player->name, "PLAYER"),
                     player->alive ? "" : "OUT");
        } else {
            snprintf(line, sizeof(line), "S%u -- EMPTY --", i + 1U);
        }
        cell_place(i, 0, 44 + (int)i * 22, 202, 20, line,
                   player->seat == s_target_cursor,
                   player->occupied && player->alive,
                   true);
    }
    footer_set("UP/DN TARGET  |  OK REVIEW", WW_UI_TEXT);
}

static void render_day_result(void)
{
    char line[40];
    render_reset(WW_UI_PANEL_WARM, WW_UI_TEXT);
    label_place(s_title, text_or(s_model.headline, "DAY RESULT"),
                0, 6, 202, 28, &ui_font_kode_bold_21,
                WW_UI_TEXT, LV_TEXT_ALIGN_CENTER);
    snprintf(line, sizeof(line), "DAY %u", s_model.round);
    label_place(s_subtitle, line, 0, 37, 202, 20,
                &ui_font_kode_regular_13, WW_UI_SUCCESS, LV_TEXT_ALIGN_CENTER);
    label_place(s_detail, text_or(s_model.detail, "No one was eliminated."),
                12, 76, 178, 85, &ui_font_kode_regular_13,
                WW_UI_TEXT, LV_TEXT_ALIGN_CENTER);
    footer_set(s_action_latched ? "ACK SENT  WAIT" : "OK CONTINUE", WW_UI_TEXT);
}

static void render_speaking(void)
{
    char line[64];
    const werewolf_ui_player_t *speaker = player_by_seat(s_model.speaker_seat);
    render_reset(WW_UI_PANEL_WARM, WW_UI_TEXT);
    label_place(s_title, text_or(s_model.headline, "SPEAKING"),
                0, 5, 202, 26, &ui_font_kode_bold_21,
                WW_UI_TEXT, LV_TEXT_ALIGN_CENTER);
    if (s_model.speaker_seat == WEREWOLF_UI_NO_SEAT) {
        snprintf(line, sizeof(line), "--  WAITING");
    } else {
        snprintf(line, sizeof(line), "S%u  %s",
                 seat_number(s_model.speaker_seat),
                 speaker ? text_or(speaker->name, "PLAYER") : "PLAYER");
    }
    label_place(s_subtitle, line, 0, 39, 202, 22,
                &ui_font_kode_regular_13, WW_UI_SUCCESS, LV_TEXT_ALIGN_CENTER);
    snprintf(line, sizeof(line), "GUIDE %uS", s_model.guide_seconds);
    label_place(s_timer, line, 0, 82, 202, 34,
                &ui_font_kode_bold_21,
                WW_UI_FOCUS,
                LV_TEXT_ALIGN_CENTER);
    label_place(s_detail, text_or(s_model.detail, "Speech is not recorded."),
                12, 132, 178, 40, &ui_font_kode_regular_13,
                WW_UI_TEXT, LV_TEXT_ALIGN_CENTER);
    if (s_model.input_enabled && local_player_alive() &&
        s_model.local_seat == s_model.speaker_seat && !s_action_latched) {
        footer_set("HOLD OK TO PASS", WW_UI_TEXT);
    } else {
        footer_set("LISTEN  INPUT PAUSED", WW_UI_TEXT);
    }
}

static void render_eliminated(void)
{
    char line[48];
    render_reset(WW_UI_PANEL, WW_UI_TEXT);
    label_place(s_title, text_or(s_model.headline, "PLAYER OUT"),
                0, 8, 202, 28, &ui_font_kode_bold_21,
                WW_UI_ERROR, LV_TEXT_ALIGN_CENTER);
    if (s_model.affected_seat != WEREWOLF_UI_NO_SEAT) {
        snprintf(line, sizeof(line), "SEAT %u",
                 seat_number(s_model.affected_seat));
    } else {
        snprintf(line, sizeof(line), "NO EXILE");
    }
    label_place(s_timer, line, 0, 63, 202, 32,
                &ui_font_kode_bold_21, WW_UI_TEXT, LV_TEXT_ALIGN_CENTER);
    label_place(s_detail,
                text_or(s_model.detail,
                        "Role stays hidden. Dead players keep public view."),
                12, 113, 178, 62, &ui_font_kode_regular_13,
                WW_UI_TEXT, LV_TEXT_ALIGN_CENTER);
    footer_set(s_action_latched ? "ACK SENT  WAIT" : "OK CONTINUE", WW_UI_TEXT);
}

static void render_game_over(void)
{
    char line[64];
    uint32_t winner_color = WW_UI_WARNING;

    if (s_model.winner == WEREWOLF_UI_WINNER_GOOD) {
        winner_color = WW_UI_SUCCESS;
    } else if (s_model.winner == WEREWOLF_UI_WINNER_WOLVES) {
        winner_color = WW_UI_ERROR;
    }
    render_reset(WW_UI_PANEL, WW_UI_TEXT);
    label_place(s_title, winner_name(s_model.winner),
                0, 0, 202, 25, &ui_font_kode_bold_21,
                winner_color, LV_TEXT_ALIGN_CENTER);
    label_place(s_subtitle, "ROLE REVIEW", 0, 25, 202, 18,
                &ui_font_kode_regular_13, WW_UI_SUCCESS, LV_TEXT_ALIGN_CENTER);

    for (unsigned i = 0; i < WEREWOLF_UI_PLAYER_COUNT; ++i) {
        const werewolf_ui_player_t *player = &s_model.players[i];
        snprintf(line, sizeof(line), "S%u %-10.10s %s",
                 player->occupied ? seat_number(player->seat) : i + 1U,
                 player->occupied ? text_or(player->name, "PLAYER") : "EMPTY",
                 player->occupied ? role_name(player->role) : "--");
        cell_place(i, 0, 44 + (int)i * 22, 202, 20, line,
                   player->seat == s_model.local_seat,
                   player->occupied, false);
    }
    if (!s_model.input_enabled) {
        footer_set("DELIVERING REVIEW  |  WAIT", WW_UI_TEXT);
    } else if (s_action_latched) {
        footer_set("LEAVE SENT  |  WAIT", WW_UI_TEXT);
    } else {
        footer_set("OK LEAVE REVIEW", WW_UI_TEXT);
    }
}

static void render_error(void)
{
    render_reset(WW_UI_ERROR_PANEL, WW_UI_TEXT);
    label_place(s_title, error_title(s_model.error), 0, 10, 202, 28,
                &ui_font_kode_bold_21, WW_UI_ERROR, LV_TEXT_ALIGN_CENTER);
    label_place(s_timer, error_name(s_model.error), 4, 62, 194, 48,
                &ui_font_kode_bold_21, WW_UI_TEXT, LV_TEXT_ALIGN_CENTER);
    label_place(s_detail, text_or(s_model.detail,
                                  "The game has not advanced."),
                12, 126, 178, 46, &ui_font_kode_regular_13,
                WW_UI_TEXT, LV_TEXT_ALIGN_CENTER);
    if (s_model.recoverable) {
        footer_set("OK RETRY  HOLD DN EXIT", WW_UI_TEXT);
    } else {
        footer_set("HOLD DN EXIT", WW_UI_TEXT);
    }
}

static void render_room_closed(void)
{
    render_reset(WW_UI_PANEL, WW_UI_TEXT);
}

static void render_connection_banner(void)
{
    const char *text = NULL;
    uint32_t color = WW_UI_WARNING;

    if (s_model.page == WEREWOLF_UI_PAGE_ERROR || room_modal_active()) {
        hide_obj(s_banner);
        return;
    }

    switch (s_model.connection) {
    case WEREWOLF_UI_CONNECTION_RECONNECTING:
        text = "RECONNECTING  INPUT PAUSED";
        break;
    case WEREWOLF_UI_CONNECTION_DISCONNECTED:
        text = "DISCONNECTED  INPUT PAUSED";
        color = WW_UI_ERROR;
        break;
    case WEREWOLF_UI_CONNECTION_HOST_LOST:
        text = "HOST LOST  GAME ABORTED";
        color = WW_UI_ERROR;
        break;
    default:
        hide_obj(s_banner);
        return;
    }

    show_obj(s_banner);
    lv_obj_set_style_bg_color(s_banner, lv_color_hex(color), 0);
    lv_obj_set_style_text_color(
        s_banner_label,
        lv_color_hex(color == WW_UI_WARNING ? WW_UI_BG : WW_UI_TEXT), 0);
    lv_label_set_text(s_banner_label, text);
    lv_obj_center(s_banner_label);
}

static void render(void)
{
    if (!s_screen) {
        return;
    }

    switch (s_model.page) {
    case WEREWOLF_UI_PAGE_MODE:
        render_mode();
        break;
    case WEREWOLF_UI_PAGE_ROOM_LIST:
        render_room_list();
        break;
    case WEREWOLF_UI_PAGE_LOBBY:
        render_lobby();
        break;
    case WEREWOLF_UI_PAGE_PLAYER_ACTION:
        render_player_action();
        break;
    case WEREWOLF_UI_PAGE_ROLE:
    case WEREWOLF_UI_PAGE_PRIVATE_RESULT:
        render_private();
        break;
    case WEREWOLF_UI_PAGE_NIGHT_SELECT:
    case WEREWOLF_UI_PAGE_NIGHT_CONFIRM:
        render_target(false);
        break;
    case WEREWOLF_UI_PAGE_DAY_RESULT:
        render_day_result();
        break;
    case WEREWOLF_UI_PAGE_SPEAKING:
        render_speaking();
        break;
    case WEREWOLF_UI_PAGE_VOTE_SELECT:
    case WEREWOLF_UI_PAGE_VOTE_CONFIRM:
        render_target(true);
        break;
    case WEREWOLF_UI_PAGE_ELIMINATED:
        render_eliminated();
        break;
    case WEREWOLF_UI_PAGE_GAME_OVER:
        render_game_over();
        break;
    case WEREWOLF_UI_PAGE_ERROR:
        render_error();
        break;
    case WEREWOLF_UI_PAGE_ROOM_CLOSED:
        render_room_closed();
        break;
    default:
        render_error();
        break;
    }
    render_connection_banner();
    render_room_modal();
}

static void sanitize_model(werewolf_ui_model_t *model)
{
    model->local_name[WEREWOLF_UI_PLAYER_NAME_MAX - 1] = '\0';
    model->room_code[WEREWOLF_UI_ROOM_CODE_MAX - 1] = '\0';
    model->headline[WEREWOLF_UI_HEADLINE_MAX - 1] = '\0';
    model->detail[WEREWOLF_UI_DETAIL_MAX - 1] = '\0';
    model->private_detail[WEREWOLF_UI_PRIVATE_DETAIL_MAX - 1] = '\0';
    for (unsigned i = 0; i < WEREWOLF_UI_PLAYER_COUNT; ++i) {
        model->players[i].name[WEREWOLF_UI_PLAYER_NAME_MAX - 1] = '\0';
    }
    for (unsigned i = 0; i < WEREWOLF_UI_ROOM_COUNT_MAX; ++i) {
        model->rooms[i].code[WEREWOLF_UI_ROOM_CODE_MAX - 1] = '\0';
    }
}

void werewolf_ui_model_init(werewolf_ui_model_t *model)
{
    if (!model) {
        return;
    }
    memset(model, 0, sizeof(*model));
    model->page = WEREWOLF_UI_PAGE_MODE;
    model->public_phase = WEREWOLF_UI_PUBLIC_PHASE_MODE;
    model->connection = WEREWOLF_UI_CONNECTION_RADIO_OFF;
    model->battery_state = WEREWOLF_UI_BATTERY_UNAVAILABLE;
    model->signal = WEREWOLF_UI_SIGNAL_NO_SAMPLE;
    model->local_seat = WEREWOLF_UI_NO_SEAT;
    model->selected_seat = WEREWOLF_UI_NO_SEAT;
    model->affected_seat = WEREWOLF_UI_NO_SEAT;
    model->speaker_seat = WEREWOLF_UI_NO_SEAT;
    model->private_seat = WEREWOLF_UI_NO_SEAT;
    model->kick_seat = WEREWOLF_UI_NO_SEAT;
    for (unsigned i = 0; i < WEREWOLF_UI_PLAYER_COUNT; ++i) {
        model->players[i].seat = (uint8_t)i;
        model->players[i].alive = true;
        model->players[i].publicly_alive = true;
    }
}

bool werewolf_ui_create(const werewolf_ui_model_t *initial_model)
{
    if (s_screen) {
        return false;
    }

    werewolf_ui_model_init(&s_model);
    if (initial_model) {
        memcpy(&s_model, initial_model, sizeof(s_model));
        sanitize_model(&s_model);
    }

    s_mode_cursor = s_model.mode;
    s_target_cursor = s_model.selected_seat;
    s_lobby_cursor = WEREWOLF_UI_NO_SEAT;
    s_room_cursor_token = s_model.selected_room_token;
    s_player_action_focus_kick = false;
    s_confirm_pending = s_model.page == WEREWOLF_UI_PAGE_NIGHT_CONFIRM ||
                        s_model.page == WEREWOLF_UI_PAGE_VOTE_CONFIRM;
    s_private_revealed = false;
    s_private_press_armed = false;
    s_private_press_page = WEREWOLF_UI_PAGE_MODE;
    s_private_press_epoch = 0U;
    s_action_latched = false;
    s_close_focus_close = false;
    s_feedback = WEREWOLF_UI_FEEDBACK_NONE;
    normalize_target_cursor(true);
    normalize_room_cursor(true);
    normalize_lobby_cursor(true);

    s_screen = screen_create();
    s_panel = box(s_screen, 8, 50, 224, 240, WW_UI_PANEL, LV_OPA_COVER);
    lv_obj_set_style_radius(s_panel, 8, 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_pad_all(s_panel, 7, 0);
    s_moon_disc = box(s_panel, 148, -42, 100, 100,
                      WW_UI_MOON_DIM, LV_OPA_20);
    lv_obj_set_style_radius(s_moon_disc, LV_RADIUS_CIRCLE, 0);
    s_moon_cutout = box(s_panel, 124, -50, 92, 92,
                        WW_UI_PANEL, LV_OPA_COVER);
    lv_obj_set_style_radius(s_moon_cutout, LV_RADIUS_CIRCLE, 0);
    s_moon_horizon = box(s_panel, 132, 45, 88, 1,
                         WW_UI_FOCUS, LV_OPA_40);
    s_title = label_create(s_panel, "", &ui_font_kode_bold_21, WW_UI_TEXT);
    s_subtitle = label_create(s_panel, "", &ui_font_kode_regular_13,
                              WW_UI_TEXT);
    s_detail = label_create(s_panel, "", &ui_font_kode_regular_13,
                            WW_UI_TEXT);
    s_timer = label_create(s_panel, "", &ui_font_kode_bold_21, WW_UI_TEXT);

    for (unsigned i = 0; i < CELL_COUNT; ++i) {
        s_cells[i] = cell_create(s_panel);
        s_cell_bars[i] = box(s_cells[i], 0, 0, 4, 1,
                             WW_UI_STRUCTURE, LV_OPA_COVER);
        s_cell_labels[i] = label_create(s_cells[i], "",
                                        &ui_font_kode_regular_13, WW_UI_TEXT);
        s_lobby_seat_labels[i] = label_create(
            s_cells[i], "", &ui_font_kode_regular_11, WW_UI_MUTED);
        s_lobby_identity_badges[i] = box(s_cells[i], 35, 3, 14, 14,
                                        WW_UI_BG, LV_OPA_COVER);
        lv_obj_set_style_radius(s_lobby_identity_badges[i],
                                LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s_lobby_identity_badges[i], 1, 0);
        lv_obj_set_style_border_color(s_lobby_identity_badges[i],
                                      lv_color_hex(WW_UI_STRUCTURE), 0);
        lv_obj_set_style_border_opa(s_lobby_identity_badges[i],
                                    LV_OPA_COVER, 0);
        s_lobby_identity_labels[i] = centered_label(
            s_lobby_identity_badges[i], "", &ui_font_kode_bold_13,
            WW_UI_TEXT);
        s_lobby_name_labels[i] = label_create(
            s_cells[i], "", &ui_font_kode_regular_13, WW_UI_TEXT);
        s_lobby_state_labels[i] = label_create(
            s_cells[i], "", &ui_font_kode_regular_11, WW_UI_MUTED);
        hide_obj(s_lobby_seat_labels[i]);
        hide_obj(s_lobby_identity_badges[i]);
        hide_obj(s_lobby_name_labels[i]);
        hide_obj(s_lobby_state_labels[i]);
    }

    box(s_screen, 8, 297, 224, 1, WW_UI_STRUCTURE, LV_OPA_80);
    lv_obj_t *footer_mark = box(s_screen, 113, 295, 14, 3,
                                WW_UI_FOCUS, LV_OPA_COVER);
    lv_obj_set_style_radius(footer_mark, 2, 0);
    s_footer = label_create(s_screen, "", &ui_font_kode_regular_11,
                            WW_UI_TEXT);
    s_banner = box(s_screen, 8, 50, 224, 30,
                   WW_UI_WARNING, LV_OPA_COVER);
    lv_obj_set_style_radius(s_banner, 6, 0);
    s_banner_label = centered_label(s_banner, "",
                                    &ui_font_kode_bold_13, WW_UI_BG);

    s_modal_overlay = box(s_screen, 8, 50, 224, 240,
                          WW_UI_BG, LV_OPA_80);
    lv_obj_set_style_radius(s_modal_overlay, 8, 0);
    s_modal_dialog = box(s_modal_overlay, 12, 18, 200, 204,
                         WW_UI_ERROR_PANEL, LV_OPA_COVER);
    lv_obj_set_style_radius(s_modal_dialog, 8, 0);
    lv_obj_set_style_border_width(s_modal_dialog, 1, 0);
    lv_obj_set_style_border_color(s_modal_dialog,
                                  lv_color_hex(WW_UI_STRUCTURE), 0);
    s_modal_rail = box(s_modal_dialog, 95, 8, 10, 10,
                       WW_UI_ERROR, LV_OPA_COVER);
    lv_obj_set_style_radius(s_modal_rail, LV_RADIUS_CIRCLE, 0);
    s_modal_title = label_create(s_modal_dialog, "",
                                 &ui_font_kode_bold_21, WW_UI_TEXT);
    s_modal_status = label_create(s_modal_dialog, "",
                                  &ui_font_kode_bold_13, WW_UI_WARNING);
    s_modal_body = label_create(s_modal_dialog, "",
                                &ui_font_kode_regular_13, WW_UI_TEXT);
    for (unsigned i = 0U; i < 2U; ++i) {
        s_modal_options[i] = box(s_modal_dialog, 12, 120, 176, 30,
                                 WW_UI_PANEL_ALT, LV_OPA_COVER);
        lv_obj_set_style_radius(s_modal_options[i], 5, 0);
        lv_obj_set_style_border_width(s_modal_options[i], 1, 0);
        s_modal_option_bars[i] = box(s_modal_options[i], 0, 0, 4, 30,
                                     WW_UI_STRUCTURE, LV_OPA_COVER);
        lv_obj_set_style_radius(s_modal_option_bars[i], 2, 0);
        s_modal_option_labels[i] = label_create(
            s_modal_options[i], "", &ui_font_kode_bold_13, WW_UI_TEXT);
    }
    hide_obj(s_modal_overlay);

    render();
    lv_screen_load(s_screen);
    return true;
}

void werewolf_ui_set_model(const werewolf_ui_model_t *model)
{
    if (!s_screen || !model) {
        return;
    }

    werewolf_ui_page_t old_page = s_model.page;
    uint32_t old_private_epoch = s_model.private_epoch;
    bool old_input_enabled = s_model.input_enabled;
    bool old_room_close_prompt = s_model.room_close_prompt;
    bool old_player_kicking = s_model.player_kicking;
    uint8_t old_selected_seat = s_model.selected_seat;
    uint8_t old_target_cursor = s_target_cursor;
    memcpy(&s_model, model, sizeof(s_model));
    sanitize_model(&s_model);

    bool page_changed = old_page != s_model.page;
    bool private_context_changed = page_changed ||
                                   old_private_epoch != s_model.private_epoch;
    if (page_changed) {
        s_confirm_pending = s_model.page == WEREWOLF_UI_PAGE_NIGHT_CONFIRM ||
                            s_model.page == WEREWOLF_UI_PAGE_VOTE_CONFIRM;
        s_action_latched = false;
        s_mode_cursor = s_model.mode;
        s_target_cursor = s_model.selected_seat;
        s_room_cursor_token = s_model.selected_room_token;
        s_close_focus_close = false;
        s_player_action_focus_kick = false;
        if (s_model.page == WEREWOLF_UI_PAGE_LOBBY) {
            normalize_lobby_cursor(true);
        } else {
            s_lobby_cursor = WEREWOLF_UI_NO_SEAT;
        }
    } else if (!old_input_enabled && s_model.input_enabled) {
        /* A reconnect/restarted uncommitted phase explicitly reopened input. */
        s_action_latched = false;
        s_confirm_pending = false;
    }
    if (!page_changed && s_model.page == WEREWOLF_UI_PAGE_LOBBY) {
        normalize_lobby_cursor(false);
    }
    if (!old_room_close_prompt && s_model.room_close_prompt) {
        s_close_focus_close = false;
        s_action_latched = false;
    } else if (old_room_close_prompt && !s_model.room_close_prompt) {
        /* CANCEL and CONFIRM both return through a same-page model update.
         * Release the local action gate when the authoritative controller
         * closes the prompt; otherwise LOBBY remains stuck on ACTION SENT. */
        s_close_focus_close = false;
        s_action_latched = false;
    } else if (!s_model.room_close_prompt) {
        s_close_focus_close = false;
    }
    if (!page_changed && s_model.page == WEREWOLF_UI_PAGE_PLAYER_ACTION &&
        old_selected_seat != s_model.selected_seat) {
        s_player_action_focus_kick = false;
    }
    if (old_player_kicking != s_model.player_kicking) {
        s_action_latched = false;
    }

    if (private_context_changed) {
        s_private_revealed = false;
        s_private_press_armed = false;
        s_private_press_epoch = 0U;
    }
    if (s_model.connection != WEREWOLF_UI_CONNECTION_ONLINE ||
        !s_model.input_enabled) {
        s_private_revealed = false;
        s_private_press_armed = false;
        s_private_press_epoch = 0U;
        s_confirm_pending = false;
    }
    normalize_target_cursor(page_changed);
    normalize_room_cursor(page_changed);
    if (!page_changed && s_target_cursor != old_target_cursor) {
        /* Never carry an armed confirmation onto a replacement target. */
        s_confirm_pending = false;
    }
    if (s_model.page == WEREWOLF_UI_PAGE_LOBBY && !lobby_can_start()) {
        s_confirm_pending = false;
    }
    render();
}

static bool emit_action(werewolf_ui_action_t *action,
                        werewolf_ui_action_type_t type, uint8_t seat)
{
    if (action) {
        action->type = type;
        action->source_revision = s_model.revision;
        action->private_epoch = s_model.private_epoch;
        action->source_page = s_model.page;
        action->mode = s_mode_cursor;
        action->seat = seat;
        action->room_token = 0U;
    }
    return type != WEREWOLF_UI_ACTION_NONE;
}

static bool emit_room_action(werewolf_ui_action_t *action,
                             werewolf_ui_action_type_t type,
                             uint32_t token)
{
    if (!emit_action(action, type, WEREWOLF_UI_NO_SEAT)) {
        return false;
    }
    if (action != NULL) {
        action->room_token = token;
    }
    return token != 0U;
}

static bool handle_private_key(werewolf_ui_key_t key,
                               werewolf_ui_key_event_t event,
                               werewolf_ui_action_t *action)
{
    if (!s_model.input_enabled) {
        bool needs_render = s_private_revealed;
        s_private_revealed = false;
        s_private_press_armed = false;
        s_private_press_epoch = 0U;
        if (needs_render) {
            render();
            if (s_model.page == WEREWOLF_UI_PAGE_ROLE) {
                s_feedback = WEREWOLF_UI_FEEDBACK_PRIVATE_SEAL;
            }
        }
        return false;
    }
    if (key != WEREWOLF_UI_KEY_OK) {
        return false;
    }
    if (event == WEREWOLF_UI_KEY_EVENT_PRESS) {
        s_private_revealed = false;
        s_private_press_armed = true;
        s_private_press_page = s_model.page;
        s_private_press_epoch = s_model.private_epoch;
        return false;
    }

    if (event == WEREWOLF_UI_KEY_EVENT_LONG) {
        if (s_private_press_armed && !s_private_revealed &&
            s_private_press_page == s_model.page &&
            s_private_press_epoch == s_model.private_epoch) {
            s_private_revealed = true;
            render();
            if (s_model.page == WEREWOLF_UI_PAGE_ROLE) {
                s_feedback = WEREWOLF_UI_FEEDBACK_PRIVATE_REVEAL;
            }
        }
        return false;
    }

    if (event == WEREWOLF_UI_KEY_EVENT_RELEASE) {
        bool was_revealed = s_private_revealed;
        s_private_revealed = false;
        s_private_press_armed = false;
        s_private_press_epoch = 0U;
        if (was_revealed) {
            /* render_reset() clears secret strings before the release action
             * can leave this module. */
            render();
            if (s_model.page == WEREWOLF_UI_PAGE_ROLE) {
                s_feedback = WEREWOLF_UI_FEEDBACK_PRIVATE_SEAL;
            }
        }
        if (!was_revealed || s_action_latched) {
            return false;
        }
        s_action_latched = true;
        return emit_action(action,
                           s_model.page == WEREWOLF_UI_PAGE_ROLE ?
                           WEREWOLF_UI_ACTION_ROLE_SEEN :
                           WEREWOLF_UI_ACTION_ACK_RESULT,
                           WEREWOLF_UI_NO_SEAT);
    }
    return false;
}

static bool handle_target_key(bool vote, werewolf_ui_key_t key,
                              werewolf_ui_key_event_t event,
                              werewolf_ui_action_t *action)
{
    if (!s_model.input_enabled || !local_player_alive() || s_action_latched) {
        return false;
    }
    if (event == WEREWOLF_UI_KEY_EVENT_LONG &&
        key == WEREWOLF_UI_KEY_DOWN && s_confirm_pending) {
        s_confirm_pending = false;
        render();
        if (vote) {
            s_feedback = WEREWOLF_UI_FEEDBACK_SELECT;
        }
        return false;
    }
    if (event == WEREWOLF_UI_KEY_EVENT_CLICK &&
        (key == WEREWOLF_UI_KEY_UP || key == WEREWOLF_UI_KEY_DOWN)) {
        bool was_pending = s_confirm_pending;
        uint8_t old_cursor = s_target_cursor;
        if (s_confirm_pending) {
            s_confirm_pending = false;
        }
        move_target_cursor(key == WEREWOLF_UI_KEY_UP ? -1 : 1);
        if (was_pending || old_cursor != s_target_cursor) {
            render();
            if (vote) {
                s_feedback = WEREWOLF_UI_FEEDBACK_MOVE;
            }
        }
        return false;
    }
    if (key != WEREWOLF_UI_KEY_OK || s_target_cursor == WEREWOLF_UI_NO_SEAT) {
        return false;
    }
    if (event == WEREWOLF_UI_KEY_EVENT_CLICK && !s_confirm_pending) {
        s_confirm_pending = true;
        render();
        if (vote) {
            s_feedback = WEREWOLF_UI_FEEDBACK_CONFIRM_ARMED;
        }
        return false;
    }
    if (event == WEREWOLF_UI_KEY_EVENT_LONG && s_confirm_pending) {
        s_action_latched = true;
        render();
        if (vote) {
            s_feedback = WEREWOLF_UI_FEEDBACK_CONFIRMED;
        }
        return emit_action(action,
                           vote ? WEREWOLF_UI_ACTION_SUBMIT_VOTE :
                                  WEREWOLF_UI_ACTION_SUBMIT_NIGHT_TARGET,
                           s_target_cursor);
    }
    return false;
}

bool werewolf_ui_handle_key(werewolf_ui_key_t key,
                            werewolf_ui_key_event_t event,
                            werewolf_ui_action_t *action)
{
    s_feedback = WEREWOLF_UI_FEEDBACK_NONE;
    if (action) {
        action->type = WEREWOLF_UI_ACTION_NONE;
        action->source_revision = s_model.revision;
        action->private_epoch = s_model.private_epoch;
        action->source_page = s_model.page;
        action->mode = s_mode_cursor;
        action->seat = WEREWOLF_UI_NO_SEAT;
        action->room_token = 0U;
    }
    if (!s_screen) {
        return false;
    }

    /* Release always gets first priority so a disconnect cannot leave a secret
     * visible while the network state is changing. */
    if ((s_model.page == WEREWOLF_UI_PAGE_ROLE ||
         s_model.page == WEREWOLF_UI_PAGE_PRIVATE_RESULT) &&
        key == WEREWOLF_UI_KEY_OK &&
        event == WEREWOLF_UI_KEY_EVENT_RELEASE) {
        return handle_private_key(key, event, action);
    }

    /* Modal input is handled before the connection gate. A room-closed notice
     * is intentionally offline, yet its sole OK action must remain usable. */
    if (s_model.page == WEREWOLF_UI_PAGE_ROOM_CLOSED) {
        if (key == WEREWOLF_UI_KEY_OK &&
            event == WEREWOLF_UI_KEY_EVENT_CLICK) {
            s_feedback = WEREWOLF_UI_FEEDBACK_SELECT;
            return emit_action(action,
                               WEREWOLF_UI_ACTION_ACK_ROOM_CLOSED,
                               WEREWOLF_UI_NO_SEAT);
        }
        return false;
    }
    if (s_model.player_kicking) {
        return false;
    }
    if (s_model.room_closing || s_model.leaving_room) {
        return false;
    }
    if (s_model.page == WEREWOLF_UI_PAGE_LOBBY && s_model.is_host &&
        s_model.room_close_prompt) {
        if (!s_model.input_enabled || s_action_latched) {
            return false;
        }
        if (event == WEREWOLF_UI_KEY_EVENT_CLICK &&
            (key == WEREWOLF_UI_KEY_UP || key == WEREWOLF_UI_KEY_DOWN)) {
            s_close_focus_close = !s_close_focus_close;
            render();
            s_feedback = WEREWOLF_UI_FEEDBACK_MOVE;
            return false;
        }
        if (key == WEREWOLF_UI_KEY_DOWN &&
            event == WEREWOLF_UI_KEY_EVENT_LONG) {
            s_action_latched = true;
            render();
            s_feedback = WEREWOLF_UI_FEEDBACK_SELECT;
            return emit_action(action,
                               WEREWOLF_UI_ACTION_CANCEL_CLOSE_ROOM,
                               WEREWOLF_UI_NO_SEAT);
        }
        if (key == WEREWOLF_UI_KEY_OK &&
            event == WEREWOLF_UI_KEY_EVENT_CLICK &&
            !s_close_focus_close) {
            s_action_latched = true;
            render();
            s_feedback = WEREWOLF_UI_FEEDBACK_SELECT;
            return emit_action(action,
                               WEREWOLF_UI_ACTION_CANCEL_CLOSE_ROOM,
                               WEREWOLF_UI_NO_SEAT);
        }
        if (key == WEREWOLF_UI_KEY_OK &&
            event == WEREWOLF_UI_KEY_EVENT_LONG &&
            s_close_focus_close) {
            s_action_latched = true;
            render();
            s_feedback = WEREWOLF_UI_FEEDBACK_CONFIRMED;
            return emit_action(action,
                               WEREWOLF_UI_ACTION_CONFIRM_CLOSE_ROOM,
                               WEREWOLF_UI_NO_SEAT);
        }
        return false;
    }

    if (s_model.page == WEREWOLF_UI_PAGE_ROOM_LIST) {
        if (key == WEREWOLF_UI_KEY_DOWN &&
            event == WEREWOLF_UI_KEY_EVENT_LONG) {
            s_feedback = WEREWOLF_UI_FEEDBACK_CONFIRMED;
            return emit_action(action, WEREWOLF_UI_ACTION_LEAVE_GAME,
                               WEREWOLF_UI_NO_SEAT);
        }
        if (event == WEREWOLF_UI_KEY_EVENT_CLICK &&
            (key == WEREWOLF_UI_KEY_UP || key == WEREWOLF_UI_KEY_DOWN)) {
            move_room_cursor(key == WEREWOLF_UI_KEY_UP ? -1 : 1);
            render();
            s_feedback = WEREWOLF_UI_FEEDBACK_MOVE;
            return emit_room_action(action, WEREWOLF_UI_ACTION_SELECT_ROOM,
                                    s_room_cursor_token);
        }
        if (key == WEREWOLF_UI_KEY_OK &&
            event == WEREWOLF_UI_KEY_EVENT_CLICK &&
            room_by_token(s_room_cursor_token) != NULL) {
            s_feedback = WEREWOLF_UI_FEEDBACK_SELECT;
            return emit_room_action(
                action, WEREWOLF_UI_ACTION_JOIN_SELECTED_ROOM,
                s_room_cursor_token);
        }
        return false;
    }

    if (s_model.page == WEREWOLF_UI_PAGE_LOBBY && s_model.is_host &&
        s_model.input_enabled && !s_action_latched &&
        key == WEREWOLF_UI_KEY_DOWN &&
        event == WEREWOLF_UI_KEY_EVENT_LONG) {
        s_feedback = WEREWOLF_UI_FEEDBACK_CONFIRM_ARMED;
        return emit_action(action,
                           WEREWOLF_UI_ACTION_REQUEST_CLOSE_ROOM,
                           WEREWOLF_UI_NO_SEAT);
    }

    /* Joining must always have a deterministic escape route.  This is checked
     * before the connection gate because scanning/pairing intentionally pause
     * normal lobby input while cancellation remains available. */
    if (s_model.page == WEREWOLF_UI_PAGE_LOBBY && !s_model.is_host &&
        key == WEREWOLF_UI_KEY_DOWN &&
        event == WEREWOLF_UI_KEY_EVENT_LONG &&
        (s_model.connection == WEREWOLF_UI_CONNECTION_SCANNING ||
         s_model.connection == WEREWOLF_UI_CONNECTION_PAIRING ||
         s_model.connection == WEREWOLF_UI_CONNECTION_ONLINE)) {
        s_feedback = WEREWOLF_UI_FEEDBACK_CONFIRMED;
        return emit_action(action, WEREWOLF_UI_ACTION_LEAVE_GAME,
                           WEREWOLF_UI_NO_SEAT);
    }

    if (s_model.connection != WEREWOLF_UI_CONNECTION_ONLINE &&
        s_model.page != WEREWOLF_UI_PAGE_MODE &&
        s_model.page != WEREWOLF_UI_PAGE_ERROR) {
        return false;
    }

    switch (s_model.page) {
    case WEREWOLF_UI_PAGE_MODE:
        if (event == WEREWOLF_UI_KEY_EVENT_CLICK &&
            (key == WEREWOLF_UI_KEY_UP || key == WEREWOLF_UI_KEY_DOWN)) {
            s_mode_cursor = s_mode_cursor == WEREWOLF_UI_MODE_CREATE ?
                            WEREWOLF_UI_MODE_JOIN : WEREWOLF_UI_MODE_CREATE;
            render();
            s_feedback = WEREWOLF_UI_FEEDBACK_MOVE;
        } else if (event == WEREWOLF_UI_KEY_EVENT_CLICK &&
                   key == WEREWOLF_UI_KEY_OK) {
            s_feedback = WEREWOLF_UI_FEEDBACK_SELECT;
            return emit_action(action,
                               s_mode_cursor == WEREWOLF_UI_MODE_CREATE ?
                               WEREWOLF_UI_ACTION_CREATE_ROOM :
                               WEREWOLF_UI_ACTION_JOIN_ROOM,
                               WEREWOLF_UI_NO_SEAT);
        }
        break;

    case WEREWOLF_UI_PAGE_ROOM_LIST:
        break;

    case WEREWOLF_UI_PAGE_LOBBY:
        if (!s_model.input_enabled || s_action_latched) {
            break;
        }
        if (event == WEREWOLF_UI_KEY_EVENT_CLICK &&
            (key == WEREWOLF_UI_KEY_UP || key == WEREWOLF_UI_KEY_DOWN)) {
            uint8_t old_cursor = s_lobby_cursor;

            if (s_model.is_host) {
                move_lobby_cursor(key == WEREWOLF_UI_KEY_UP ? -1 : 1);
            }
            if (old_cursor != s_lobby_cursor) {
                render();
                s_feedback = WEREWOLF_UI_FEEDBACK_MOVE;
            }
        } else if (s_model.is_host &&
                   s_lobby_cursor != s_model.local_seat &&
                   lobby_guest_is_manageable(s_lobby_cursor) &&
                   key == WEREWOLF_UI_KEY_OK &&
                   event == WEREWOLF_UI_KEY_EVENT_CLICK) {
            s_action_latched = true;
            render();
            s_feedback = WEREWOLF_UI_FEEDBACK_SELECT;
            return emit_action(action,
                               WEREWOLF_UI_ACTION_OPEN_PLAYER_ACTION,
                               s_lobby_cursor);
        } else if (key == WEREWOLF_UI_KEY_OK &&
                   event == WEREWOLF_UI_KEY_EVENT_CLICK &&
                   (!s_model.is_host ||
                    s_lobby_cursor == s_model.local_seat)) {
            s_feedback = s_model.local_ready
                             ? WEREWOLF_UI_FEEDBACK_READY_OFF
                             : WEREWOLF_UI_FEEDBACK_READY_ON;
            return emit_action(action, WEREWOLF_UI_ACTION_TOGGLE_READY,
                               s_model.local_seat);
        } else if (key == WEREWOLF_UI_KEY_OK &&
                   event == WEREWOLF_UI_KEY_EVENT_LONG &&
                   s_model.is_host && lobby_can_start()) {
            s_action_latched = true;
            render();
            s_feedback = WEREWOLF_UI_FEEDBACK_CONFIRMED;
            return emit_action(action, WEREWOLF_UI_ACTION_START_GAME,
                               WEREWOLF_UI_NO_SEAT);
        }
        break;

    case WEREWOLF_UI_PAGE_PLAYER_ACTION:
        if (!s_model.input_enabled || s_action_latched ||
            s_model.player_kicking) {
            break;
        }
        if (event == WEREWOLF_UI_KEY_EVENT_CLICK &&
            (key == WEREWOLF_UI_KEY_UP || key == WEREWOLF_UI_KEY_DOWN) &&
            lobby_guest_is_manageable(s_model.selected_seat)) {
            s_player_action_focus_kick = !s_player_action_focus_kick;
            render();
            s_feedback = WEREWOLF_UI_FEEDBACK_MOVE;
            break;
        }
        if (key == WEREWOLF_UI_KEY_DOWN &&
            event == WEREWOLF_UI_KEY_EVENT_LONG) {
            s_action_latched = true;
            render();
            s_feedback = WEREWOLF_UI_FEEDBACK_SELECT;
            return emit_action(action,
                               WEREWOLF_UI_ACTION_CLOSE_PLAYER_ACTION,
                               s_model.selected_seat);
        }
        if (key == WEREWOLF_UI_KEY_OK &&
            event == WEREWOLF_UI_KEY_EVENT_CLICK) {
            s_action_latched = true;
            render();
            s_feedback = WEREWOLF_UI_FEEDBACK_SELECT;
            return emit_action(action,
                               s_player_action_focus_kick &&
                                       lobby_guest_is_manageable(
                                           s_model.selected_seat)
                                   ? WEREWOLF_UI_ACTION_REQUEST_KICK_PLAYER
                                   : WEREWOLF_UI_ACTION_CLOSE_PLAYER_ACTION,
                               s_model.selected_seat);
        }
        break;

    case WEREWOLF_UI_PAGE_ROLE:
    case WEREWOLF_UI_PAGE_PRIVATE_RESULT:
        return handle_private_key(key, event, action);

    case WEREWOLF_UI_PAGE_NIGHT_SELECT:
    case WEREWOLF_UI_PAGE_NIGHT_CONFIRM:
        return handle_target_key(false, key, event, action);

    case WEREWOLF_UI_PAGE_VOTE_SELECT:
    case WEREWOLF_UI_PAGE_VOTE_CONFIRM:
        return handle_target_key(true, key, event, action);

    case WEREWOLF_UI_PAGE_SPEAKING:
        if (key == WEREWOLF_UI_KEY_OK &&
            event == WEREWOLF_UI_KEY_EVENT_LONG &&
            s_model.input_enabled && local_player_alive() &&
            s_model.local_seat == s_model.speaker_seat && !s_action_latched) {
            s_action_latched = true;
            render();
            s_feedback = WEREWOLF_UI_FEEDBACK_CONFIRMED;
            return emit_action(action, WEREWOLF_UI_ACTION_PASS_SPEECH,
                               s_model.local_seat);
        }
        break;

    case WEREWOLF_UI_PAGE_DAY_RESULT:
    case WEREWOLF_UI_PAGE_ELIMINATED:
        if (key == WEREWOLF_UI_KEY_OK &&
            event == WEREWOLF_UI_KEY_EVENT_CLICK &&
            werewolf_ui_action_gate_open(s_model.input_enabled,
                                         s_action_latched)) {
            s_action_latched = true;
            render();
            s_feedback = WEREWOLF_UI_FEEDBACK_SELECT;
            return emit_action(action, WEREWOLF_UI_ACTION_ACK_RESULT,
                               WEREWOLF_UI_NO_SEAT);
        }
        break;

    case WEREWOLF_UI_PAGE_GAME_OVER:
        if (key == WEREWOLF_UI_KEY_OK &&
            event == WEREWOLF_UI_KEY_EVENT_CLICK &&
            werewolf_ui_action_gate_open(s_model.input_enabled,
                                         s_action_latched)) {
            s_action_latched = true;
            render();
            s_feedback = WEREWOLF_UI_FEEDBACK_SELECT;
            return emit_action(action, WEREWOLF_UI_ACTION_LEAVE_GAME,
                               WEREWOLF_UI_NO_SEAT);
        }
        break;

    case WEREWOLF_UI_PAGE_ERROR:
        if (key == WEREWOLF_UI_KEY_OK &&
            event == WEREWOLF_UI_KEY_EVENT_CLICK && s_model.recoverable) {
            s_feedback = WEREWOLF_UI_FEEDBACK_SELECT;
            return emit_action(action, WEREWOLF_UI_ACTION_RETRY,
                               WEREWOLF_UI_NO_SEAT);
        }
        if (key == WEREWOLF_UI_KEY_DOWN &&
            event == WEREWOLF_UI_KEY_EVENT_LONG) {
            s_feedback = WEREWOLF_UI_FEEDBACK_CONFIRMED;
            return emit_action(action, WEREWOLF_UI_ACTION_LEAVE_GAME,
                               WEREWOLF_UI_NO_SEAT);
        }
        break;

    case WEREWOLF_UI_PAGE_ROOM_CLOSED:
        break;

    default:
        break;
    }
    return false;
}

werewolf_ui_feedback_t werewolf_ui_take_feedback(void)
{
    werewolf_ui_feedback_t feedback = s_feedback;

    s_feedback = WEREWOLF_UI_FEEDBACK_NONE;
    return feedback;
}

bool werewolf_ui_action_matches_model(const werewolf_ui_action_t *action,
                                      const werewolf_ui_model_t *model)
{
    if (!action || !model || action->type == WEREWOLF_UI_ACTION_NONE) {
        return false;
    }
    if (action->type == WEREWOLF_UI_ACTION_ROLE_SEEN) {
        return action->source_page == WEREWOLF_UI_PAGE_ROLE &&
               model->page == WEREWOLF_UI_PAGE_ROLE &&
               action->private_epoch == model->private_epoch;
    }
    if (action->type == WEREWOLF_UI_ACTION_ACK_RESULT &&
        action->source_page == WEREWOLF_UI_PAGE_PRIVATE_RESULT) {
        return model->page == WEREWOLF_UI_PAGE_PRIVATE_RESULT &&
               action->private_epoch == model->private_epoch;
    }
    if (action->type == WEREWOLF_UI_ACTION_SELECT_ROOM ||
        action->type == WEREWOLF_UI_ACTION_JOIN_SELECTED_ROOM) {
        if (action->source_page != WEREWOLF_UI_PAGE_ROOM_LIST ||
            model->page != WEREWOLF_UI_PAGE_ROOM_LIST ||
            action->room_token == 0U) {
            return false;
        }
        for (unsigned i = 0U; i < WEREWOLF_UI_ROOM_COUNT_MAX; ++i) {
            if (model->rooms[i].visible &&
                model->rooms[i].token == action->room_token) {
                return true;
            }
        }
        return false;
    }
    /* READY/profile heartbeats may refresh a lobby while a local management
     * key event is already queued. Keep those actions bound to the visible
     * page, exact seat, and progress state instead of dropping a safe action
     * solely due to an unrelated lobby revision. The controller still
     * revalidates the authoritative paired/occupied state before changing
     * anything. */
    if (action->type == WEREWOLF_UI_ACTION_OPEN_PLAYER_ACTION) {
        return action->source_page == WEREWOLF_UI_PAGE_LOBBY &&
               model->page == WEREWOLF_UI_PAGE_LOBBY && model->is_host &&
               action->seat > 0U &&
               action->seat < WEREWOLF_UI_PLAYER_COUNT &&
               !model->room_close_prompt;
    }
    if (action->type == WEREWOLF_UI_ACTION_CLOSE_PLAYER_ACTION ||
        action->type == WEREWOLF_UI_ACTION_REQUEST_KICK_PLAYER) {
        return action->source_page == WEREWOLF_UI_PAGE_PLAYER_ACTION &&
               model->page == WEREWOLF_UI_PAGE_PLAYER_ACTION &&
               model->is_host && action->seat == model->selected_seat &&
               !model->player_kicking;
    }
    if (action->type == WEREWOLF_UI_ACTION_REQUEST_CLOSE_ROOM) {
        return action->source_page == WEREWOLF_UI_PAGE_LOBBY &&
               model->page == WEREWOLF_UI_PAGE_LOBBY && model->is_host &&
               model->input_enabled && !model->room_close_prompt &&
               !model->room_closing;
    }
    if (action->type == WEREWOLF_UI_ACTION_CANCEL_CLOSE_ROOM ||
        action->type == WEREWOLF_UI_ACTION_CONFIRM_CLOSE_ROOM) {
        return action->source_page == WEREWOLF_UI_PAGE_LOBBY &&
               model->page == WEREWOLF_UI_PAGE_LOBBY && model->is_host &&
               model->input_enabled && model->room_close_prompt &&
               !model->room_closing;
    }
    return action->source_revision == model->revision;
}

void werewolf_ui_hide_private(void)
{
    if (!s_screen) {
        return;
    }
    bool needs_render = s_private_revealed;
    s_private_revealed = false;
    s_private_press_armed = false;
    s_private_press_epoch = 0U;
    if (needs_render) {
        render();
    }
}

void werewolf_ui_cancel_pending_action(void)
{
    if (!s_screen) {
        return;
    }
    s_action_latched = false;
    s_confirm_pending = false;
    s_close_focus_close = false;
    s_player_action_focus_kick = false;
    s_private_revealed = false;
    s_private_press_armed = false;
    s_private_press_epoch = 0U;
    render();
}

void werewolf_ui_destroy(void)
{
    if (!s_screen) {
        return;
    }
    lv_obj_delete(s_screen);
    s_screen = NULL;
    s_panel = NULL;
    s_moon_disc = NULL;
    s_moon_cutout = NULL;
    s_moon_horizon = NULL;
    s_title = NULL;
    s_subtitle = NULL;
    s_detail = NULL;
    s_timer = NULL;
    s_footer = NULL;
    s_banner = NULL;
    s_banner_label = NULL;
    s_header_phase = NULL;
    s_modal_overlay = NULL;
    s_modal_dialog = NULL;
    s_modal_rail = NULL;
    s_modal_title = NULL;
    s_modal_status = NULL;
    s_modal_body = NULL;
    memset(s_modal_options, 0, sizeof(s_modal_options));
    memset(s_modal_option_bars, 0, sizeof(s_modal_option_bars));
    memset(s_modal_option_labels, 0, sizeof(s_modal_option_labels));
    memset(s_header_link_nodes, 0, sizeof(s_header_link_nodes));
    memset(s_header_link_bridges, 0, sizeof(s_header_link_bridges));
    memset(s_header_signal_segments, 0, sizeof(s_header_signal_segments));
    memset(s_header_player_slots, 0, sizeof(s_header_player_slots));
    memset(s_header_player_fills, 0, sizeof(s_header_player_fills));
    memset(s_header_player_cuts, 0, sizeof(s_header_player_cuts));
    memset(s_header_battery_segments, 0,
           sizeof(s_header_battery_segments));
    memset(s_cells, 0, sizeof(s_cells));
    memset(s_cell_bars, 0, sizeof(s_cell_bars));
    memset(s_cell_labels, 0, sizeof(s_cell_labels));
    memset(s_lobby_seat_labels, 0, sizeof(s_lobby_seat_labels));
    memset(s_lobby_identity_badges, 0, sizeof(s_lobby_identity_badges));
    memset(s_lobby_identity_labels, 0, sizeof(s_lobby_identity_labels));
    memset(s_lobby_name_labels, 0, sizeof(s_lobby_name_labels));
    memset(s_lobby_state_labels, 0, sizeof(s_lobby_state_labels));
    s_confirm_pending = false;
    s_private_revealed = false;
    s_private_press_armed = false;
    s_private_press_page = WEREWOLF_UI_PAGE_MODE;
    s_private_press_epoch = 0U;
    s_action_latched = false;
    s_close_focus_close = false;
    s_player_action_focus_kick = false;
    s_feedback = WEREWOLF_UI_FEEDBACK_NONE;
    s_target_cursor = WEREWOLF_UI_NO_SEAT;
    s_lobby_cursor = WEREWOLF_UI_NO_SEAT;
    s_room_cursor_token = 0U;
}

lv_obj_t *werewolf_ui_screen(void)
{
    return s_screen;
}
