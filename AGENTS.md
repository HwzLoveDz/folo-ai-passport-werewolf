# Repository Guidelines

## Project Structure & Module Organization

This repository is the offline multiplayer Werewolf firmware for the ESP32-C3-based FoloToy AI Passport.

- `components/bsp/include/`: public BSP APIs and the hardware pin/configuration source of truth (`bsp_pins.h`).
- `components/bsp/src/`: display, button, audio, battery, and shared-I2C implementations.
- `main/`: product entry point, deterministic game core, pairing, payload codecs, ESP-NOW transport, controller, and LVGL UI.
- `tests/`: native host tests for rules, serialization, replay protection, and UI-independent helpers.
- `docs/MVP_RULES.md`: normative seven-player rules and privacy behavior.
- `docs/CONTROLS.md`: normative three-button semantics and page exceptions.
- `sdkconfig.defaults`: reproducible target, console, LVGL, and memory defaults.
- `README.md`: product overview, current progress, and the required on-device acceptance checklist.
- `docs/AI_HARDWARE_DEVELOPMENT_GUIDE.md`: hardware evidence boundaries, pin/configuration sources, known board traps, and physical validation methods.

Keep reusable hardware logic in `components/bsp`; keep rules independent from ESP-IDF and LVGL. Network callbacks may only enqueue bounded events. The game task is the sole writer of authoritative state.

## Product Software Boundary

- This is an independent Werewolf product, not an extension of the official
  hardware demo. Boot must enter `werewolf_app` directly; no demo launcher,
  Radar screen, upstream navigation, upstream business state, or upstream UI
  asset may enter the product image.
- Official/upstream firmware is evidence for hardware behavior only: confirmed
  pin mapping, panel/controller initialization, ADC key ladder, shared I2C,
  codec, battery gauge, and other BSP details. Re-verify copied behavior against
  this board and keep it below the application boundary.
- The current product-owned visual baseline is **Eclipse Ledger**: near-black
  space, lunar rings and cells, warm parchment text, restrained amber/teal/red
  state colors, Kode Mono, and integer-pixel layout. The Gesture Wand repository
  is only an early workflow/layout reference; do not restore its FUI screens or
  import gesture, PIN, trace, BLE-link, or other Wand business logic.
- Firmware work follows the installed `harvey-embedded-engineering` workflow:
  inspect the actual repository/board first, preserve dirty state and build
  caches, render deterministic production-UI screenshots before firmware
  builds, and report host, build, flash, and physical-device evidence as
  separate proof levels.
- UI changes must compile `main/werewolf_ui.c` in `simulator/`; a second mock UI
  is not acceptable. Candidate renders stay separate from the tracked
  `current-*` visual baseline until reviewed. Simulator approval and physical
  panel validation are separate evidence levels.
- Building never implies permission to flash. Re-enumerate the live port and
  obtain explicit write approval before each device-write step. Erase, eFuse,
  key, or security-state changes need their own explicit authorization.

## Game Privacy and Persistence

- Broadcast carries only discovery and the pre-LMK commit/reveal handshake. Never broadcast roles, teammates, targets, votes, Seer results, resume credentials, shared secrets, or LMKs.
- Release logs must not print any secret game state. Tests should assert that public views cannot expose private fields.
- Do not hard-code production PMK/LMK material. Refuse to start encrypted gameplay when caller-provided key material is absent.
- Never call `nvs_flash_erase()` as automatic recovery. NVS may contain unrelated user state; report the error and require an explicit recovery workflow.
- Night traffic, prompts, timing, brightness and sound must not reveal which player has an active role.
- Protocol/rules v5 may start only with seven occupied seats, seven echoed
  profiles, seven local `READY` states, and six installed Host-to-client
  encrypted links.
- The six-digit `VERIFY` code is locally derived and read-only: show it below
  `ROOM` on a guest and in the selected guest detail on the Host. Never transmit
  the code, add a machine confirmation action/state, or include it in `READY`
  or START. A mismatch is handled by guest leave or Host kick. Commitment
  binding, X25519, per-peer LMKs and encrypted unicast protect the selected
  transcript; physical identity depends on people actually comparing matching
  displays. Nicknames and `H`/`Y`/`G` badges are display metadata only.

## Build, Test, and Development Commands

Use ESP-IDF 5.5.3:

```bash
source /path/to/esp-idf-v5.5.3/export.sh  # Replace with the local install path
idf.py set-target esp32c3                 # Fresh checkout or target change only
idf.py build                              # Compile and inspect warnings/size
idf.py flash monitor                      # Only with explicit device-write permission
```

Preserve `build/`, managed-component caches, and local configuration for
incremental work. Run `idf.py fullclean` only when there is evidence of a
damaged cache or configuration contamination.

Run `./tests/run_host_tests.sh` before the ESP-IDF build. Treat a clean `idf.py build` as the minimum firmware check, then run every applicable item in the README acceptance checklist on seven real devices.

## Coding Style & Naming Conventions

Write C using four-space indentation and K&R-style braces, following nearby files. Use `snake_case` for functions and locals, `BSP_*` for public hardware constants, and `s_` for file-local state. Keep BSP APIs prefixed with `bsp_`. Prefer `static` for internal symbols. UI text stays English; explanatory comments may be Chinese. Preserve comments documenting hardware-specific register values and memory constraints.

## Testing Guidelines

Before submitting, run the native test suite, build from the repository root, and inspect warnings. On hardware, validate discovery, matching locally derived `VERIFY` displays without on-wire code or confirmation state, all seven occupied/profile/ready states, six encrypted client links, `H`/`Y`/`G` badges, self readiness, targeted kick, all phase transitions, private hold-to-reveal, release-to-seal without submission, repeated review, separate short-OK completion, reconnect/abort behavior, and a complete seven-player game. For pin, display-rotation, ADC, Wi-Fi, or DMA changes, explicitly record the observed hardware result in the PR. Do not increase LVGL or network allocations without checking ESP32-C3 internal RAM usage; the board has no PSRAM.

## Commit & Pull Request Guidelines

History follows Conventional Commit-style subjects such as `feat(bsp): ...`, `feat(game): ...`, `feat(ui): ...`, `fix(bsp): ...`, and `docs: ...`. Keep commits focused by subsystem. Pull requests should explain the hardware/revision tested, summarize behavior changes, list build and on-device results, and include photos or screenshots for display changes. Link related issues and call out wiring, pin-map, or compatibility impacts.
