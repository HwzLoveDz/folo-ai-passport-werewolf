#!/usr/bin/env bash
set -euo pipefail

SIM_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${WEREWOLF_UI_BUILD_DIR:-/tmp/werewolf-ui-simulator-build}"
OUTPUT_DIR="${WEREWOLF_UI_OUTPUT_DIR:-${SIM_DIR}/out}"
SIMULATOR="${BUILD_DIR}/werewolf-ui-sim"

if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    cmake -S "${SIM_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
fi
cmake --build "${BUILD_DIR}" --parallel "$(nproc)"
mkdir -p "${OUTPUT_DIR}"

states=(
    mode-create mode-join connection-scanning connection-pairing
    room-list-empty room-list
    lobby-host-self-wait lobby-host-self-ready
    lobby-host-guest-focus player-detail-back player-detail-kick
    lobby-host-all-ready
    lobby-guest-wait lobby-guest-ready
    player-kicking
    lobby-host-exit-back lobby-host-exit-close
    lobby-host-exit-returned lobby-host-exit-return-reference
    lobby-host-closing
    lobby-nickname-limit lobby-nickname-exact
    room-closed-guest room-closed-from-wolf room-closed-from-seer
    kicked-guest kicked-from-wolf kicked-from-seer
    role-sealed-wolf role-sealed-seer role-revealed
    role-revealed-villager role-revealed-guard
    role-release role-review-ready-villager role-review-again-villager
    role-confirm-villager role-click-before-view role-trailing-click
    role-long-unarmed role-heartbeat role-private-epoch-changed
    role-heartbeat-release role-private-epoch-release
    night-select night-confirm night-waiting night-known-dead
    private-sealed-wolf private-sealed-good private-good
    private-sealed-good-no-pending private-good-no-pending
    private-sealed-good-no-known-dead
    private-sealed-good-local-pending private-sealed-good-no-local-pending
    private-no-result private-review-ready private-review-again
    private-confirm private-click-before-view private-trailing-click
    private-heartbeat private-epoch-changed private-waiting
    day-result speaking vote-select vote-confirm eliminated game-over
    status-self-dead status-self-host-dead reconnecting connection-disconnected
    connection-host-lost error-recoverable error error-connection
    error-protocol error-hardware
    error-host-clean error-host-close-stale
    status-link-online status-link-scanning status-link-pairing
    status-signal-none status-signal-weak status-signal-fair
    status-signal-good status-signal-strong status-signal-stale
    status-signal-disconnected status-signal-disconnected-cached-strong
    status-battery-unavailable status-battery-low status-battery-critical
    status-battery-stale status-battery-full status-battery-zero
)

for state in "${states[@]}"; do
    "${SIMULATOR}" --state "${state}" \
        --output "${OUTPUT_DIR}/candidate-${state}.png"
done

# The interaction candidates must visibly distinguish local readiness, Host
# focus movement, and the BACK/KICK choices of the direct player menu.
if cmp -s "${OUTPUT_DIR}/candidate-lobby-host-self-wait.png" \
          "${OUTPUT_DIR}/candidate-lobby-host-self-ready.png" ||
   cmp -s "${OUTPUT_DIR}/candidate-lobby-guest-wait.png" \
          "${OUTPUT_DIR}/candidate-lobby-guest-ready.png" ||
   cmp -s "${OUTPUT_DIR}/candidate-lobby-host-self-wait.png" \
          "${OUTPUT_DIR}/candidate-lobby-host-guest-focus.png" ||
   cmp -s "${OUTPUT_DIR}/candidate-lobby-host-guest-focus.png" \
          "${OUTPUT_DIR}/candidate-player-detail-back.png" ||
   cmp -s "${OUTPUT_DIR}/candidate-player-detail-back.png" \
          "${OUTPUT_DIR}/candidate-player-detail-kick.png"; then
    printf 'Lobby/player-menu interaction states rendered identically\n' >&2
    exit 1
fi

# Cancelling the Host close prompt returns to an ordinary, fully interactive
# lobby. Its rendered frame must not retain ACTION SENT / WAIT or modal state.
cmp "${OUTPUT_DIR}/candidate-lobby-host-exit-returned.png" \
    "${OUTPUT_DIR}/candidate-lobby-host-exit-return-reference.png"

# A sealed screen is independent of the secret, and delayed/unmatched input
# cannot reveal it. These are byte-for-byte PNG comparisons, not heuristics.
cmp "${OUTPUT_DIR}/candidate-role-sealed-wolf.png" \
    "${OUTPUT_DIR}/candidate-role-sealed-seer.png"
cmp "${OUTPUT_DIR}/candidate-role-sealed-wolf.png" \
    "${OUTPUT_DIR}/candidate-role-long-unarmed.png"
cmp "${OUTPUT_DIR}/candidate-role-sealed-wolf.png" \
    "${OUTPUT_DIR}/candidate-role-click-before-view.png"
cmp "${OUTPUT_DIR}/candidate-role-sealed-wolf.png" \
    "${OUTPUT_DIR}/candidate-role-private-epoch-changed.png"
cmp "${OUTPUT_DIR}/candidate-role-revealed.png" \
    "${OUTPUT_DIR}/candidate-role-heartbeat.png"
cmp "${OUTPUT_DIR}/candidate-role-release.png" \
    "${OUTPUT_DIR}/candidate-role-review-ready-villager.png"
cmp "${OUTPUT_DIR}/candidate-role-release.png" \
    "${OUTPUT_DIR}/candidate-role-trailing-click.png"
cmp "${OUTPUT_DIR}/candidate-role-revealed-villager.png" \
    "${OUTPUT_DIR}/candidate-role-review-again-villager.png"
cmp "${OUTPUT_DIR}/candidate-role-confirm-villager.png" \
    "${OUTPUT_DIR}/candidate-role-heartbeat-release.png"
cmp "${OUTPUT_DIR}/candidate-role-sealed-wolf.png" \
    "${OUTPUT_DIR}/candidate-role-private-epoch-release.png"
cmp "${OUTPUT_DIR}/candidate-private-sealed-wolf.png" \
    "${OUTPUT_DIR}/candidate-private-sealed-good.png"
cmp "${OUTPUT_DIR}/candidate-private-sealed-good.png" \
    "${OUTPUT_DIR}/candidate-private-click-before-view.png"
cmp "${OUTPUT_DIR}/candidate-private-sealed-good.png" \
    "${OUTPUT_DIR}/candidate-private-epoch-changed.png"
cmp "${OUTPUT_DIR}/candidate-private-good.png" \
    "${OUTPUT_DIR}/candidate-private-review-again.png"
cmp "${OUTPUT_DIR}/candidate-private-good.png" \
    "${OUTPUT_DIR}/candidate-private-heartbeat.png"
cmp "${OUTPUT_DIR}/candidate-private-review-ready.png" \
    "${OUTPUT_DIR}/candidate-private-trailing-click.png"
cmp "${OUTPUT_DIR}/candidate-private-confirm.png" \
    "${OUTPUT_DIR}/candidate-private-waiting.png"
if cmp -s "${OUTPUT_DIR}/candidate-role-sealed-wolf.png" \
          "${OUTPUT_DIR}/candidate-role-release.png" ||
   cmp -s "${OUTPUT_DIR}/candidate-role-revealed-villager.png" \
          "${OUTPUT_DIR}/candidate-role-review-ready-villager.png"; then
    printf 'Private review-ready prompt did not render distinctly\n' >&2
    exit 1
fi
cmp "${OUTPUT_DIR}/candidate-room-closed-from-wolf.png" \
    "${OUTPUT_DIR}/candidate-room-closed-from-seer.png"
cmp "${OUTPUT_DIR}/candidate-kicked-from-wolf.png" \
    "${OUTPUT_DIR}/candidate-kicked-from-seer.png"
cmp "${OUTPUT_DIR}/candidate-lobby-nickname-limit.png" \
    "${OUTPUT_DIR}/candidate-lobby-nickname-exact.png"
if cmp -s "${OUTPUT_DIR}/candidate-error-host-clean.png" \
          "${OUTPUT_DIR}/candidate-error-host-close-stale.png"; then
    printf 'Reliable host termination did not render its locked wait state\n' >&2
    exit 1
fi

# Authoritative night resolution must not alter the persistent public player
# strip until DAWN.  This is tested for both sealed and revealed private pages.
cmp "${OUTPUT_DIR}/candidate-private-sealed-good-no-pending.png" \
    "${OUTPUT_DIR}/candidate-private-sealed-good.png"
cmp "${OUTPUT_DIR}/candidate-private-good-no-pending.png" \
    "${OUTPUT_DIR}/candidate-private-good.png"
cmp "${OUTPUT_DIR}/candidate-private-sealed-good-no-local-pending.png" \
    "${OUTPUT_DIR}/candidate-private-sealed-good-local-pending.png"

# Conversely, a death that is already public must alter the strip. This guards
# against a renderer that protects secrets by accidentally ignoring all deaths.
if cmp -s "${OUTPUT_DIR}/candidate-private-sealed-good-no-known-dead.png" \
          "${OUTPUT_DIR}/candidate-private-sealed-good-no-pending.png"; then
    printf 'Public death indicator did not change the rendered header\n' >&2
    exit 1
fi

# Use an otherwise identical model to prove each pre-online link topology is
# distinct. Full reconnect/disconnect states also carry contextual banners.
if cmp -s "${OUTPUT_DIR}/candidate-status-link-online.png" \
          "${OUTPUT_DIR}/candidate-status-link-scanning.png" ||
   cmp -s "${OUTPUT_DIR}/candidate-status-link-online.png" \
          "${OUTPUT_DIR}/candidate-status-link-pairing.png" ||
   cmp -s "${OUTPUT_DIR}/candidate-status-link-scanning.png" \
          "${OUTPUT_DIR}/candidate-status-link-pairing.png"; then
    printf 'Connection indicator states rendered identically\n' >&2
    exit 1
fi

# Filled count carries strength; hue distinguishes a weak/fair warning from a
# good/strong link. Every adjacent state must therefore render differently.
signal_states=(none weak fair good strong stale)
for ((i = 1; i < ${#signal_states[@]}; ++i)); do
    previous="${signal_states[i - 1]}"
    current="${signal_states[i]}"
    if cmp -s "${OUTPUT_DIR}/candidate-status-signal-${previous}.png" \
              "${OUTPUT_DIR}/candidate-status-signal-${current}.png"; then
        printf 'Signal states %s and %s rendered identically\n' \
            "${previous}" "${current}" >&2
        exit 1
    fi
done

# A disconnected topology must suppress a cached strong value immediately.
cmp "${OUTPUT_DIR}/candidate-status-signal-disconnected.png" \
    "${OUTPUT_DIR}/candidate-status-signal-disconnected-cached-strong.png"

printf 'Werewolf UI candidates and privacy comparisons: %s\n' "${OUTPUT_DIR}"
