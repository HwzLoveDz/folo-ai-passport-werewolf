# Mote Werewolf

[简体中文](README.md) | **English**

**Mote Werewolf** is an offline seven-device Werewolf game for FoloToy AI Passport. It boots directly into the game and needs no phone, router, or cloud service. One device creates the room and remains the Host; six Guests join over ESP-NOW.

The current MVP implements room discovery, secure pairing, readiness, role assignment, night actions, dawn, ordered discussion, secret voting, tie revotes, victory evaluation, and the final reveal. A deterministic C state machine owns the rules. Future AI features may narrate the game, but never decide roles, votes, or winners.

## UI Preview

These screens are rendered directly from the production LVGL source and cover room setup, player management, and the complete game flow.

| Mode Select | All Ready | Player Detail |
| :---: | :---: | :---: |
| <img src="simulator/out/current-mode-create.png" width="200" alt="Mote Werewolf mode selection screen"> | <img src="simulator/out/current-lobby-host-all-ready.png" width="200" alt="Mote Werewolf all-ready lobby"> | <img src="simulator/out/current-player-detail-kick.png" width="200" alt="Mote Werewolf player detail and kick action"> |
| Role Reveal | Night Action | Final Review |
| <img src="simulator/out/current-role-revealed.png" width="200" alt="Mote Werewolf private role reveal screen"> | <img src="simulator/out/current-night-select.png" width="200" alt="Mote Werewolf night target selection screen"> | <img src="simulator/out/current-game-over.png" width="200" alt="Mote Werewolf final role review screen"> |

## Current features

- Room creation, stable room-directory selection, secure pairing, readiness, the complete game-phase flow, and final reveal.
- A persistent header for local identity, public phase/round, connection topology, live ESP-NOW signal strength, seven-seat state, and segmented CW2017 battery state.
- Non-blocking local cues through ES8311. Audio faults degrade to silence without affecting the rule state machine or adding a permanent status-bar error label.
- Nicknames, `H/Y/G` badges, and locally derived read-only `VERIFY` codes help players identify the intended in-person link without putting the code on air.

## Rules

- Exactly seven players: two Werewolves, one Seer, one Guard, and three Villagers.
- Werewolves jointly choose a night target. They receive one reselection after disagreement; a second disagreement means no attack that night.
- The Guard may protect themselves but cannot protect the same seat on consecutive nights.
- The Seer checks one player per night and sees only Werewolf or Good.
- Living players speak in seat order and vote secretly. A first-place tie triggers discussion and a revote; another tie eliminates nobody.
- Good wins when every Werewolf is eliminated. Werewolves win when the number of living Werewolves is at least the number of all other living players.
- Roles of eliminated players stay private until the game ends.

The normative rules are in [docs/MVP_RULES.md](docs/MVP_RULES.md). See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for protocol and security design, [docs/CONTROLS.md](docs/CONTROLS.md) for the three-button interaction model, and [docs/ROADMAP.md](docs/ROADMAP.md) for delivery stages.

## Lobby and controls

- Mode page: select Create or Join with `UP/DOWN`, then press `OK`.
- Room directory: select a visible room with `UP/DOWN`, press `OK` to join, or hold `DOWN` to return. The device never auto-joins the first beacon.
- Lobby: selecting your own row and pressing `OK` toggles `WAIT/READY`. The Host may select a Guest, inspect the read-only verification code, or kick that Guest directly.
- Badges: `H` marks the Host, `Y` marks the current Guest, and `G` marks another Guest. They are display metadata, not authentication.
- Each Guest shows a six-digit read-only `VERIFY` code below `ROOM`. The Host sees the matching code in that Guest's detail page. Players compare the two displays in person; the code is never transmitted and has no machine-confirmation state.
- Protocol/rules v5 starts only when all seven seats are occupied, seven profiles are echoed, all seven players are `READY`, and the Host has six encrypted Guest links.
- A Guest holds `DOWN` to leave reliably. The Host holds `DOWN` to open the close-room warning; `BACK` is selected by default and short `OK` returns to the Lobby. Closing the room requires selecting `CLOSE ROOM` and holding `OK`.
- Private pages reveal a role or Seer result only while `OK` is held and hide it immediately on release.
- Target and vote pages use `UP/DOWN` to select, short `OK` to enter confirmation, and held `OK` to submit.

Nicknames contain at most ten printable ASCII characters; extra characters are truncated. Duplicate nicknames remain distinguishable by seat number. Nicknames, seats, and `H/Y/G` badges do not authenticate physical identity.

## Security and reliability

- Only discovery and the pre-LMK commitment/reveal handshake use broadcast. Acceptance, readiness, roles, actions, votes, and results use encrypted ESP-NOW unicast.
- Each pairing attempt receives fresh X25519 material and nonces. Transcript-bound HKDF derives a unique LMK for each Host-to-Guest link and a room-derived PMK.
- `VERIFY` is derived locally from the existing transcript and key material. It is not sent in packets, persisted as a confirmation state, or used by `READY` and START gates.
- The start gate fails closed unless all seats, profiles, readiness states, and encrypted links are complete.
- Transport includes acknowledgements, bounded retries, a 32-packet replay window, idempotency keys, and encrypted heartbeat snapshots.
- Guest leave, Host close, targeted kick, and abnormal Host termination use bounded reliable termination before session teardown.
- Serial logs exclude roles, targets, votes, keys, and Seer results. NVS initialization failures never trigger an automatic erase.
- Host migration and restart recovery are outside the current MVP. Permanent Host loss or device restart aborts the game.

## Build and test

Target: ESP-IDF 5.5.3 on ESP32-C3.

```bash
source ../esp-idf-v5.5.3/export.sh
idf.py set-target esp32c3
idf.py build
```

Do not treat `idf.py flash` as an ordinary build step. Before writing a device, verify its actual port, partition table, and security state, and obtain explicit authorization for that write.

Run deterministic host tests without ESP-IDF:

```bash
./tests/run_host_tests.sh
```

Render every production-UI state directly from the LVGL source:

```bash
WEREWOLF_UI_OUTPUT_DIR=/tmp/werewolf-ui-candidate \
    bash simulator/preview.sh
```

The simulator verifies the complete public state set and privacy invariants. A simulator render is a candidate until the physical display is reviewed.

## Project layout

```text
main/werewolf_game.*            deterministic authoritative game state machine
main/werewolf_identity.*        NVS nickname identity and per-device factory seed
main/werewolf_nickname.*        ten-character nickname normalization
main/werewolf_protocol.*        fixed-endian ESP-NOW framing
main/werewolf_net.*             encrypted peers, ACK, retry, deduplication, heartbeat
main/werewolf_pairing.*         commitment/reveal, X25519, HKDF, LMK/PMK, VERIFY
main/werewolf_messages.*        lobby and game payload codecs
main/werewolf_lobby.*           pure lobby validation and fail-closed start gate
main/werewolf_room_directory.*  bounded discovery and stable room selection
main/werewolf_termination.*     reliable termination targets and deadlines
main/werewolf_app.*             Host and Guest session controller
main/werewolf_power.*           background CW2017 telemetry
main/werewolf_sound.*           non-blocking ES8311 cue queue
main/werewolf_ui.*              240 x 320 three-button LVGL interface
components/bsp/                 display, buttons, audio, battery, and shared-I2C BSP
simulator/                      deterministic renderer using production UI code
tests/                          native Linux host tests
partitions.csv                  4 MiB-compatible 2 MiB factory layout without OTA
```

## Hardware acceptance

A successful build is not physical validation. Complete acceptance requires:

1. One-device boot, button-edge, private-screen release, stack, and heap checks.
2. Three-device room selection, matching locally derived `VERIFY` displays, badge/readiness behavior, targeted kick, leave/close flows, encrypted-peer count, packet-loss retry, and Host-loss abort checks.
3. A full seven-device game covering every role, Werewolf reselection, consecutive-Guard restriction, tie revotes, and both victory paths.
4. Serial and over-the-air evidence that secrets never enter logs or broadcast traffic, plus measured discovery range and packet-loss behavior.

The displayed phase durations are guidance only. Automatic countdown/timeout advancement and AI narration are planned work, not completed features.
