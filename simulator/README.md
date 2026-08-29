# Werewolf production UI simulator

This host renderer compiles the real `main/werewolf_ui.c`, its security text
helpers, and the same embedded Kode Mono font objects used by the firmware. It
does not duplicate or approximate the device UI.

```bash
WEREWOLF_UI_OUTPUT_DIR=/tmp/werewolf-ui-candidate \
    bash simulator/preview.sh
```

The cached build defaults to `/tmp/werewolf-ui-simulator-build`. Output is a
deterministic set of 240 x 320 RGB565 PNGs.

The primary lobby and input candidates are:

- `lobby-host-self-wait` / `lobby-host-self-ready`: Host focus starts on the
  local seat and short OK emits `TOGGLE_READY`.
- `lobby-host-guest-focus`: UP/DOWN traverses occupied seats including the
  Host, and short OK on a guest emits `OPEN_PLAYER_ACTION`.
- `player-detail-back` / `player-detail-kick`: show the same read-only
  six-digit comparison code as the guest device and an explicit two-item
  BACK/KICK PLAYER menu. BACK is the entry focus; UP/DOWN moves focus, short
  OK emits CLOSE or the direct KICK request, and long DOWN always emits CLOSE.
- `player-kicking`: keeps the reliable removal progress overlay input-locked;
  there is no second KICK confirmation prompt.
- `lobby-host-all-ready`: all seven seats are READY and long OK emits START.
- `lobby-guest-wait` / `lobby-guest-ready`: the read-only comparison code is
  visible below ROOM, while short OK toggles the local guest.
- `room-list`, `night-confirm`, and `error-recoverable`: long DOWN is the
  common back, cancel, or exit gesture. Long OK is never used as Back.
- `lobby-host-exit-back` / `lobby-host-exit-close` keep the destructive CLOSE
  ROOM confirmation paired with a visible BACK option. Room-closed notices
  remain single-OK acknowledgements, while close/leave/kick progress overlays
  stay input-locked instead of inventing a selectable BACK.

The production lobby itself renders the `H`, `Y`, and `G` identity badges; the
simulator does not paint substitute labels. The event smoke checks run before
each named frame is written, so a screenshot is produced only if its expected
action type and seat/token scope match. Host player-management actions are
also checked across an unrelated revision refresh: OPEN, KICK request, and
CLOSE remain valid, while a changed page, target seat, or locked progress
state rejects them.

The comparison code has no VERIFY action, confirmed state, or start-game gate.
It is rendered only for optional in-person comparison; READY remains the only
per-player lobby state.

The full preview also performs exact PNG comparisons proving that different
secrets have an identical sealed frame, release restores that frame, and
unmatched or stale long-press events stay sealed. The checks also prove that a
routine UI heartbeat does not cancel a hold inside the same private gate epoch.
Controller-queue reordering is also modeled: a confirmation remains current
if a same-gate heartbeat is processed first, but is rejected after the private
gate epoch changes.

These images are candidates, not an approved `current-*` baseline. Promote a
baseline only after visual review on both the simulator and physical panel.
