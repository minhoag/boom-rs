# Rust project architecture

Revision **`architecture-r1`**, 2026-10-19 (r0 was a draft from before the repo existed). Baseline `KR-00967-V1-scope-1`.
Canonical source for crate layout, boundaries, contracts, and code rules. Player scope and policy live in
[SCOPE_V1](SCOPE_V1.md). Reasoned decisions are ADR 0003–0008.

## 1. Repo and crates

Repo `boom-rs` ([ADR 0003](../decisions/0003-engine-repo-and-docs-home.md)):

```text
boom-rs/
  docs/                                 # operating docs (see docs/README.md)
  tools/                                # RE, packaging, and localization scripts (Python)
  Cargo.toml, rust-toolchain.toml       # workspace, pinned toolchain
  .cargo/config.toml                    # `cargo xtask` alias
  crates/
    boom-core/      # [present] match rules: Mode, System, SimulationEvent, state, RNG, hash; pure, no IO
      src/systems/  #      one module per gameplay mechanism (movement; bomb/item/trap added later)
    boom-content/   # [present] rules-profile schema + runtime map; load and validate TOML
    boom-assets/    # [present] read legacy formats (.map) into the content schema; CLI
    boom-replay/    # [present] CLI to run/check a replay and compare checkpoint hashes
    boom-scenario/  # [present] shared match-setup format + boundary-scenario harness
    boom-proto/     # [frame, batch 2] envelope + new-protocol DTOs (ADR 0009); no rules
    boom-net/       # [frame, batch 2] transport trait, framing, session (ADR 0006)
    boom-sync/      # [frame, batch 2] SimulationState ↔ wire DTO adapter, shared by server and client
    boom-room/      # [present] pure room/lifecycle state machine (SCOPE_V1 §3); API in §5.4
    boom-client/    # [frame, batch 2] macroquad (ADR 0010): input, prediction, render/audio/UI; reads bundles, not packs
    boom-server/    # [frame, batch 2] auth, lobby, authority, persistence
  xtask/                                # `cargo xtask ci`, golden replay
  data/rules/                           # rules profiles with provenance (TOML)
  data/assets/                          # presentation/closure mappings with evidence labels (TOML)
  data/overlays/, data/items/, data/events/  # [planned] rule overlays, item effects, liveops (ADR 0008)
  fixtures/replays/                     # replays + golden hashes, run on three-OS CI
  fixtures/scenarios/<system>/          # boundary scenarios per system (active/pending)
  .github/workflows/ci.yml              # three-OS CI (needs a remote to run)
```

A `[planned]` crate is created only when that phase's task opens. Do not split render/audio/UI into
their own crates before there is a reuse need or a clear owner.

Dependencies (arrow = depends on):

```text
boom-content → (serde, toml)              # does not depend on core/client/server
boom-core    → boom-content
boom-assets  → boom-content
boom-scenario → boom-core, boom-content
boom-replay  → boom-core, boom-content, boom-scenario
boom-proto   → boom-content (ID/schema identity)
boom-net     → boom-proto
boom-sync    → boom-core, boom-content, boom-proto
boom-qa      → boom-client
boom-room    → boom-content
boom-client  → boom-core, boom-content, boom-proto, boom-net, boom-sync
boom-server  → boom-core, boom-content, boom-proto, boom-net, boom-sync, boom-room, boom-scenario (replay format + `apply_authority`, #87)
```

`boom-proto` does not depend on `SimulationState`. Server and client convert DTOs in the adapter so the
network schema does not lock the core memory layout. Runtime consumes a validated bundle. The client
does not carry a pack decoder.

Current foundation dependencies: `serde` (derive) and `toml` in `boom-content` to read profiles; `postcard` 1.x in
`boom-proto` (ADR 0009); `macroquad` =0.4.16 (feature `audio`) in `boom-client` (ADR 0010); `rusqlite` (bundled), `argon2`, `getrandom`,
`fs4`, `serde_json`, `sha2`, `base64` (plus `tempfile` for tests) declared in the workspace for `boom-server` (ADR 0011 §10).
Record the reason for a new dependency in the commit. Record foundation dependencies here.

`miniquad` 0.4.11 (a macroquad dependency) uses the source pin in
[`vendor/miniquad`](../../vendor/miniquad/Cargo.toml), via `[patch.crates-io]`. Version and API stay the same.
Issue 270 changes only X11 `set_fullscreen`: send EWMH add/remove for the same atom, keep the mapped window
and the other states, instead of unmap/remap and replacing the whole property.
[Provenance](../../vendor/miniquad/UPSTREAM.json) has the original crate SHA, commit, per-file hash, the
MIT/Apache license, and the condition for dropping the patch once upstream has an equivalent fix. The vendored
crate sits outside the workspace. The packaging tool still lists this path dependency in LICENSES. Other backends
keep upstream source.
`.gitattributes` keeps vendor bytes stable on three OSes and turns off whitespace warnings only in the upstream tree.
Project source is still checked normally.
Reference: [EWMH §5.7](https://specifications.freedesktop.org/wm/latest/ar01s05.html).

## 2. Extending features, modes, and events

Follow [ADR 0008](../decisions/0008-extensibility-model.md). One tick:

```text
accepted input ─▶ Mode.systems() in order ─▶ SimulationState (typed, hashed)
                         │                              │
                         ▼                              ▼
                EventSink: SimulationEvent{id=(tick,seq), kind}   Mode.outcome() → RoundEnded
                         │
         ┌───────────────┼───────────────────────┐
         ▼               ▼                       ▼
 presentation journal   server: domain event   replay/golden: event_digest
 (audio, effects)       → stats/quest/reward
```

| Adding | Where |
|---|---|
| A gameplay mechanism | `boom-core/src/systems/<name>.rs` implements `System`, then add it to the mode's `systems()` |
| A mode | A type that implements `Mode`, registered in `modes()`. The ID is stored in replay and protocol |
| A parameter | A rule in `data/rules/*.toml` plus a RULES_V1 claim |
| An event or season | `RulesOverlay` (rules) plus liveops data (schedule, rewards, flags) on the server |
| A per-match reaction | A `SimulationEvent` consumer outside core, deduped by event ID + round ID |
| A new item | Item data mapped onto an existing effect primitive. A new primitive only when one is required |
| A feature outside the match | A backend module per domain, consuming domain events. Idempotent ledger |

## 3. Code rules

- Toolchain pinned in `rust-toolchain.toml`. Edition 2024. Shared `[workspace.lints]`.
- `unsafe` is forbidden in every current crate.
- `boom-core` ([ADR 0004](../decisions/0004-time-numeric-model.md)): no floats
  (`clippy::float_arithmetic` deny), no `HashMap`/`HashSet`, `Instant`, or `SystemTime`
  (`crates/boom-core/clippy.toml`), no IO, sockets, wall clock, render, or audio.
- Every rule value comes from the rules profile. Core code does not embed gameplay constants.
- `cargo xtask ci` = fmt check + clippy `-D warnings` + tests (including every scenario) + golden replay check + oracle check.
- Every change to `main` goes through a pull request. Three-OS CI must be green. The reviewer is someone other than the author, except a progress-ledger PR.

## 4. Rules profile, content, and the time model

TOML profiles in `data/rules/` (overlays in `RulesOverlay`, ADR 0008) have `profile_id`, `schema`, `release`, and one entry per rule:
`{ value, unit, source, status, claims }`. `source` and `status` use the evidence labels in ADR 0013.
`claims` point at IDs in [RULES_V1](../specification/gameplay/RULES_V1.md). `boom-content` refuses to load when:

- `status = "unknown"` and a `value` is present, or a `value` is missing when status is not unknown;
- a profile with `release = true` still has a `candidate` or `unknown` rule;
- a rule has no claim (except a pure architecture `design` such as the tick).

The profile identity is an `fnv1a64-v0` hash of the canonical encoding of the rules, not a hash of the raw file, so CRLF versus LF
does not change the identity. A replay records this identity and rejects a different profile.

The C00/C01 stat tables are **rule values**, so they live in the profile (`characters.c00_*`,
`characters.c01_*`; RULES_V1 R-040/R-041), not in a server config file. Everyone running the
server must use the same source, and the stats are part of the identity, so `rules_hash` changes when the stats
change and the handshake catches a mismatch. `boom-content` reads and checks the stat table (both
characters present, current level at or under the cap). If it is missing or wrong, `serve` reports a clear error and does not invent a default.

The runtime map (`boom-content::MapGrid`) holds size, per-build cells `(tile_id, obj_id)`, and spawns.
Cell kinds (walkable, hard block, soft block) are a separate classification table per build. An ID missing from the table is
`Unknown`, and core **refuses** to build a world that contains an Unknown cell instead of guessing. Hand-written fixtures use an ASCII map
with known cell kinds. The legacy `.map` reader supports both the VN header (spawn at +76) and the KR header (+80), per R-003.

Time: fixed step, `sim.ticks_per_second` from the profile. Coordinates are `Mpx` (1/1000 px). Milliseconds convert to ticks by rounding up
once at load (ADR 0004). Calibrating the original tick and speed-per-level is still open (Q-004, Q-005).

## 5. Contracts — `contracts-r4`

Revision **`contracts-r4`**, 2026-10-19 (r3 was reviewed in #2; r4 only adds snapshot getters and waits for review in the batch-2 PR). Code is the canonical source of the data types.
This section keeps the rules. When a review changes them, the integrator publishes a new revision and tells the affected tasks.

| Revision | Change | Reason |
|---|---|---|
| r1 | First cut: state by module, V1 events, FFA pipeline, write rights, scenario harness | START-C |
| r2 | `check_invariants(&World)`: IDs unique across bombs and items; cell, owner, grace, and `ItemKind` checked against World/Rules; players inside the grid. `new_round` rejects stats over the cap (`CoreError::InvalidStat`) and checks invariants after `on_round_start` (`CoreError::Contract`) | Review #2: three P2 bugs (duplicate IDs, bad references, a round built with bad stats) |
| r3 | `check_invariants` checks grid data length before any cell access. `Field::kind`/`hidden` do not panic on a mismatch. `World::new` rejects terrain whose cell count differs from the size, an empty grid, or a spawn outside the grid (`CoreError::BadTerrain`). Systems are unaffected | Review #2, second pass: a grid declared as 3 cells that held 2 was still Ok; 0 cells panicked |
| r4 | `Rng::state()`/`Rng::from_state()`, `IdAllocator::peek_next()`/`IdAllocator::from_next()` so a snapshot can restore RNG and the allocator | ADR 0009 §4.4 |

### 5.1 Types in `boom-core`

| Contract | Defined in | Notes |
|---|---|---|
| `PlayerInput { dir, place_bomb }` | `sim.rs` | Input **already accepted by authority** for one tick. `place_bomb` is a press edge, not a hold |
| `PlayerSetup { id, stats: CharacterStats }` | `sim.rs`, `state/player.rs` | `Stat { value, max }` for speed/bombs/range, taken from BomberAttr through content |
| `SimulationState` | `state/mod.rs` | `tick, phase, rng, ids, players, field, bombs, flames, items`. Each part hashes itself in its own file |
| `TickScratch` | `scratch.rs` | Data **for one tick**, passed between systems (blast channel: cells just covered by an explosion). **Not part of state**: not hashed, not snapshotted, not in the DTO. On the gameplay path `SimulationState::step` creates an empty scratch every tick, so the tick result is always a function of state + input |
| `EntityId`, `IdAllocator` | `state/mod.rs` | Bomb and item IDs increase from 1 within the round. Allocate only through `ids.next_id()` |
| `Player`, `Life`, `Stat` | `state/player.rs` | `Life::Trapped { since }` still counts as not dead |
| `Field` | `state/field.rs` | Cell kinds that can change during the round, plus items hidden in soft blocks |
| `Bomb { id, owner, cell, range, explodes_at, grace }` | `state/bomb.rs` | Range is captured at placement (R-032). `grace` = who may walk off the bomb |
| `Flame { cell, until, source, owner }` | `state/flame.rs` | `until` is the last tick the flame still exists (inclusive). One flame per cell |
| `FloorItem`, `ItemKind` | `state/item.rs` | `ItemKind(i)` = index into `Rules::items` (V1: Bomb, Fire, Roller) |
| `Rules` | `rules.rs` | Every V1 rule value, converted from the profile. New rules go through the integrator |
| `EventKind` (10 V1 kinds) | `event.rs` | `name()` + `fields()` is the shared encoding for digest, scenario, and log |
| `Mode`, `System` | `mode.rs`, `system.rs` | `System::on_round_start` for start-of-round work (hide items). `System::run` also takes `&mut TickScratch` for in-tick data |

`SimulationState::check_invariants(&World)` (`state/invariants.rs`) checks: lists are sorted; one bomb, item, or
flame per cell; entity IDs are unique across kinds and were issued by the allocator; bombs and items sit on floor cells, flames on floor or
soft block; cell-data count matches the grid size (checked before any cell access); owner and grace are real players; `ItemKind` exists in `Rules::items`; players are inside the grid;
stats do not exceed the cap; hidden items are only inside soft blocks. The function does not panic on a bad state. `new_round` calls it
before returning state. The scenario harness and replay call it after every tick, so a system that breaks a contract is caught on the tick that did it.

The blast channel **is not in state**, so it is not in `check_invariants`. It is checked separately by
`TickScratch::check(&World)` (`scratch.rs`), and that function is the only check for the channel.

### 5.2 Order inside one tick (FFA pipeline)

`movement → bomb_place → explosion → blocks → pickup → life`, then `tick += 1`, then `Mode::outcome`.

Movement: Alive uses the speed level. Trapped uses `Rules::trapped_speed` from the key
`player.trapped_speed_px_per_s` (R-161/R-162). Dead stands still. Trapped still cannot place a bomb or pick up.
Both axes, the hitbox, and corner assist keep the V1 policy. Native measurement so far only covers horizontal input. The predictor uses the same system.
Start of round: grid from terrain → spawn permutation by seed → `on_round_start` on each system in the order above.
RNG is consumed in exactly that order. Changing the order changes the rules: integrator only, with a scenario and a golden bless.

Consequences already fixed: a bomb placed in step N sits on the cell the player occupies **after** movement in step N. A bomb with
`explodes_at = N` explodes in step N. An explosion in step N breaks blocks (the tick's blast channel, `TickScratch`).
Flame spawned in step N traps a player in the same step N. The round result is evaluated after every system has run. Still open (decide
inside the matching task and record it in RULES_V1): whether flame destroys an item already on the floor; whether a pickup at the cap consumes the item or leaves it.

The blast channel lives in that tick's `TickScratch`: `explosion` writes it, `blocks` reads it and clears it. Because it is outside
state, **state is the only source of the tick**. The same state plus the same input always produces the same tick result,
and state restored from a snapshot cannot diverge from the original state on the next tick. The fixture path
`SimulationState::step_with_scratch` lets a test prebuild the blast channel to exercise the `blocks` system alone.
On that path the scratch is an **extra input** of the tick and is checked with `TickScratch::check`.

### 5.3 Write-rights matrix

The detailed table is at the top of `crates/boom-core/src/systems/mod.rs`. Summary: each public field has one
system allowed to write it (movement → position/facing and clears `grace`; bomb_place → adds bombs; explosion → removes bombs, writes flames,
writes the tick's blast channel; blocks → grid, revealed or destroyed items, reads then clears the tick's blast channel; pickup → removes items, writes stats;
life → `Player.life`). Reads are free. Writing another system's field means opening an issue for the integrator, not editing it directly.
The tick's blast channel is not a public field of state. It is temporary data inside the tick, passed through
`TickScratch` (`scratch.rs`).

### 5.4 Network and replay contract (no code yet when this was written)

Revision **`proto-r1`**, 2026-10-19, **Accepted** under
[ADR 0009](../decisions/0009-wire-protocol-input-snapshot-codec.md) (PR #24). `boom-proto` had not been created yet.
The integrator opens the implementation task in P7. Once code exists, the DTO types are canonical. This section keeps the rules and the review invariants.

| Contract | Minimum contents | Invariant to review |
|---|---|---|
| Envelope | 16 bytes: magic `BOOM`, `wire_schema=1`, `kind`, `flags`, `payload_len`, reserved; postcard payload | Reject bad magic/schema/kind/flags/len before decode. Frame max 64 KiB. An unknown message must not be guessed |
| Handshake (V1-002) | `wire_schema`, `state_schema`, `rules_profile_id`, `rules_hash`, `content_sha256`, capability | Exact match in V1. The server compares against its own profile and bundle. It does not use the client hash to choose content. A mismatch is rejected before the lobby |
| `InputCommand` / batch | `round_id`, player bound by the server, `ack_snapshot`, at most 8 commands `{seq, target_tick, dir\|bomb}` | Seq increases continuously **inside a batch** (a hole between batches is a gap). **Payload is immutable for a `seq` after the first send**. Only a command not yet sent may recompute `target_tick`. **Dedup happens before the window check**. A command with `seq <= applied_seq` is dropped even if `target_tick` is old. A past command at the head of the queue is retired and acked (not applied). Late window 100 ms, early window 200 ms. A gap timeout of 100 ms only retires the missing sequence. It does not drop pending commands. `InputRejected` carries `applied_seq`/`server_tick`/`first_seq`+`command_count` so the rejected batch can be identified and resynced. Rate limit. Authority turns this into `PlayerInput` |
| `Snapshot` | `snapshot_seq`, round/tick, `state_schema`, `rules_hash`, `content_epoch`, phase, RNG/allocator, input ack, players/field/bombs/flames/items | **Complete**. It can restore `SimulationState`. The client rebuilds and calls `check_invariants`. Drop an old snapshot or one from another round. No delta yet |
| `ReplayHeader` | schema, profile ID + hash, mode, map, seed, roster/spawn, `hash_algo` | Reject a mismatched revision or profile. Current file format: `boom-replay` schema 1 |
| `ReplayRecord` | tick, `PlayerInput` **after the scheduler** (including neutral/expired), checkpoint hash | Record the input that was actually applied. Do not record the raw batch or `target_tick`. Do not depend on the wall clock |
| Outbound event | `round_id` + `EventId` + `name` + `fields` | Consumers dedup by `(round_id, EventId)`. A snapshot does not replay events |

Accompanying rules:

- Network input is only a **proposal**. The server turns it into `PlayerInput` after checking round, sequence, window, and
  rate limit. The client does not send coordinates, health, items, or results. A duplicate, late, or out-of-window command must not change
  state. An expired command or a gap is recorded as neutral in the replay. `applied_seq` is the sequence that has been **resolved**
  (applied or retired), not "input was applied". Acks use this value.
- **Liveness:** dedup before the window check. A past command at the head is retired as soon as the batch is accepted,
  plus an ack. Gap timeout only retires the missing sequence and then processes pending commands. `OutOfWindow` is only for a
  `target_tick` too far in the future. `InputRejected` carries `applied_seq`/`server_tick` so the client can drop old commands, split
  commands that are too far ahead, and resend. **Payload is immutable for a `seq` after the first send**: the client only recomputes
  `target_tick` for a command not yet sent. A command that may already be buffered is resent unchanged (not retargeted). A command that is
  certainly not buffered is dropped and replaced by a new seq. The hole is retired by gap timeout. No branch keeps an expired command at the
  head of the queue without advancing the ack.
- A V1 `Snapshot` is full state, not a delta. A 4-player Village00 snapshot is estimated at **829 B** typical and
  a **5,692 B** ceiling over the enforced V1 domain, under `MAX_SNAPSHOT_BYTES=16 KiB` (ADR 0009 §4.2). This is
  design, not a runtime measurement. The ceiling uses real postcard varints (u32 5 B, u64 10 B, cell 2 B, Vec prefix) and
  the enforced `15×13` domain and counts. Coordinates `x`/`y` are `Mpx` (1/1000 px, **not** a cell index): domain
  `0 <= x < width·tile_mpx`, `0 <= y < height·tile_mpx` (V1: `x<600_000`, `y<520_000`), so zigzag is 3 B.
  If the decoder does not enforce the domain, the `i32` bound of 5 B gives a total of **845/5,708 B**.
- `snapshot_seq`/`round_id`/`content_epoch`/`rules_hash` must match the session. The full `content_sha256` is only in the
  handshake. The client drops an old or duplicate snapshot and sends `SnapshotRequest` when it needs a resync.
- The client predicts its own movement from the confirmed snapshot and replays commands that are not yet acked. A bomb is only a waiting ghost.
  Explosion, trap, death, and items follow server events only (ADR 0005). A full snapshot makes resync simple. A later delta
  needs a baseline, a resync, and a new ADR.
- `INPUT_*`/`SNAPSHOT_INTERVAL`/`MAX_*` are **protocol parameters** in a versioned `ProtocolLimits`. They are not
  gameplay rules and they are not in `Rules`. Tick and round still come from `Rules`. Do not scatter constants through the code.
- A snapshot needs RNG-state and allocator-next getters from core (contracts-r4, integrator). Without them a full snapshot cannot
  be implemented. Do not edit `crates/boom-core` inside task #14.
- Trusted room commands (`ready`/`start`/`leave`, reserved kinds) are separate from high-rate match input. Both have
  identity and dedup, with different expiry policies. Room, start, and disconnect policy come from SCOPE_V1.
- The ADR index and the "Protocol codec" row in §8 are updated by the integrator when ADR 0009 is accepted. Task #14 only edits the new
  ADR and this section, within the issue's scope.

#### `boom-room` API (proposed by task #13, integrator approves)

`boom-room` is a pure state machine. No sockets, clock, or storage. The entry point is `Lobby::apply(RoomCommand)`:

- The server binds `PlayerId` and `RequestId` onto the command. The same request as the most recent success returns
  `Outcome::Duplicate` (no state change, no event). An older request returns `StaleRequest`.
  A failed command does not change state, so it does not record the request ID. The same ID may be retried after the condition is fixed.
- `RoomPolicy { max_players, min_players_to_start }` is passed in by the server. The crate has no default.
  This task does not add `room.*` keys to `data/rules`, because that is outside the file scope. P7 must connect
  policy from the rules profile and SCOPE_V1 §3 before real use.
- `Lobby` guarantees one person is in at most one room, and issues `RoomId`/`RoundId` in increasing order. Join takes the lowest free slot.
  The next host is the earliest remaining member.
- Phases `Waiting`/`Started`. Start locks `RoundMember { player, slot, character }` exactly once.
  `Loading`/`Playing`/`Results`, the loading barrier, disconnect, waiting-room chat, and spawn belong to V1-009/010/022/023.
- A roster or character change while `Waiting` emits `ReadyReset` and clears every ready. The host must also be ready
  (an intentional difference from the original client, RULES_V1 R-063). A repeated start command does not create a second round.

### 5.5 Boundary scenarios

Acceptance scenarios live in `fixtures/scenarios/<system>/*.toml` and run with `cargo test -p boom-scenario`
(format in `crates/boom-scenario/src/scenario.rs`). `active` must pass. `pending` is the starting acceptance
for a feature that is not built yet. A scenario can prebuild bombs, items, flames, and hidden items, so each system is tested independently of
the others. The worker who takes a feature moves its scenario to `active`, adds edge cases, and the reviewer
checks the scenario against RULES_V1, not only against the code.

The blast channel is **per-tick** data, so it is not in state. A scenario prebuilds it with the
`[[blast]]` key, and the harness runs the tick through `SimulationState::step_with_scratch` (the gameplay path
`SimulationState::step` uses an empty scratch). This fixture exists only to test the `blocks` system by itself.

## 6. Authority, prediction, and side effects

Follow [ADR 0005](../decisions/0005-authority-and-prediction.md). The server decides every result. The client predicts
its own movement and replays unacked input when a snapshot arrives. A bomb is only a "waiting" ghost in presentation.
Other players are interpolated. Sharing the core does not create anti-cheat by itself. Rate limit, validation, and auth/session remain
the server's job.

Audio, effects, and UI consume events through a presentation journal that has IDs. A prediction replay does not record economy, result logs,
or send a notification a second time. The round result is committed once by authority. A late snapshot from an old round is dropped
by round ID. Network acceptance uses the SCOPE_V1 fixture (RTT, jitter, loss, duplicate, reorder, resync,
loading, disconnect). Besides the convergence hash, measure correction, input latency, and that audio and effects do not repeat.

## 7. Asset pipeline, animation, and Vietnamese localization

Pipeline: pack manifest and hash → decode with provenance → resolve the dependency closure → runtime schema →
bundle, version, and hash → validation → client. P4 ports pack reading into `boom-assets` from the format already reverse-engineered
(`boom-re-l10n/tools/idf.py`, `boomkr-re/rust/krpack`). Rewrite it. Do not copy the code. Bytes that decode correctly do not
prove that frame assembly or timing is correct.

The V1 content closure connects Village00 to tile, object, and collision; C00/C01 to Attr and animation; and every bomb,
flame, item, trap, and death state. Do not skip an unknown field or asset silently. PNG and atlas need a pixel check.
Animation and layout need their own scene fixture. The renderer takes a presentation snapshot plus animation metadata.
Render cadence is separate from the simulation tick.

Vietnamese strings come from the `boom-re-l10n/l10n/` catalog, with an explicit key mapping. An image that contains text enters the V1 bundle only after a
review decision. Unicode font, fallback, wrap, and focus are the UI contract. The Vietnamese-localization golden is separate from the original-client golden.

## 8. Decisions still open

| Decision | Status | Gate |
|---|---|---|
| Client stack | **ADR 0010: macroquad, conditional** | Still open: real build and run on 3 OSes, held-input and focus, audio playback (P6) |
| Persistence | **ADR 0011 (Accepted)**: SQLite (`rusqlite`) + Argon2id + a session token bound to `instance_id`/`connection_id` (released on disconnect, timeout, or restart; the account is not locked until TTL); client options stored on the client; runtime health through a loopback admin listener plus a separate offline check | Auth and health messages: ADR 0009 Addendum A. Still open: TLS (ADR 0006), a real backend test in P7 |
| Protocol codec | **ADR 0009** | Implement `boom-proto` and `boom-net` in P7 |
| Content bundle format | Open (P4) | Provenance, validation, asset closure, reproducible rebuild |
| Detailed order inside a tick | Open (START-C, Q-006) | Boundary scenarios for pickup, flame, chain, and death on the same tick |

Already decided: time and numerics (ADR 0004), authority and prediction (ADR 0005), default transport (ADR 0006),
extension model (ADR 0008). Waiting on review when this was written: identity, persistence, and log —
[ADR 0011](../decisions/0011-identity-persistence-log-v1.md) (Q-011).
The V1 backend stores only identity and options. Room and round are ephemeral, per SCOPE_V1 (see ADR 0011).
