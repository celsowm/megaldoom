# Enemy AI Improvement Plan

## Status

**Phase 0 and Phase 1 shipped 2026-08-30** (§5 and §6). Phases 2-5 (§7-§10) and
the deferred facing cone (§9) remain proposed only. This document is the result
of reading our live AI (`src/billboard/billboard_enemy.c`, `billboard_combat.c`,
`billboard_internal.h`) against id Software's original monster thinker
(`.externals/DOOM/linuxdoom-1.10/p_enemy.c`) and against the constraints this
project has already committed to elsewhere.

### Phase 0 + Phase 1 verification results (2026-08-30)

- `npm run test`: full suite green, including the extended
  `test-active-battle-perf.py` (saturating-subtract wiring, the
  `DUMMY_MOVE_INTERVAL >= 3` invariant, the `tics > 0` walk-cadence guard
  exercised at 1 vblank/iteration, and the death-sequence/walk-cadence timing
  bounds computed from an exact simulation of the real 35 Hz accumulator, not
  estimated).
- `npm run check`: work RAM unchanged (44880/65536 used, 20656 free — neither
  phase adds a struct field), ROM code/data *shrank* ~768 bytes (Phase 0's box
  rejects removing more code than they add).
- Cadence probe on `checkpoints` (`CADENCE_STAGE_PROBE`, release-cadence build,
  not `DEBUG_PERF` — see §4.2's note on why that distinction matters): baseline
  vs. Phase 0 + Phase 1 combined, both reaching all required checkpoints.
  `rebuild_frames`, `nodes_visited`, `boxes_projected`, `segs_tested`,
  `segs_drawn`, `samples_drawn` and `box_calls` were bit-for-bit identical
  between the two runs — proof both builds reached the same player pose on this
  route, the precondition §3.4's measurement-hazard rule requires. Result:
  **avg vblanks/iteration 8.00 → 7.81** (a ~2.4% improvement, not a
  regression); missed-deadline count unchanged (14/21). Phase 0's fix paid for
  Phase 1's deliberately increased movement churn with room to spare on this
  route.
- **Gap, stated plainly:** this was a 2-way comparison (baseline vs. combined),
  not the full 3-way §12.2 specifies. There is no isolated measurement of
  Phase 0's contribution alone on this route — only the combined result, which
  came out ahead.
- **Not yet done:** an in-game play test. Per standing project feedback, a
  static/simulated result does not settle a *feel* question — the definition of
  done in §13 is not met until the user has played it and confirms enemies now
  behave the same standing still or running.

`.externals/cpp-doom` was also checked and is **not** a useful reference here: it
carries renderer, level and menu scaffolding but no monster AI thinker at all.
Every "Doom does X" claim below cites `linuxdoom-1.10/p_enemy.c`.

## Revision note (2026-08-30)

A complexity self-review after the first draft changed three things, and they are
worth flagging because two of them contradict the draft:

- The draft claimed Phase 1's "cost stays flat". That is true of the decision
  pass and **false overall** — corrected in §6.3.
- The AI's only super-linear structure (the O(S²) separation pair loop) turns out
  **not** to be the bottleneck. The dominant term is linear with a fat constant.
  A spatial index would optimize the cheap side. See §4.
- That constant is a real, isolated fix, and it is now **Phase 0** (§5), a
  prerequisite rather than an optimization, because Phase 1 spends exactly the
  budget it frees.

## Correction to the verbal summary given on 2026-08-29

That summary claimed enemy AI runs "~5x slower in combat" and ranked it as the
top defect. The wall-clock half of that is right; the framing was wrong, and the
number that actually matters is **2x, not 5x**.

The reason is [main.c:426](../src/main.c#L426):

```c
elapsed_frames = (elapsed_vblanks > 4) ? 4 : elapsed_vblanks;
```

The player's own 35 Hz movement simulation is fed the **clamped** delta, so the
player slows down on a heavy frame too. Both clocks degrade; the enemy's just
degrades twice as fast. The corrected arithmetic is in §1.2. The defect is real
and still worth fixing first, but the plan must be built on the true ratio, not
the flattering one.

## Goal

Make enemies behave consistently and legibly at every framerate the game
actually runs at, and close the gaps against Doom's monster AI that cost the
most perceived quality per byte of ROM and per 68000 cycle.

## Non-Goals

- Multiple enemy archetypes. There is exactly one (`BILLBOARD_TYPE_DUMMY`,
  a Doom trooper/`MT_POSSESSED` analogue). Ranged attacks, projectiles,
  `P_SpawnMissile`, and the rest of Doom's roster are out of scope.
- Infighting (§11.2).
- Any change to the pair-separation pass, which is a deliberate improvement over
  Doom and must survive this work (§11.1).
- Introducing a PRNG. This is a hard constraint, not a preference (§3.1).
- Changing the renderer, the billboard projection path, or the sprite atlas,
  except where §10.1 explicitly costs an atlas regeneration.

---

# 1. Current architecture and the timing defect

## 1.1 Two clocks, only one of them correct

The main loop runs a variable number of vblanks per iteration. Per the
established ground truth for this project, an idle iteration is ~2 vblanks
(30 fps) and a heavy motion iteration — one doing a full base-plane redraw — is
~10-11 vblanks.

Against that, the codebase currently has **three** different timing disciplines:

| Discipline | Unit | Used by |
|---|---|---|
| Real vblanks, saturating subtract | wall clock | `shot_cooldown`, `g_weapon_flash`, `death_lockout`, `bsp_update_doors`, and (as of 2026-08-29) `death_timer` |
| 35 Hz tic accumulator on clamped `elapsed_frames` | Doom tics | player movement, turning, weapon bob ([player_controller.c:326-333](../src/player_controller.c#L326-L333)) |
| **Raw loop iterations** | *nothing meaningful* | **the entire live enemy AI** |

`billboard_update_enemies` is called exactly once per iteration
([main.c:707](../src/main.c#L707)), and inside `update_dummy_alive` every counter
is decremented by a literal `1`
([billboard_enemy.c:156-180](../src/billboard/billboard_enemy.c#L156-L180)):

```c
if (object->move_cooldown > 0)   { object->move_cooldown--; }
if (object->attack_cooldown > 0) { object->attack_cooldown--; }
if (object->spot_cooldown > 0)   { object->spot_cooldown--; }
if (object->attack_anim > 0)     { object->attack_anim--; }
...
if (object->anim_timer == 0) { advance walk pose } else { object->anim_timer--; }
```

An iteration is not a unit of time. The comment on `ENEMY_WALK_HOLD`
([billboard_internal.h:165](../src/billboard/billboard_internal.h#L165)) says
"frames at the locked 30fps" — that lock is exactly what does not hold, and it is
the same wrong premise that produced the death-animation slowdown fixed on
2026-08-29.

## 1.2 The corrected arithmetic

Player simulation tics credited per iteration is `elapsed_frames * 35/60`, with
`elapsed_frames` clamped to 4:

| | vblanks/iter | iterations/sec | player tics/iter | player tics/sec | enemy AI steps/iter | enemy steps/sec |
|---|---|---|---|---|---|---|
| Idle | 2 | 30 | 1.17 | 35.0 | 1 | 30.0 |
| Motion | 11 | 5.45 | 2.33 (clamped) | 12.7 | 1 | 5.45 |

So, wall clock, the enemy AI drops **5.5x** while the player's own simulation
drops **2.75x**. Relative to the clock the player is actually experiencing, the
enemy runs at exactly **half speed** on a motion frame — exactly, because the
clamp is 4 and an idle frame is 2.

The gameplay consequence is perverse and matches the "AI is not good" report:

- **Stand still** and enemies close in and attack at full, tuned speed.
- **Run** — the only reason motion frames are slow is that you are moving — and
  every enemy's pursuit, reaction and attack rate halves relative to you.

Fleeing makes your pursuers relatively slower. That is backwards, and no amount
of retuning the `DUMMY_*` constants fixes it, because the error is a ratio that
moves with the framerate.

Concrete effects at the shipped constants:

| Constant | Value | Idle | Motion | Doom equivalent |
|---|---|---|---|---|
| `DUMMY_MOVE_INTERVAL` | 5 | 0.17 s/step | 0.92 s/step | — |
| `DUMMY_SPOT_DELAY_FRAMES` | 12 | 0.40 s | 2.20 s | `reactiontime` |
| `DUMMY_ATTACK_COOLDOWN` | 30 | 1.00 s | 5.50 s | attack state tics |
| `ENEMY_WALK_HOLD` | 4 | 0.13 s/pose | 0.73 s/pose | 4 tics = 0.114 s |

The walk cadence line is the visible one: the leg cycle is close to Doom's when
idle and crawls at ~1.4 poses/sec while you move. That alone reads as broken
animation before any behavioural judgement is made.

---

# 2. Feature comparison against `p_enemy.c`

| Capability | Doom (`p_enemy.c`) | MegalDoom today | Plan |
|---|---|---|---|
| AI clock | 35 Hz tics, `P_MobjThinker` | raw loop iterations | §6 |
| Chase pathing | `P_NewChaseDir`: diagonal → axes → old dir → randomized 8-way sweep → turnaround ([L363](../.externals/DOOM/linuxdoom-1.10/p_enemy.c#L363)) | two axis-aligned tries, then give up | §7 |
| Direction memory | `actor->movedir` persists | none | §7 |
| Sound alerting | `P_NoiseAlert` / `P_RecursiveSound` flood ([L105](../.externals/DOOM/linuxdoom-1.10/p_enemy.c#L105)) | none | §8 |
| First-contact perception | 180° forward cone, `allaround=false` ([L498](../.externals/DOOM/linuxdoom-1.10/p_enemy.c#L498)) | omnidirectional LOS | §9 |
| Post-engagement perception | omnidirectional (`allaround=true`) | omnidirectional | already correct |
| Target lock after damage | `actor->threshold` | none | §10.2 |
| Pain reaction | pain state + `A_Pain` sound | movement stun only, no pose | §10.1 |
| Damage variance | `((P_Random()%5)+1)*3` | flat `PLAYER_HIT_DAMAGE 20` | §10.3 |
| Melee/missile split | `meleestate` / `missilestate` | single melee-ish attack | out of scope |
| Anti-overlap | none (monsters jam) | pairwise separation pass | §11.1 — keep ours |
| Infighting | yes | no | §11.2 — declined |

---

# 3. Constraints that shape every phase

These are pre-existing project commitments. Any design that violates one is
wrong regardless of how good it looks in isolation.

## 3.1 No PRNG — determinism is load-bearing

[weapons.h:57-62](../src/weapons.h#L57-L62):

> This walks that same set on a module-static counter instead of a PRNG: the
> BlastEm route harness replays fixed input and compares outcomes, so combat has
> to be reproducible run to run. Same distribution, same mean.

`weapon_roll_damage()` ([weapons.c:46-53](../src/weapons.c#L46-L53)) is a 3-entry
cycle `{10, 5, 15}` over a static index. **Every** "randomness" this plan
introduces — `P_NewChaseDir`'s direction sweep, attack damage variance, pain
chance — must use the same deterministic-cycle technique. Doom's `P_Random` is
itself a fixed 256-entry table walked by a static index, so this is closer to the
original than a real PRNG would be.

## 3.2 Work-RAM budget: prefer the spare bits, not new bytes

`BILLBOARD_OBJECT_COUNT` is `MEGALDOOM_MAP_MAX_ACTIVE_THINGS` = **207**
([generated_map_limits.h:11](../src/bsp/generated_map_limits.h#L11)). So every
`u8` added to `BillboardObject` costs 207 bytes.

Current guardrail state: 44880/65536 used, **20656 bytes free**.
`tools/check-rom.ps1` warns below 20480 and fails below 16384.

- Slack before the **warning**: 176 bytes — *less than one new `u8` field*.
- Slack before the **hard failure**: 4272 bytes — about 20 new `u8` fields.

Therefore: **new per-enemy state goes in `reserved_flags` first.**
`BillboardObject` currently has `u8 reserved_flags : 5`
([billboard_internal.h:213](../src/billboard/billboard_internal.h#L213)) — five
free bits per enemy at zero RAM cost. §7 needs 4 of them (`movedir`), §10.2 needs
1. That is the entire budget, and it happens to fit. Anything beyond that must be
justified against the 176-byte warning slack explicitly, in the phase that wants
it, with the `npm run check` delta quoted.

## 3.3 The AI must not become the frame-time bottleneck

Per the established perf route findings, pack and cast are co-dominant; enemy AI
is not currently a top cost. That is a budget to protect, not spend. §4 quantifies
where the AI's time actually goes, and Phase 1's design (§6.3) is chosen so that
the *decision* cost stays constant per iteration rather than multiplying by the
tic count.

## 3.4 Route-harness compatibility

`tools/routes/` contains `stationary-combat.txt` and `tour-east-combat.txt`.
Behaviour changes will move these routes' outcomes. Per the standing
measurement-hazard rule, **an A/B perf comparison across an AI change is only
valid if both builds end at the same player pose** — and an AI change can alter
where the player ends up. Any perf claim in this work must use the cadence probe
with `checkpoints`, not a naive route diff.

---

# 4. Complexity budget

Per iteration, with N = enemies in the registry, S = engaged (simulated) enemies,
B = blocking props, M = enemies attempting to move:

| Term | Source | Constant |
|---|---|---|
| O(N) | main enemy loop; LOS is generation-cached | small |
| O(N) projections | up to 2 `enemy_affects_view` per changed enemy | medium, already `DIVS.W`-optimized |
| **O(M · B)** | `is_position_blocked` → `billboard_position_blocked`, 2-4x per moving enemy | **~840 cycles/test, no early reject** |
| O(S²) | separation pair loop | ~20 cycles/pair (box-rejects first) |
| O(S) | post-separation visibility | small |

## 4.1 The quadratic term is not the bottleneck

This is the counterintuitive part, and it is the reason this section exists.

`dummies_need_separation`
([billboard_enemy.c:80-83](../src/billboard/billboard_enemy.c#L80-L83)) rejects
on the bounding box *before* it multiplies, so a pair test that fails is roughly
20 cycles. At S=20 the entire O(S²) loop is ~190 tests ≈ 3% of a 68000 frame
(~127,840 cycles at 7.67 MHz).

Meanwhile `billboard_position_blocked`
([billboard.c:493-516](../src/billboard/billboard.c#L493-L516)) has **no** early
reject and does three raw 32-bit multiplies per prop — each a `__mulsi3` libgcc
call, not a hardware opcode — inside a linear scan of every blocking prop, called
2-4 times per moving enemy.

**So a spatial index for the separation pass would optimize the cheap term** and
pay a per-frame rebuild (enemies move; the existing blockmap is static and cannot
hold them) to do it. Do not build one.

That conclusion is contingent on S staying modest, and it is measurable rather
than assumed: `s_debug_pair_tests`, `s_debug_close_pairs` and
`billboard_get_debug_simulated_enemy_count` already exist. If those show S large
enough that the pair loop rivals the collision term, this call is the one to
revisit.

The `O(M · B)` term is a pure constant-factor problem, not an algorithmic one.
Phase 0 (§5) fixes it.

## 4.2 On new assembly: not warranted

The 68000 idiom this needs is already in the tree three times over:
`player_muls_word` ([player_controller.c:90](../src/player_controller.c#L90)),
`bsp_muls_word`, and `billboard_muls_word`
([billboard.c:164](../src/billboard/billboard.c#L164)) — all the same four-line
`muls.w` wrapper. `bsp_circle_blocked` already uses its one; the billboard
prop-collision path in the *same file* as `billboard_muls_word` does not.

A new `.s` hot path would therefore duplicate an existing helper, add assembly
surface on a path not yet measured as dominant, and re-enter the validation
hazard this project already hit once (the pack asm/C differential that turned out
to be comparing asm against asm). Phase 0 is pure C whose operand ranges the
compiler can check. That is the better trade here.

---

# 5. Phase 0 — the collision constant factor

A prerequisite for Phase 1, not an optimization to schedule later. Phase 1
deliberately raises M, which is a direct multiplier on the dominant term, so it
spends exactly the budget this phase frees (§6.3).

## 5.1 The defect

`billboard_position_blocked` never got the pass `bsp_circle_blocked` got — the
latter is both blockmap-bounded and `muls.w`-based. The former, for **every**
blocking prop, with no early reject:

```c
const s32 dx = object->x - x;
const s32 dy = object->y - y;
const s32 limit = radius + type->radius;
if ((dx * dx) + (dy * dy) < limit * limit) return TRUE;   // 3x __mulsi3
```

[player_controller.c:184](../src/player_controller.c#L184) already states the
motivation, having hit the same thing: *"Keeping the actual multiply word-sized
avoids four `__mulsi3` calls/tic."*

## 5.2 The change

In `billboard.c`:

1. Add the bounding-box reject before the multiplies —
   `if (dx > limit || dx < -limit || dy > limit || dy < -limit) continue;`
   This skips almost every prop outright, and it also **bounds `dx`/`dy` into
   `s16` for the survivors**, which is what makes step 2 provably safe rather
   than merely plausible.
2. Route the surviving multiplies through the existing `billboard_muls_word`.
3. Same treatment at [billboard.c:446](../src/billboard/billboard.c#L446) (pickup
   collect radius — same shape, same file, same missing reject).
4. `dummies_need_separation`
   ([billboard_enemy.c:88](../src/billboard/billboard_enemy.c#L88)) already
   box-rejects to ±`DUMMY_SEPARATION_RANGE`, so its operands are provably s16
   today. It needs the helper only, no new reject.

**Deliberately left alone:**
[billboard_enemy.c:379](../src/billboard/billboard_enemy.c#L379) (attacker
distance) and [billboard_combat.c:169](../src/billboard/billboard_combat.c#L169)
(point-blank barrel scan). Those run on unclamped player-to-enemy deltas with no
bounding reject, so a silent s16 overflow would corrupt targeting rather than
merely cost cycles. Narrowing them needs a range argument this plan does not
have.

## 5.3 Why it is safe to do first

Phase 0 is **behaviour-neutral**: it changes how a predicate is computed, not
what it returns (the box reject is implied by the circle test it guards). That
makes it the one change in this document that can be A/B'd cleanly on a route
without the pose-drift hazard of §3.4 — which is precisely why it should be
measured on its own before Phase 1 muddies the comparison.

---

# 6. Phase 1 — put the AI on the player's tic clock

The behavioural fix. Everything after it is tuning that cannot be judged while
the clock is incoherent, so this is a dependency, not merely the highest-value
item.

## 6.1 Rejected approach: saturating vblank subtraction

The obvious move is to copy what `death_timer` now does — charge each counter
`elapsed_vblanks` with a saturating subtract. **This is the wrong fix here**, for
two distinct reasons:

1. **It overcorrects.** It would make enemy cooldowns wall-clock exact while the
   player is still clamped at 4 vblanks/iteration. Enemies would go from 0.5x the
   player's rate to ~1.5x it on a motion frame. Trading "too slow" for "too fast"
   is not progress.
2. **It does not fix chase speed at all.** `update_dummy_alive` moves the enemy
   at most one `DUMMY_MOVE_STEP` per call. Expiring the cooldown sooner cannot
   produce more than one step per iteration, so wall-clock pursuit speed stays
   capped by the iteration rate no matter what the counter does.

The death animation was a pure display timer with no movement integration, which
is why the saturating subtract was right there and is wrong here. Worth stating
plainly so the precedent is not over-applied.

## 6.2 The fix: consume the player's own tic count

`player_controller_update` already runs the correct clock
([player_controller.c:326-333](../src/player_controller.c#L326-L333)) and is
called at [main.c:484](../src/main.c#L484), well before the enemy update at
[main.c:707](../src/main.c#L707).

1. Add `u16 player_controller_tics_last_update(void)` to
   `player_controller.h` / `.c`, returning how many 35 Hz tics the accumulator
   actually fired in the last call. Set it inside the existing `while` loop; no
   new accumulator, no parallel arithmetic.
2. Change the signature to
   `billboard_update_enemies(const PlayerState *player, bool redraw_pending, u16 tics)`,
   replacing the `elapsed_vblanks` parameter added on 2026-08-29.
3. In `update_dummy_alive`, charge every counter `tics` with a saturating
   subtract instead of `--`:
   ```c
   object->move_cooldown = (object->move_cooldown > tics)
       ? (u8)(object->move_cooldown - tics) : 0;
   ```
   Same for `attack_cooldown`, `spot_cooldown`, `attack_anim`.
4. Walk cadence: advance **one** pose when `anim_timer` reaches 0, do not
   fast-forward the cycle. A 4-tic hold against ≤3 tics/iteration cannot skip a
   pose, and holding to one pose per call keeps the "every frame is seen"
   property that the death-animation fix established and its guard test asserts.
5. `death_timer` moves off vblanks onto `tics` too, so the corpse collapse and the
   live AI share one clock. `ENEMY_DEATH_HOLD_VBLANKS 9` becomes
   `ENEMY_DEATH_HOLD_TICS 5` — Doom's actual value, since a tic is now a tic.
   Update `billboard_combat.c:207` and `billboard_explosion.c:85` with it.
6. Retire the "frames at the locked 30fps" comment at
   [billboard_internal.h:165](../src/billboard/billboard_internal.h#L165) and
   restate every `DUMMY_*` constant's unit as tics in one block comment. The
   values need no numeric change: at 35 Hz they land within ~15% of what the
   idle case was already delivering, which is the case that was tuned.

Deriving the count from the player's own accumulator rather than recomputing it
from `elapsed_frames` is deliberate: it makes lockstep true **by construction**,
so the two clocks cannot drift apart under a later change to the clamp.

## 6.3 Cost: the decision pass stays flat, the movement pass does not

The first draft of this document claimed cost stays flat. That is true of the
decision pass and **false overall**; §4 is why.

What does stay flat: tics per iteration are bounded by
`ceil(4 * 35/60)` = **3**, but the perception/decision pass still runs **once**
per iteration — only the counters are charged `tics`. So LOS queries, projections
and the separation pair loop are unchanged in frequency. This is the deliberate
divergence from Doom, which re-runs the whole thinker per tic and can afford to.

What does not: this phase exists to make enemies move roughly twice as often
relative to the player on motion frames. M is a direct multiplier on the dominant
`O(M · B)` term. More movement also means more
`billboard_invalidate_object_visibility` (more LOS cache misses, so more
`bsp_segment_hits_wall`) and more `update.moved` results, hence more overlay
redraw requests — all landing on the frames that are already slowest.

This cannot spiral: tics credited per iteration is capped at 3 by the
`elapsed_frames` clamp, so slower frames cannot buy unbounded extra movement. It
can, however, shift the equilibrium framerate down. Phase 0 exists to pay for
that in advance, and the three-way cadence probe in §12 is what proves the trade
came out ahead rather than assuming it.

There is also a hard precondition. The single-step-per-iteration design is safe
only while movement never *needs* more than one step per iteration — i.e.
`DUMMY_MOVE_INTERVAL >= 3` (the max tics an iteration can credit). It is 5, so
there is margin, but this is an invariant and the guard test must assert it:
dropping it to 2 in a later tuning pass would silently re-cap pursuit speed at
the iteration rate and quietly reintroduce this entire bug.

## 6.4 Guard test (extend `tools/test-active-battle-perf.py`)

- Assert the counters use saturating subtraction against `tics`, not `--`.
- Assert `DUMMY_MOVE_INTERVAL >= 3`, citing §6.3 in the failure message.
- Simulate, as `death_sequence_vblanks` already does for the death animation:
  at 2 vblanks/iter and at 11 vblanks/iter, the tics credited per wall-clock
  second must match the player's within a few percent, and the walk cycle must
  not skip a pose at either rate.
- Assert `billboard_update_enemies` is called with the player's tic count and
  not with `elapsed_vblanks`.

---

# 7. Phase 2 — chase-direction fallback (`P_NewChaseDir`)

## 7.1 The defect

[billboard_enemy.c:208-254](../src/billboard/billboard_enemy.c#L208-L254) picks a
step, tries the primary axis then the other, and if **both** are blocked does
nothing and returns. There is no direction memory and no alternate route, so an
enemy pressed into a corner or against a pillar retries the same blocked step
every cooldown expiry indefinitely. This is the classic "monster hugs the wall
and never comes around the pillar" shape, and it is the most likely behavioural
contributor to the complaint after the clock.

Doom never gives up that easily ([L363](../.externals/DOOM/linuxdoom-1.10/p_enemy.c#L363)):
diagonal toward target → each straight axis (order swapped by a random roll) →
the previous direction → a randomized sweep of all 8 directions → finally the
turnaround.

## 7.2 Design

- Store `movedir` as a 3-bit field in `reserved_flags` (8 directions; reuse
  Doom's `DI_EAST..DI_SOUTHEAST` ordering and its `opposite[]` table so the
  turnaround-last rule ports directly). A 4th bit flags `DI_NODIR`. Zero RAM cost
  per §3.2.
- Port `P_NewChaseDir`'s ladder, substituting a deterministic cycle counter for
  each `P_Random()` decision per §3.1: the axis-order swap
  (Doom: `P_Random() > 200`) and the sweep direction (Doom: `P_Random()&1`).
- Cap the fallback sweep. Doom tries up to 8 directions, each a full
  `P_TryMove`; ours costs `bsp_circle_blocked` + `billboard_position_blocked` per
  try — i.e. it multiplies the `O(M · B)` term §4 identifies as dominant, which
  is the reason Phase 0 comes first. **Bound the sweep to 3 candidate directions
  per tic** and let a genuinely boxed-in enemy resolve over successive tics
  rather than paying an 8-try worst case on every stuck enemy every tic. Measure
  before widening.
- Keep the existing diagonal-first behaviour: Doom's `diags[]` pick is already
  what our two-axis attempt approximates on the success path.

## 7.3 Verification

Deterministic Python simulation in a new `tools/test-enemy-chase-dir.py`,
mirroring the `death_sequence_vblanks` precedent: model a blocking grid, place an
enemy behind a pillar/corner, and assert it reaches the target within a bounded
tic count — where the current two-try algorithm provably never does. Plus a
headless BlastEm run on `tour-east-combat` to confirm no enemy is stuck against
geometry at the end of the route.

---

# 8. Phase 3 — noise alerting

## 8.1 The gap

Doom's `P_NoiseAlert` → `P_RecursiveSound`
([L105](../.externals/DOOM/linuxdoom-1.10/p_enemy.c#L105)) floods sound through
connected sectors, stopping at `ML_SOUNDBLOCK` lines, waking every monster in
earshot **regardless of line of sight**. It is why firing one shot in Doom brings
the room. Ours has nothing: each dummy wakes only off its own
wake-range-plus-LOS test
([billboard_enemy.c:130-141](../src/billboard/billboard_enemy.c#L130-L141)), so
gunfire never alerts anything you cannot already see. Combat cannot escalate.

## 8.2 Design — radius, not sector flood

A faithful `P_RecursiveSound` needs sector adjacency with per-line sound-blocking
flags. We have a BSP and a blockmap but no sound-propagation graph, and building
one is disproportionate to the payoff.

Substitute: on a weapon discharge (and on an enemy death), wake every dormant
`DUMMY` within `DUMMY_ALERT_RADIUS` by seeding `last_seen_x/y` with the player's
position and setting `has_last_seen` — which drops the enemy straight into the
existing "investigate last known position" branch
([billboard_enemy.c:224-226](../src/billboard/billboard_enemy.c#L224-L226)) with
no new state machine.

Design points to settle during implementation:

- **Wall-blocked or not?** Doom's flood is stopped by geometry, so a pure radius
  test is more permissive than Doom. Gate it on
  `bsp_segment_crosses_wall`, matching what `billboard_explosion.c` already does
  for blast damage — one LOS query per candidate, only on the frames a shot is
  actually fired, and only against dormant enemies inside the radius.
- **Per-weapon radius.** Doom has no loudness model, but we have per-weapon
  defs already; the fist/chainsaw waking a room is wrong. Start with: melee
  weapons alert nothing, `DUMMY_ALERT_RADIUS` for the rest. One new field in
  `WEAPON_DEFS` is ROM, not work RAM, so §3.2 does not bind.
- Cost is bounded by the registry walk already used by
  `billboard_apply_explosion`, on fire frames only.
- **Second-order cost:** waking enemies raises S and M, which feeds the terms in
  §4. This phase is the most likely of all of them to move the frame time, so it
  needs its own cadence probe rather than riding on Phase 1's.

## 8.3 Verification

Headless BlastEm on `stationary-combat`: fire one shot and assert the count of
engaged enemies rises where it previously did not. Guard test asserts melee
weapons alert nobody and that the alert path runs only on fire.

---

# 9. Phase 4 — facing cone for first contact

Doom's dormant monsters only spot you in a 180° forward cone, with a
`dist > MELEERANGE` override so a monster you are pressed against reacts anyway
([L535-L551](../.externals/DOOM/linuxdoom-1.10/p_enemy.c#L535-L551)). Once
engaged, `A_Chase` calls `P_LookForPlayers(actor, true)` and perception becomes
omnidirectional — our always-omnidirectional model is already correct for that
half.

Ours has no facing at all: `BillboardObject` has no angle field, and the sprite
is a single-rotation billboard, so an enemy has no *visual* facing either.

**This phase is listed but not recommended for the first pass.** Reasons:

- It needs a real angle byte (207 bytes, crossing the §3.2 warning threshold),
  and `reserved_flags` is already fully committed to §7 and §10.2.
- Without rotation sprites the player cannot see which way an enemy faces, so
  the rule would be invisible and read as inconsistent wake behaviour rather
  than as stealth.

Revisit only if rotation frames are ever added to the atlas. Recorded here so the
gap is documented rather than forgotten.

---

# 10. Phase 5 — combat texture

Smaller items, ordered by value. None should be started before §6 lands.

## 10.1 Pain flinch

Doom interrupts a damaged monster into a pain state with a distinct sprite
(`POSSG`) and `A_Pain` sound. We have half of this already: `push_dummy_on_hit`
sets `move_cooldown = DUMMY_HIT_STUN_FRAMES` ([billboard_combat.c:46](../src/billboard/billboard_combat.c#L46)),
so a hit enemy *is* movement-stunned — there is simply no visible pose for it.

The atlas has no pain frame: `WORLD_SPRITE_INPUTS`
([world_assets.py:62-63](../tools/world_assets.py#L62-L63)) ships
`POSSA1 B1 C1 D1` (walk), `POSSF1` (attack), `POSSH0 I0 J0 K0 L0` (death) — 10
poses, matching `ENEMY_FRAME_GEOMETRY_COUNT`. Adding `POSSG0` means an atlas
regeneration, a new `ENEMY_FRAME_GEOMETRY` row (re-derived by
`tools/test-billboard-enemy-geometry.py`, which asserts every row), and VRAM/ROM
budget review. Cost is real but the payoff — hits reading as hits — is the
highest of this phase.

## 10.2 Target threshold

Doom's `actor->threshold` locks a monster onto whatever damaged it for a few
tics, so it does not get distracted mid-fight. With a single player target this
matters less than in Doom, but it is the mechanism that makes a damaged enemy
commit to closing instead of resuming its leash/home logic. One bit in
`reserved_flags` plus reuse of an existing cooldown byte; cheap once §7 has
established the bitfield pattern.

## 10.3 Damage variance

Every enemy hit deals a flat `PLAYER_HIT_DAMAGE 20`
([main.c:721](../src/main.c#L721)); Doom rolls `((P_Random()%5)+1)*3`, i.e. 3-15,
mean 9. Fix by adding an `enemy_roll_damage()` beside `weapon_roll_damage()` in
`weapons.c`, using the identical static-cycle technique (§3.1) over Doom's actual
`{3,6,9,12,15}` set. Note this *lowers* mean per-hit damage substantially from
20, so player survivability shifts — retune `DUMMY_ATTACK_COOLDOWN` or the set
against play, and treat it as a balance change requiring the user's judgement,
not a correctness fix.

---

# 11. Deliberate divergences from Doom

Recorded so they are not later "fixed" toward the reference by mistake.

## 11.1 Keep the separation pass

Doom has no anti-overlap system; monsters are solid and jam in doorways.
`separate_dummies` ([billboard_enemy.c:91](../src/billboard/billboard_enemy.c#L91))
pushes overlapping enemies apart every tick. This is better for this hardware —
it avoids gridlock and keeps the render/collision working set bounded — and its
visibility-caching structure is already contract-tested in
`test-active-battle-perf.py`. **No phase here may regress it.** §7 in particular
must leave the pair loop and its `SEPARATION_*` bookkeeping intact.

Note that §4.1 also declines to *optimize* it: its O(S²) shape is the loudest
thing in the complexity table and the least of its costs.

## 11.2 Decline infighting

Doom monsters retaliate against each other after accidental damage. Ours has one
archetype and one hostile faction; barrel splash can kill a `DUMMY`
([billboard_explosion.c:83-86](../src/billboard/billboard_explosion.c#L83-L86))
but no enemy can target another. Implementing it means per-object target
tracking (RAM, §3.2) for an effect that needs a varied roster to be legible.
Not worth it here.

---

# 12. Sequencing and verification

Phases are ordered by dependency, not just value. §5 before §6 because Phase 1
spends the budget Phase 0 frees; §6 before everything else because no later
tuning can be judged against an incoherent clock.

| Phase | Change | Risk | Gate |
|---|---|---|---|
| §5 | Phase 0: collision constant factor | Low — behaviour-neutral | DEBUG_PERF counter deltas + clean route A/B |
| §6 | Phase 1: tic-lockstep AI clock | Low — pattern already proven 4x in-tree | `npm run test` + new simulation asserts + cadence probe |
| §7 | `P_NewChaseDir` fallback | Medium — new per-tic cost on the dominant term | New `test-enemy-chase-dir.py` + `npm run check` delta + headless `tour-east-combat` |
| §8 | Noise alerting | Medium — changes encounter pacing *and* raises S/M | `stationary-combat` engaged-count assertion + own cadence probe; user play test |
| §10.1 | Pain pose | Medium — atlas regeneration | `test-billboard-enemy-geometry.py` re-derivation + VRAM budget |
| §10.2 | Threshold | Low | Guard test |
| §10.3 | Damage variance | **Balance, not correctness** | User judgement required |
| §9 | Facing cone | Deferred — see §9 | — |

## 12.1 Measure Phase 0 on its own, first

Build with `-DebugPerf` and read the counters that already exist —
`g_debug_prop_collision_calls`, `g_debug_prop_collision_scanned`,
`g_debug_prop_collision_candidates`, `s_debug_pair_tests`, `s_debug_close_pairs`,
`billboard_get_debug_simulated_enemy_count` — on `stationary-combat` and
`tour-east-combat`. These give real N/S/B/M values, which §4's table only
estimates. Two outcomes worth acting on:

- `scanned` >> `candidates` confirms the missing box reject is the whole cost,
  and Phase 0 should then show a measurable subtick drop on the same route.
- If `s_debug_pair_tests` is large enough to rival the collision term, §4.1's
  "do not build a spatial index" conclusion is the thing to revisit. That call is
  contingent on S staying modest, and these counters are what settle it.

## 12.2 The cadence probe runs three ways

Baseline, Phase 0 alone, and Phase 0 + Phase 1. The middle build proves Phase 0
actually bought headroom rather than being assumed to; the third proves Phase 1
did not spend more than Phase 0 freed. Per §3.4 this must be the `checkpoints`
route, never a naive route diff — and Phase 0 alone, being behaviour-neutral, is
the one clean comparison available in this whole document.

## 12.3 Everything else

Every phase must pass `npm run test` (full suite, including the untouched
`test-weapon-bob.py` / `test-hud-layout.py` invariants) and `npm run check`
(§3.2 budget), and must quote the work-RAM delta.

**Motion cannot be signed off from a static screenshot.** Per standing project
feedback, a fidelity or feel change needs the user judging real gameplay; the
headless captures and simulations above establish that the code does what it
claims, not that the result plays well. §6, §8 and §10.3 in particular each need
a play test before being called done.

---

# 13. Definition of done for Phases 0 and 1

**Phase 0:**

- `billboard_position_blocked` and the pickup-collect scan bounding-box reject
  before multiplying, and their surviving multiplies go through
  `billboard_muls_word`.
- No new `.s` file and no new inline-asm helper: the existing one is reused.
- The unclamped-delta sites (`billboard_enemy.c:379`,
  `billboard_combat.c:169`) are demonstrably untouched.
- A DEBUG_PERF subtick drop is measured on a route, not asserted.

**Phase 1:**

- Enemy cooldowns, attack animation, walk cadence and death animation all
  advance on the player's 35 Hz tic count, sourced from
  `player_controller_tics_last_update()`.
- No AI counter is decremented by a bare `--` anywhere in
  `billboard_enemy.c`.
- Enemy simulation rate per wall-clock second tracks the player's within a few
  percent at both 2 and 11 vblanks/iteration, asserted by simulation in
  `tools/test-active-battle-perf.py`.
- The walk cycle skips no pose at either rate.
- `DUMMY_MOVE_INTERVAL >= 3` is asserted with §6.3 cited in the message.
- Every `DUMMY_*` and `ENEMY_*` cadence constant's comment states its unit as
  tics, and the "locked 30fps" claim is gone from the tree.
- `npm run test` green, `npm run check` guardrails pass with the work-RAM delta
  reported.
- The three-way cadence probe (§12.2) shows the Phase 0 + Phase 1 build is not
  slower than baseline.
- The user has played it and agrees enemies now behave the same whether they are
  standing still or running.
