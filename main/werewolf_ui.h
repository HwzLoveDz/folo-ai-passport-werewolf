#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "werewolf_nickname.h"
#include "werewolf_ui_text.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The MVP rules require exactly seven seats. Keeping every field fixed-size
 * makes a UI snapshot safe to pass between tasks without heap ownership. */
#define WEREWOLF_UI_PLAYER_COUNT       7
#define WEREWOLF_UI_ROOM_COUNT_MAX     8
#define WEREWOLF_UI_NO_SEAT            UINT8_MAX
#define WEREWOLF_UI_PLAYER_NAME_MAX    WEREWOLF_NICKNAME_CAPACITY
#define WEREWOLF_UI_HEADLINE_MAX       32
#define WEREWOLF_UI_DETAIL_MAX         80
#define WEREWOLF_UI_PRIVATE_DETAIL_MAX 56

typedef enum {
    WEREWOLF_UI_MODE_CREATE = 0,
    WEREWOLF_UI_MODE_JOIN,
} werewolf_ui_mode_t;

typedef enum {
    WEREWOLF_UI_ROLE_UNKNOWN = 0,
    WEREWOLF_UI_ROLE_WOLF,
    WEREWOLF_UI_ROLE_SEER,
    WEREWOLF_UI_ROLE_GUARD,
    WEREWOLF_UI_ROLE_VILLAGER,
} werewolf_ui_role_t;

typedef enum {
    WEREWOLF_UI_FACTION_UNKNOWN = 0,
    WEREWOLF_UI_FACTION_GOOD,
    WEREWOLF_UI_FACTION_WOLVES,
} werewolf_ui_faction_t;

typedef enum {
    WEREWOLF_UI_WINNER_NONE = 0,
    WEREWOLF_UI_WINNER_GOOD,
    WEREWOLF_UI_WINNER_WOLVES,
    WEREWOLF_UI_WINNER_ABORTED,
} werewolf_ui_winner_t;

typedef enum {
    WEREWOLF_UI_ERROR_NONE = 0,
    WEREWOLF_UI_ERROR_ROOM_NOT_FOUND,
    WEREWOLF_UI_ERROR_ROOM_FULL,
    WEREWOLF_UI_ERROR_TIMEOUT,
    WEREWOLF_UI_ERROR_PROTOCOL,
    WEREWOLF_UI_ERROR_HARDWARE,
    WEREWOLF_UI_ERROR_HOST_LOST,
} werewolf_ui_error_t;

typedef enum {
    WEREWOLF_UI_PAGE_MODE = 0,
    WEREWOLF_UI_PAGE_ROOM_LIST,
    WEREWOLF_UI_PAGE_LOBBY,
    WEREWOLF_UI_PAGE_PLAYER_ACTION,
    WEREWOLF_UI_PAGE_ROLE,
    WEREWOLF_UI_PAGE_NIGHT_SELECT,
    WEREWOLF_UI_PAGE_NIGHT_CONFIRM,
    WEREWOLF_UI_PAGE_PRIVATE_RESULT,
    WEREWOLF_UI_PAGE_DAY_RESULT,
    WEREWOLF_UI_PAGE_SPEAKING,
    WEREWOLF_UI_PAGE_VOTE_SELECT,
    WEREWOLF_UI_PAGE_VOTE_CONFIRM,
    WEREWOLF_UI_PAGE_ELIMINATED,
    WEREWOLF_UI_PAGE_GAME_OVER,
    WEREWOLF_UI_PAGE_ERROR,
    WEREWOLF_UI_PAGE_ROOM_CLOSED,
} werewolf_ui_page_t;

typedef enum {
    WEREWOLF_UI_KEY_UP = 0,
    WEREWOLF_UI_KEY_DOWN,
    WEREWOLF_UI_KEY_OK,
} werewolf_ui_key_t;

/* RELEASE is deliberately explicit. The role screen cannot meet its privacy
 * contract if the input adapter only forwards CLICK/LONG events. */
typedef enum {
    WEREWOLF_UI_KEY_EVENT_PRESS = 0,
    WEREWOLF_UI_KEY_EVENT_RELEASE,
    WEREWOLF_UI_KEY_EVENT_CLICK,
    WEREWOLF_UI_KEY_EVENT_LONG,
} werewolf_ui_key_event_t;

/* Pull-based feedback keeps codec work out of the LVGL/key path.  The input
 * adapter takes one value after releasing the display lock and queues the
 * corresponding non-blocking sound.  Only the all-player ROLE reveal/seal may
 * emit private-screen feedback; PRIVATE_RESULT and all NIGHT target input stay
 * silent so role and submission timing cannot leak. */
typedef enum {
    WEREWOLF_UI_FEEDBACK_NONE = 0,
    WEREWOLF_UI_FEEDBACK_MOVE,
    WEREWOLF_UI_FEEDBACK_SELECT,
    WEREWOLF_UI_FEEDBACK_READY_ON,
    WEREWOLF_UI_FEEDBACK_READY_OFF,
    WEREWOLF_UI_FEEDBACK_PRIVATE_REVEAL,
    WEREWOLF_UI_FEEDBACK_PRIVATE_SEAL,
    WEREWOLF_UI_FEEDBACK_CONFIRM_ARMED,
    WEREWOLF_UI_FEEDBACK_CONFIRMED,
} werewolf_ui_feedback_t;

typedef struct {
    uint8_t seat; /* Internal game/player ID 0..6; UI renders this as 1..7. */
    bool occupied;
    bool ready;
    bool alive;
    /* Frozen public knowledge for the persistent header strip.  This may
     * intentionally remain true after authoritative night resolution until
     * the following public DAWN gate. */
    bool publicly_alive;
    bool eligible;
    werewolf_ui_role_t role; /* Used only on GAME_OVER. */
    char name[WEREWOLF_UI_PLAYER_NAME_MAX];
} werewolf_ui_player_t;

typedef struct {
    bool visible;
    uint32_t token;
    uint8_t occupied_count;
    char code[WEREWOLF_UI_ROOM_CODE_MAX];
} werewolf_ui_room_t;

/* This is a presentation snapshot. The game/network layer owns validation,
 * phase transitions, timers and secrets, then publishes only what this device
 * is allowed to render. Do not put keys, packets or game object pointers here. */
typedef struct {
    uint32_t revision;
    /* Changes only when the private gate/content context changes. A routine
     * heartbeat may advance revision without invalidating an in-progress
     * hold-to-reveal gesture. */
    uint32_t private_epoch;
    werewolf_ui_page_t page;
    werewolf_ui_mode_t mode;
    werewolf_ui_connection_t connection;
    werewolf_ui_error_t error;
    werewolf_ui_winner_t winner;
    werewolf_ui_public_phase_t public_phase;
    werewolf_ui_battery_state_t battery_state;
    werewolf_ui_signal_t signal;

    bool is_host;
    bool game_started;
    bool local_ready;
    bool can_start;
    bool input_enabled;
    bool recoverable;
    /* Controller-owned modal state. It survives lobby network/model refreshes
     * and prevents UI-only flags from being lost when a peer changes READY. */
    bool room_close_prompt;
    bool room_closing;
    bool leaving_room;
    bool waiting_for_players;
    bool player_kicking;
    /* Locally derived, read-only code for optional in-person comparison.
     * It is never a machine-side confirmation or start-game gate. */
    bool has_verify_code;

    uint8_t battery_soc;
    uint8_t local_seat;
    uint8_t selected_seat;
    uint8_t affected_seat;
    uint8_t speaker_seat;
    uint8_t private_seat;
    uint8_t kick_seat;
    uint8_t round;
    uint16_t guide_seconds;
    uint8_t votes_received;
    uint8_t votes_expected;
    uint32_t verify_code;
    uint32_t selected_room_token;

    werewolf_ui_role_t local_role;
    werewolf_ui_faction_t private_faction;
    char local_name[WEREWOLF_UI_PLAYER_NAME_MAX];
    char room_code[WEREWOLF_UI_ROOM_CODE_MAX];
    char headline[WEREWOLF_UI_HEADLINE_MAX];
    char detail[WEREWOLF_UI_DETAIL_MAX];
    /* Role reminder / Wolf teammate. Seer result uses the typed faction above,
     * so this UI cannot accidentally disclose an exact good role. */
    char private_detail[WEREWOLF_UI_PRIVATE_DETAIL_MAX];
    werewolf_ui_player_t players[WEREWOLF_UI_PLAYER_COUNT];
    werewolf_ui_room_t rooms[WEREWOLF_UI_ROOM_COUNT_MAX];
} werewolf_ui_model_t;

typedef enum {
    WEREWOLF_UI_ACTION_NONE = 0,
    WEREWOLF_UI_ACTION_CREATE_ROOM,
    WEREWOLF_UI_ACTION_JOIN_ROOM,
    WEREWOLF_UI_ACTION_SELECT_ROOM,
    WEREWOLF_UI_ACTION_JOIN_SELECTED_ROOM,
    WEREWOLF_UI_ACTION_TOGGLE_READY,
    WEREWOLF_UI_ACTION_OPEN_PLAYER_ACTION,
    WEREWOLF_UI_ACTION_CLOSE_PLAYER_ACTION,
    WEREWOLF_UI_ACTION_REQUEST_KICK_PLAYER,
    WEREWOLF_UI_ACTION_START_GAME,
    WEREWOLF_UI_ACTION_ROLE_SEEN,
    WEREWOLF_UI_ACTION_SUBMIT_NIGHT_TARGET,
    WEREWOLF_UI_ACTION_PASS_SPEECH,
    WEREWOLF_UI_ACTION_SUBMIT_VOTE,
    WEREWOLF_UI_ACTION_ACK_RESULT,
    WEREWOLF_UI_ACTION_RETRY,
    WEREWOLF_UI_ACTION_LEAVE_GAME,
    WEREWOLF_UI_ACTION_REQUEST_CLOSE_ROOM,
    WEREWOLF_UI_ACTION_CANCEL_CLOSE_ROOM,
    WEREWOLF_UI_ACTION_CONFIRM_CLOSE_ROOM,
    WEREWOLF_UI_ACTION_ACK_ROOM_CLOSED,
} werewolf_ui_action_type_t;

typedef struct {
    werewolf_ui_action_type_t type;
    uint32_t source_revision;
    uint32_t private_epoch;
    werewolf_ui_page_t source_page;
    werewolf_ui_mode_t mode;
    uint8_t seat;
    uint32_t room_token;
} werewolf_ui_action_t;

void werewolf_ui_model_init(werewolf_ui_model_t *model);

/* LVGL ownership contract:
 * - create/set_model/handle_key/hide_private/destroy must run while the caller
 *   owns bsp_lvgl_lock(). The module does not lock internally, preventing a
 *   recursive BSP mutex deadlock.
 * - create() builds and loads the 240 x 320 screen.
 * - handle_key() never calls game/network code. It returns a value action for
 *   the caller to enqueue after bsp_lvgl_unlock().
 * - map the physical OK press-up edge to KEY_EVENT_RELEASE. That edge
 *   immediately hides private information but does not acknowledge it; after
 *   a valid reveal, a new short OK press emits the private confirmation. */
bool werewolf_ui_create(const werewolf_ui_model_t *initial_model);
void werewolf_ui_set_model(const werewolf_ui_model_t *model);
bool werewolf_ui_handle_key(werewolf_ui_key_t key,
                            werewolf_ui_key_event_t event,
                            werewolf_ui_action_t *action);
werewolf_ui_feedback_t werewolf_ui_take_feedback(void);
/* Private confirmations and page/seat-bound Host player-management actions
 * remain valid across an unrelated same-context revision refresh. Other
 * actions keep strict rendered-revision matching. */
bool werewolf_ui_action_matches_model(const werewolf_ui_action_t *action,
                                      const werewolf_ui_model_t *model);
void werewolf_ui_hide_private(void);
/* Roll back UI-only latches when the controller queue rejected an action. */
void werewolf_ui_cancel_pending_action(void);
void werewolf_ui_destroy(void);
lv_obj_t *werewolf_ui_screen(void);

#ifdef __cplusplus
}
#endif
