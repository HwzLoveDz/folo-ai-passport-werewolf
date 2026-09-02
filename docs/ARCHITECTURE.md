# AI-PASSPORT Werewolf Architecture

## Product boundary

The first release is a seven-device, offline, no-phone Werewolf game. One of
the seven devices is the fixed authority and also belongs to a player. During
an active game, the authority owns all roles and game state; the other six
devices retain only the public state and their own private view. The validated
seven-role deck is distributed only for the common review after game over.

AI is deliberately outside the rules engine. A later narrator may turn
structured events into speech, but it must never choose roles, validate moves,
resolve votes, or decide a winner. A failed or unavailable AI service therefore
cannot block a game.

## Runtime ownership

- The button component reports physical key edges to the application callback.
  Under the LVGL lock, that callback first applies any newly published immutable
  model, then resolves local UI state/rendering and queues only a value action
  for the controller.
- All LVGL access is serialized by the display lock. Controller publication and
  button handling may apply a complete immutable `werewolf_ui_model_t`; neither
  path reads a partially updated controller working model.
- The controller task is the only writer of the session and authoritative game
  state.
- The ESP-NOW task validates frames, performs ACK/retry/deduplication, and queues
  decoded frames to the controller.
- ESP-NOW callbacks do no allocation, rendering, game work, or logging of
  payload contents.
- The static sound task asynchronously consumes non-authoritative UI cues; its
  private cue policy is role-neutral. The static battery task samples the
  chip-reported SOC every 30 seconds and exposes fresh, stale, or unavailable
  local telemetry, never a voltage-derived percentage. Neither task can advance
  session or game state.
- NVS persists only the local nickname record and its provisioning revision;
  optional per-device factory nickname/revision settings seed that record. It
  does not persist an active room or game checkpoint, and restart/resume is not
  implemented.

All queues, peer tables, replay windows, game state and UI models have fixed
capacity. The ESP32-C3 has no PSRAM, so the firmware must not turn packet or
event storage into unbounded lists.

## Discovery and encrypted pairing

Protocol v5 uses a commit/reveal handshake before an ESP-NOW LMK exists. Only
`BEACON`, `JOIN` (commit payload), `PAIR_HOST_REVEAL`, and
`PAIR_CLIENT_REVEAL` may be
broadcast. A beacon carries protocol/rules versions, occupied seats, one
currently offered seat, the room fingerprint, and a commitment. It deliberately
does not disclose the host public key.

A scanning client stores compatible beacons in a fixed-capacity room directory
and renders an explicit room list; it never adopts the first received beacon
automatically. A room identity binds the Host MAC, session ID and authority
epoch, while a stable selection token keeps the highlighted row attached to the
same room as beacons are refreshed and deterministically sorted. `UP/DOWN`
changes the selected room and `OK` joins it. The controller revalidates that the
selected entry is still fresh before adopting it; entries older than two seconds
are removed instead of silently redirecting the client to another Host.

The handshake for that single offered seat is:

1. The client sends a commitment to a fresh X25519 public key and nonce.
2. The host locks that client MAC and commitment, then reveals its own fresh
   public key and nonce while echoing the locked client commitment.
3. The client verifies the beacon commitment and reveals its public key and
   nonce while echoing the host commitment.
4. The host checks the client commitment. Both endpoints derive the same
   peer-specific 16-byte ESP-NOW LMK and six-digit read-only `VERIFY` code from
   the bound transcript and shared key material.
5. The host installs the encrypted peer and sends `ACCEPT` only as encrypted
   unicast. The client displays the code beneath `ROOM`; the Host displays the
   matching code only in that guest's selected detail view.

The Host creates fresh offer material when opening a room and rotates it after
each successful pairing, each bounded offer timeout, or any Lobby player
removal/kick. An unpaired client cancellation is not a message to the Host and
therefore relies on that bounded offer timeout. A client creates fresh material
for every new attempt. The Host never reuses revealed material for another seat
or candidate.
Each endpoint derives its pair values from:

```text
X25519 shared secret
  + session ID
  + offered seat
  + host/client MAC addresses
  + host/client public keys and nonces
  + host/client commitments
  -> transcript-bound HKDF
  -> peer LMK + six-digit display-only VERIFY code
```

ESP-NOW uses each device's 16-byte PMK as an AES-128 wrapping key for LMKs;
encrypted peers require the two endpoints to install the same peer LMK.
This implementation configures a versioned room-scoped PMK derived from the
public session ID, protocol epoch and room fingerprint instead of relying on an
SDK default or compiled constant. Because those inputs are public, the room PMK
is not claimed as a confidentiality or identity secret. Protection of action
frames rests on the secret X25519-derived LMK unique to each Host/Guest pair.
No production key is compiled into the firmware, advertised, or printed.
Commit/reveal binds fresh key material to one transcript before it is disclosed,
so an endpoint cannot replace that material after seeing the other reveal.

Protocol v5 has no machine-side confirmation action or persisted confirmation
state. The locally derived `VERIFY` code is never carried in a packet and never
enters the `READY` or START predicates. Players may compare the two displays in
person; a mismatch is handled operationally by the guest leaving or the Host
kicking that guest. If the comparison is actually performed and matches, it
helps the people identify the two ends of that link. Without that human step,
commitment binding, X25519, transcript-bound HKDF, the ESP-NOW PMK/LMK setup and
encrypted unicast authenticate the cryptographic transcript rather than the
person holding a nearby device. Nicknames and `H`/`Y`/`G` badges are also
presentation metadata, not credentials.

After pairing, `ACCEPT`, `PROFILE`, `READY`, roles, phases, actions, snapshots
and aborts are encrypted unicast. A nickname is display metadata, never an
authorization identity: seats remain authoritative and duplicate names are
allowed. Every name is canonical printable ASCII, at most ten characters. The
host echoes a revisioned complete roster in lobby snapshots and START; a client
cannot become ready until its own encrypted profile has been echoed. The host
refuses to start unless the v5 gate is complete: all seven seats are occupied,
all seven profiles are present, all seven players are `READY`, and all six
host-to-client encrypted links are installed. A missing nickname echo is
retried, but becomes a recoverable name-sync timeout instead of leaving the
client disabled forever. In the lobby list, `H` marks the Host, `Y` marks the
viewer's own guest row, and `G` marks another guest; these badges do not affect
authorization.

## Reliable session protocol

The application payload codec is independent from the ESP-NOW frame codec.
Both use explicit network byte order and exact lengths; C structures are never
sent directly.

Every unicast frame has a session ID, authority epoch, message sequence,
phase sequence, optional acknowledged sequence, and CRC16. CRC detects damage;
ESP-NOW CCMP provides confidentiality and authentication. The transport adds:

- application ACKs and bounded exponential retry;
- a 32-message receive window for reordering and replay rejection;
- an action key cache so the same logical move is applied once;
- old-session, old-epoch, wrong-destination and late-phase rejection;
- periodic encrypted snapshots as heartbeat and resynchronization input.

An actively joined guest leaving voluntarily, a Host closing its lobby, and a
Host terminating after a fatal session error all enter the same reliable
terminal controller state. The controller first seals private UI, destroys any
pairing attempt, discards obsolete reliable traffic, snapshots the transport's
retry-exhaustion counter, and fixes the target-peer set before queuing encrypted
`ABORT` frames. A guest leave uses `USER_CANCELLED`; a manual Host close uses
`HOST_CLOSED_ROOM`; a fatal Host path preserves its precise abort reason and may
remove a peer already known to be gone from the target set.

First-attempt enqueue failures remain explicit missing targets and are retried.
ESP-NOW stays active until every target has been queued and the pending table
drains with no new retry exhaustion, or until the nine-second terminal deadline
expires. In particular, `pending == 0` after a retry-exhaustion event is not
mistaken for acknowledgement. Receivers acknowledge at the transport layer
before controller cleanup: the Host removes a voluntarily departing guest, and
a guest receiving `HOST_CLOSED_ROOM` clears its session only after ACK and then
presents the single-action `ROOM CLOSED` notice. Input remains locked throughout
the terminal drain.

A targeted Host kick uses a separate single-peer reliable controller rather
than the all-room terminal state. The player detail menu freezes the selected
seat and defaults to `BACK`; choosing the explicit red `KICK PLAYER` item starts
the kick immediately without a second dialog. The controller then freezes the
peer generation, locks input, sends `KICKED_BY_HOST`, and waits for ACK or the
same bounded deadline before removing the link. The guest acknowledges before
cleanup and then sees the single-action `REMOVED FROM ROOM` notice. Sending and
ACK-wait overlays intentionally have no fake `BACK` action because the reliable
operation is already committed.

The transport copies per-packet receive RSSI only after an encrypted peer and
current reliable-session frame have been validated. The guest therefore shows
the Host link; the Host aggregates the weakest current guest link so one
failing device cannot be hidden by an average. The controller quantizes this
telemetry on a fixed cadence, marks old samples stale, and never advances UI
revision or private gate state for a signal-only update.

Once a reliable frame has entered the bounded pending table, the send API
reports it as accepted even if the first `esp_now_send` call is temporarily
busy. The retry worker retains ownership and either delivers it or emits one
bounded delivery-failure event; a transient first-send error must not make the
Host tear down an otherwise recoverable game burst.

Controller-only confirmation gates (role viewed, private result viewed, dawn and
exile acknowledged) carry their own `gate kind + gate epoch` in phase state,
actions and private payloads. A heartbeat tells each encrypted recipient only
whether that recipient's current gate action was accepted. Old or mismatched
gate actions/private payloads are rejected; a not-yet-acknowledged role or
private-result payload is re-sent by encrypted unicast. This keeps a lost client
action recoverable without exposing another player's acknowledgement timing.

Role and private-result gates deliberately separate viewing from acknowledgement.
Holding OK reveals only while the recorded page and private gate epoch still
match; releasing OK immediately seals the display without sending an action.
The player may repeat that hold/release cycle as often as needed. Only after at
least one valid reveal may a later, independent short OK emit `ROLE_SEEN` or
`ACK_RESULT`. Any click tail generated by the long-press sequence is suppressed.
Changing page or gate epoch, losing the link, or disabling input clears the
local reveal and completion eligibility fail-closed; an ordinary same-gate
heartbeat preserves both an active hold and the ability to review again.

Normal night, speech and vote actions remain locally in-flight until an
encrypted Host snapshot confirms the local submitted bit, the next speaker, or
a later phase. If transport retries have settled and a newer snapshot still
disagrees, input is reopened instead of leaving a permanent UI latch. A Guard's
previous target is committed only after this authoritative confirmation.

Private sealing on OK-release, controller model publication, and UI latch
rollback are sticky work items. If the LVGL lock or controller queue is
temporarily busy, the controller retries them on its next tick. Pending
controller state also blocks keys from being interpreted against an older
render, so disconnecting or changing phase cannot leave a role visible or
accept an obsolete action.

The controller stages each presentation model under a dedicated snapshot mutex.
While holding that mutex it first raises the sticky apply intent and then copies
the complete model. A button callback may claim the intent first, but it must
then acquire the same mutex and therefore cannot consume a partial copy or miss
the publication window. It applies that immutable snapshot before any delayed
release or current key event and never copies the controller's partially updated
working model. The producer does not wait for LVGL while holding the snapshot
mutex, preserving lock order. This keeps same-gate holds valid while page,
epoch, disconnect and input-lock changes remain fail-closed.

The game core independently checks a full 32-bit `phase_epoch`. A transport bug
or repeated packet therefore cannot advance the rules engine twice.

## Game sequence

1. Each client chooses the intended Host from the explicit room list; receiving
   a beacon never auto-joins a room.
2. After a guest joins and its profile/nickname is echoed, it may mark itself
   `READY`; no device may set another player's readiness. The guest and Host may
   compare their read-only `VERIFY` displays while in the lobby, but no
   confirmation is recorded and the result does not change the START gate.
3. START remains locked until all seven seats and profiles exist, all seven
   players are `READY`, and all six Host-to-client encrypted links are present.
4. The host shuffles `2 Wolf + Seer + Guard + 3 Villager` from a random 64-bit
   seed and sends each player only their private view.
5. Every player may hold OK to reveal and release to hide as often as needed.
   After viewing at least once, a separate short OK acknowledges the sealed role
   page. Night does not open until all seven devices acknowledge it.
6. Every living player uses the same night interaction and fixed-size action.
   Villagers submit a dummy target. Wolves get one structurally identical
   reselection if they disagree. Target seconds are prompts only in the MVP;
   phases do not expire automatically.
7. Every device receives the same private-result gate and uses the same
   repeat-view-then-short-OK acknowledgement flow. Only the Seer gets a target
   faction; no exact good role is disclosed.
8. Dawn result, ordered discussion, secret vote, optional tie/revote, exile,
   and the next night repeat until a win condition is met.
9. Only after `GAME_OVER` does the host send the strictly validated seven-role
   reveal deck for the common review screen. The encrypted heartbeat repeats
   both the terminal snapshot and deck while the Host remains in review; retry
   exhaustion alone does not delete that peer. A completed client keeps its
   static review if the Host later leaves.

## Failure policy

- A dropped unicast is retried and acknowledged without applying the action
  twice.
- A temporarily silent host puts a client into Reconnecting while keeping
  private input closed.
- A permanently lost host aborts the current MVP game. There is no automatic
  authority migration.
- During an active game, an explicit Guest leave or a Guest judged permanently
  lost after the delivery-failure window makes the Host reliably abort the
  whole room; seats are never backfilled mid-game. After `GAME_OVER` with no
  pending review gate, an explicit leave only retires that peer, while retry
  exhaustion keeps the peer installed so later encrypted review heartbeats can
  recover it.
- Host-side fatal termination uses the same ACK-or-deadline drain as a manual
  room close, so the controller does not tear down ESP-NOW immediately after
  queuing abort notices.
- Host restart checkpoint/resume is a later persistence gate. Until that gate
  is implemented and physically tested, a host reboot is treated as host loss.
- NVS initialization errors are reported. The firmware must never erase the
  whole NVS partition as automatic recovery.

## Delivery status

The single progress source is [ROADMAP.md](ROADMAP.md). It keeps native,
firmware, physical-device and privacy-audit evidence separate. Compilation and
simulator output cannot certify flashing, RF behavior, discovery range, loss
rate, physical long-press timing, or a complete seven-device playthrough.
