# AI-PASSPORT Werewolf MVP Rules

This file is the normative ruleset for the first seven-device build. The game
core, UI and network protocol must not silently introduce alternative rules.

## Players and roles

- Exactly seven seats are required to start.
- The deck contains two Wolves, one Seer, one Guard and three Villagers.
- Roles are shuffled from an explicit 64-bit seed so host tests can reproduce
  a deck deterministically. The MVP does not persist or export a real-game
  event log because that log would also need a separate privacy design.
- A player sees only their own role. Wolves additionally see the other Wolf's
  seat. The Seer sees faction only, never the exact good role.

## Lobby and start

- Protocol/rules v5 starts only with seven occupied seats, seven echoed
  profiles, seven players in `READY`, and six installed Host-to-client
  encrypted links.
- Each player changes only their own `WAIT/READY` state. The Host cannot mark a
  guest ready.
- In the player list, `H` marks the Host, `Y` marks the viewing guest's own row,
  and `G` marks another guest. Names and badges are presentation metadata, not
  authorization identities.
- Each Host-client link has a six-digit read-only `VERIFY` code derived locally
  from the bound handshake and key material. The guest sees it below `ROOM`;
  the Host sees it in that guest's selected detail view. The code is not sent
  over the air, cannot be confirmed in software, and creates no stored
  confirmation state or start requirement. A mismatch is handled by guest
  leave or Host kick.
- v5 retains commitment binding, X25519 key agreement, per-peer LMKs and
  encrypted unicast. A matching code can assist an in-person comparison, but
  without that human comparison the protocol does not authenticate the person
  holding a nearby device.

## Win conditions

- Good wins immediately when no Wolf remains alive.
- Wolves win immediately when living Wolves are at least as numerous as all
  other living players.
- Win conditions are checked after dawn resolution and after exile resolution.

## Night

All living players receive the same interaction shape and submit one fixed-size
action. Villagers submit a dummy target so message size and visible button
activity do not disclose a role. The MVP displays target durations but does not
automatically expire a phase; it waits for every required action.

- Each Wolf selects a living non-Wolf target.
- If only one Wolf remains alive, that Wolf's valid target is the kill candidate.
- If both Wolves select the same target, that seat is the kill candidate.
- If they disagree, every living player receives the same reselection UI and
  submits another fixed-size action. If the Wolves still disagree, the Wolves
  make no kill that night.
- The Guard may protect any living player, including themself, but may not
  protect the same seat on consecutive nights.
- The Seer selects any other living player and privately receives Wolf or Good.
- Guard protection cancels the Wolf kill. Death is announced by seat only; the
  role remains hidden until the end-of-game role review.

## Day and vote

- Living players speak in seat order. The UI shows a target duration and the
  current speaker passes manually; this MVP has no automatic countdown or
  timeout adjudication. Speech is not recorded or analysed.
- Each living player privately votes for one other living player.
- A unique top vote exiles that player.
- A first-place tie starts one defence/revote round restricted to tied seats.
- If the revote is tied, nobody is exiled that day.
- Dead players retain the public view but cannot act, speak through the timer,
  vote or inspect any additional secret.

## Input and privacy

- Short `UP/DOWN` navigates, short `OK` performs the focused primary action,
  long `OK` confirms a clearly presented consequential action or reveals
  private information, and long `DOWN` means back/cancel/exit request. The
  single dangerous short-OK exception is the explicit red `KICK PLAYER` item
  in a target player's detail menu, whose default focus is `BACK`. See
  `docs/CONTROLS.md` for page-specific behavior.
- In a joined client lobby, long `DOWN` immediately starts the reliable leave
  request; there is no second leave dialog. The link remains alive while the
  request drains to ACK or its bounded deadline.
- Hold OK to reveal a private role; releasing OK hides it immediately.
- UP/DOWN changes a game target. Submitting that target requires an explicit
  hold to confirm; a click never commits a game action. Direct single-player
  kick remains the documented detail-menu exception above.
- Private phases use identical brightness, animation, UI structure and sound
  policy for all roles. No role-specific sound, vibration or backlight pattern
  is permitted. Actual wall-clock duration depends on player input in this MVP.
- Room discovery may use broadcast. Roles, teammates, targets, votes and Seer
  results must use encrypted unicast and must never appear in release logs.

## Failure policy

- Bounded retries and encrypted heartbeat snapshots recover ordinary packet
  loss while every device remains in the same runtime session.
- Client restart/resume and host restart/checkpoint recovery are not implemented
  in this MVP. Either event aborts the current game and requires a new room.
- Permanent loss of the host aborts the MVP game. Automatic host migration is
  explicitly outside this release.
- An invalid, stale, duplicate or out-of-phase action never advances the game.
