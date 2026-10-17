# Remastered movement on the ground

What the port runs on the ground when "Remastered movement" is on
(`remastered_movement`, on by default, Controls > Controller settings > Stick aim,
`MP_REMASTERED_MOVEMENT=<0|1>`; see `docs/SETTINGS.md`) and the player is unmorphed.
Addresses are Remastered 1.0.0 offsets, as elsewhere in these notes.

## What is implemented

The grounded path is `CGameCollision::MoveGroundedRemastered`
(`platform/port_remastered_movers.cpp`, dispatched from
`CGameCollision::MovePlayer`): the port of
`CPhysicsGroundMoverColliderMP1::MoveObject` 0xf923d8 with its `MoveAlongGround`
0xf92ccc and `SnapToGround` 0xf934ec passes, over **static terrain only**. The
decisions are pure and header-only
(`platform/include/port_remastered_ground_policy.h`), unit-tested by
`tests/port_remastered_ground_policy.cpp`: the time search, the wall and floor
response, the up/down step probe, the snap selection and the retained ground/air
state. Contacts come from the port's own terrain producers, not from a retail
contact list.

Per tick the mover saves the loop-top prediction (the absolute snap target and the
whole motion state, tracked separately), runs up to five `MoveAlongGround` passes
(each with a bisection time search, the wall pass, then the floor pass only when no
wall was accepted), runs the terrain `DoCollisionFailsafe` 0xc1fb88 on every loop
exit, snaps to the selected contact, and then sends the source's post-snap
messages (`kSM_OnFloor` / `kSM_Falling`, the surface material, the floor normal,
the ground-collider material).

The integrator is the **player's own split one** (the local class with vptr
0x20e7340), not `CPhysicsEulerIntegrator`: `Pre` 0xc59814 folds the entry impulse
into the momentum before the move; every `PredictMotion` 0xc23e40 in between is
read-only on the linear state, so the tick moves at the pre-force velocity; and
`Post` 0xc5994c adds `dt * (force + persistentMomentum)` after the loop and the
snap, before the transient clear and the messages. The angular integration is the
port's existing one.

The retained ground/air result — the Remastered persisted physics object state at
`CPhysicsState+0x13c` (read as `obj+0x1224`, written through `SetObjectState(1)`
0xc162fc), one of the fields the movers persist and the one this path consumes — is
tracked in `CPlayer`, so a refused snap resends the previous message exactly as
`PostPhysicsUpdate` 0xc545bc does.

## What is not implemented or verified

- **The retail path is untouched.** With Remastered movement off (`rm0`), in
  classic controls, in morph ball, on a save-state load, or on any tick the mover
  stands aside, the tick runs the original `CGroundMovement` /
  `MoveGroundCollider_New` code. Nothing was changed inside it, and there is no
  switch on this path.
- **Unsupported cases fall back to retail; they are not approximated.** The
  Remastered mover does not run on a tick with dynamic contacts (there is no
  Remastered producer for them), in morph ball, on a save-state load, or when the
  mover gate in `CGameCollision::MovePlayer` is closed. Those ticks run the retail
  mover and mark the retained ground/air result invalid; it stays invalid until a
  verified Remastered airborne tick writes 0 again, so a stale entry state is never
  reused. The dynamic-contact test is the port's own: the mover's query region is
  overlap-tested against the near list with the actor's material list, so a detected
  dynamic contact does use retail — but the source's material-filtered near-list
  coverage, i.e. exactly which dynamic colliders are caught and which pass through,
  is **not** verified.
- **The source loop is exact; the terrain side feeding it is not.** The five-pass
  `MoveObject` loop, its bisection time search, the wall pass, the floor pass (only
  when no wall was accepted) and the snap ladder are a term-for-term port of
  `MoveAlongGround`/`SnapToGround` and are unit-tested. What is **not** verified is
  the terrain tree walk that supplies their contacts: the port builds them through
  its own octree cache (`TerrainContacts`), so the contact *traversal order* and the
  resulting per-contact sequence are the port's, not the source's tree walk — a tick
  with several terrain contacts is not proven to visit them in the source's order.
  Platform riders and the angular integration are likewise still the port's existing
  behaviour.
- **Resting pose.** The Remastered snap rests the box bottom at the selected
  contact height **+ 0.002**, where the retail mover's step-down snap ends at
  contact **+ 0.0005**. The 1.5 mm difference is each mover's own source constant
  and is not tuned away. Because the shipped airborne mover's bisection samples are
  discrete, that higher resting pose can make a jump land **one tick later** than
  with the retail mover at 60/120 Hz.
- **No parity claim.** No Remastered/Eden capture has been compared against this
  path, and there is no per-tick comparison beyond the port's own tests and
  telemetry. Ground replacement being wired is not the same as matching
  Remastered.

## The airborne landing query for box actors

The airborne mover (`CGameCollision::MoveAirborneStepped`, source `MoveObject`
0xf943b4 / `DoMovementStep` 0xf948d4) calls the source's landing check,
`CPlayerMP1::GroundCheckForOffGroundMoverCollider` 0xc6d110, from its post-step
down probe while rising and on a floor contact. Its terrain half is the port's own
contacts and runs first; its **dynamic half** now has a source-exact path for one
box actor, decided by the pure, header-only
`platform/include/port_remastered_box_landing.h` (unit-tested by
`tests/port_remastered_box_landing.cpp`) under the pre-select in
`CGameCollision::MoveAirborneStepped` (`Ground::Check`). The dynamic walk starts
from whatever `low` the terrain half already found, so a terrain point at or above
a dynamic one keeps priority.

When exactly one near-list candidate is bounds-overlapping the probed player box
and is active, non-platform and a box primitive (`AABX`), that pair alone answers
the dynamic landing check:

- The impacts are the source's paired ones, `CCollidableAABox::AABox_AABox`
  0xbf61b4 through 0xbf6650 (`BoxBoxPairedImpacts` in
  `platform/include/port_collision_failsafe.h`): each half carries its owner's
  outward face normal, the shared depth and one face-centre point of the
  intersection face, in the source's axis order. The walk reads the other half's
  point.
- The pair is gated the way `CheckCollision` 0xc1ec60 gates it: the candidate's
  own collision filter against the player's actor material, then the player's
  filter against the candidate's material, both through
  `CIncludeExcludeCollisionFilterMP1::Passes` 0xcb7084 and both against the
  translated material form (`CPhysicsActorMP1::MaterialListChanged` 0xc53ccc /
  `NGamePhysicsUtilsMP1::TranslateMP1Material` 0xc59b70), never against the
  port's raw material-list bits. Each candidate impact is then filtered by
  `SurfaceCollisionPasses` 0xcb7110 with the other half's outward normal.
- A point is accepted on the source's strict height band
  `pz <= feet && pz > low`, and advances `low` whether or not it grounds. It
  grounds only when `is_floor(orientation, up)` 0xbecb40 holds **and** the
  material passes the eligibility `(behavior & 0x10) == 0 || (filter & 0x2) != 0`:
  a material whose behaviour has 0x10 (the `NoPlayerCollision` value, which
  `Character` without `SolidCharacter` also produces) cannot ground unless its
  Platform channel 0x2 is set.

**Everything else keeps the pre-existing retail landing query, unchanged.** A
second bounds-overlapping candidate, another primitive type, a `CScriptPlatform`,
an inactive actor, non-finite or inverted candidate bounds, and the source's
physics-object type 2 classes (`Door`, `Dock`, `BloodFlower`, which skip the
candidate-filter visit test at 0xc1ed44 and so cannot use the gated path) all run
the port's original `DetectDynamicCollision` block with its retail
`AABoxAABoxIntersection` contact shapes. No bounds-overlapping candidate at all
leaves the terrain's result standing. The path is part of the existing
`remastered_movement` switch and adds no setting.

- **Not proven: the broadphase and its order.** The near list is the tick's
  collider list (the port's `CStateManager::BuildColliderList` when none is
  given), so its contents and order are the port's; the source's
  material-filtered near-list coverage and traversal order are **not** verified.
  Only the pair, once exactly one candidate is selected, is claimed to be the
  source's.
- **Not source: the response, the rider and the terrain classification.** The
  response half still runs over the port's native dynamic contacts, platform
  riders are the port's existing code, and the terrain half of the same check
  still classifies with the port's raw-bits rule rather than the translated
  material, so a grounded verdict can come from a pair the response producer
  never saw.
- **The grounded mover is untouched by this.** Its own dynamic-contact fallback
  and static-terrain scope above are unchanged: only the airborne landing query
  has a source path, and a morph ball or a gate-off tick still runs retail end to
  end.
- **The morph-ball retained result is not implemented** for this path, as before.
- **The runtime evidence is narrow, and no parity claim follows.** On Linux, in a
  private Ruined Fountain run, the dynamic half was reached with exactly one active
  static `CScriptActor` box: two repetitions of 285 samples each, with the samples and
  the plan point identical between them (the same plan point, not an exact-tick
  comparison). Under GDB the new pair path and its kernel call were entered for two
  landings, for that one actor only. Nothing here has been captured from Remastered,
  run on another platform, or compared for general ground, ball, response or rider
  parity.

## The ball's ray-vs-capsule query (prerequisite, not wired)

`NCollisionRayCast::Capsule` 0xbf6f10 — the ray-vs-capsule query the Remastered ball mover
sweeps — exists as a pure geometry module, `platform/include/port_remastered_capsule_ray.h` /
`platform/port_remastered_capsule_ray.cpp`, with the source's own dependencies:
`CMath::SolveQuadratic` 0x699b0 (`Source::SolveQuadratic`), the infinite-cylinder intersection
0xbf73c0 (`Source::InfiniteCylinder`) and the inline `frecpe` + two `frecps` reciprocal of
0xbf70bc (`Source::Reciprocal`, its bare estimate exposed as `Source::ReciprocalEstimate`). Its
output follows the source's quirks rather than a cleaned-up geometry: `tOut` is output-only (a
miss leaves it untouched, and the perpendicular branch reports `max(tNear, 0)` while ignoring
the ray length), the capsule epsilon is 1e-5 rather than the quadratic's FLT_EPSILON, a clipped
parameter below 1e-5 returns "inside" without writing, cap-inside candidates are gated on a
positive running best, and everything runs in Remastered axes with the public entry converting
once.

**It has no caller.** Nothing in the game links `port_remastered_capsule_ray`; the library is
built and tested standalone (`tests/port_remastered_capsule_ray.cpp`). There is no swept
capsule/box contact producer, no ball mover and no ball result on it, so it changes no gameplay
and makes no parity claim. Its rows are pinned bit-for-bit against the game's own AArch64
instruction bodies run under qemu-aarch64 — the provenance, source addresses, evidence digests
and the 421-row selection rule are in `tests/fixtures/port_remastered_capsule_ray_isa.h`, which
also carries an 87-row bare-`frecpe` estimate table from the same run — and the finite-input
contract in the header (unit direction, nonnegative radius and length, no intermediate
overflow) is what the kernel is claimed for; NaN and overflow rows are outside it.
