# Pull Command Design

## Goal

Add a coordinated group or raid pull controller to `mod-playerbot-bettercombat`.

The first feature is a staged `pull` command:

- the command uses the commanding player's current target
- the group or raid main tank moves into valid ranged-pull distance
- the main tank performs a ranged pull on that target
- the main tank then holds position and waits for the mob to arrive in melee
- when the mob reaches the tank, the tank marks that target with skull
- if the skull target dies, the tank clears skull, promotes X to skull when available, and assigns a new X to another active unmarked combat target when possible
- while the wait state is active, DPS bots must not attack
- while the wait state is active, healers must not heal the main tank
- existing raid target icon crowd-control behavior in `mod-playerbots` must remain intact

## Why This Belongs In A Separate Module

`mod-playerbots` already has a local `pull` strategy, but it is not a coordinated group controller.

What exists today:

- `pull` is already a class strategy name in `mod-playerbots`
- that strategy mostly narrows a single bot's opener to an action like `shoot`
- raid target icon and CC logic already exist separately through `rti`, `rti target`, and `cc target` values
- `main tank` resolution already exists through `PlayerbotAI::IsMainTank(...)`
- combat suppression patterns already exist through encounter multipliers that zero out `AttackAction`, `TankAssistAction`, `DpsAssistAction`, and selected spell casts

What is missing:

- one shared pull state for the player's controlled group or raid
- a main-tank-only ranged opener
- a temporary "do not engage yet" lock for DPS
- a temporary "do not heal the tank yet" lock for healers
- a clean release condition once the mob reaches the tank

Because that is combat orchestration instead of setup/spec behavior, it fits better in `mod-playerbot-bettercombat`.

## Stage 1 Scope

Stage 1 is intentionally narrow.

In scope:

- command: `pull`
- target source: the commanding player's current target
- affected unit set: the player's controlled bots in the same group or raid
- tank behavior: ranged pull, then hold for arrival
- DPS behavior: no offensive engagement during wait-for-arrival
- healer behavior: do not heal the main tank during wait-for-arrival
- release behavior: resume normal combat once the pull target reaches the main tank in melee

Out of scope for stage 1:

- selector syntax
- queued pulls
- chain pulls
- multi-target pull sequences
- explicit CC assignments beyond preserving current RTI behavior
- persistence across relog or restart
- SOAP or web integration
- non-warrior tank pull openers

## Current Stage 1 Implementation Notes

The current live implementation is intentionally narrower than the full design:

- intercepts exact `pull` through player chat hooks before stock playerbot chat handling
- uses the player's current target
- resolves the controlled main tank bot in the player's current group or raid
- currently only supports warrior main tanks with an equipped ranged weapon and compatible ammo
- resolves the warrior's real `Shoot` spell from the equipped weapon instead of relying on the weapon item's listed range
- moves the tank to a temporary pull point just inside the verified `Shoot` max range
- fires `Shoot`
- freezes the rest of the player's controlled bots with a temporary combat `passive` hold during the wait window
- marks the pull target with skull when the tank releases the group into combat
- applies the same skull/X rotation logic during ordinary controlled-bot combat even without a preceding `pull`
- prefers the main tank's actual first combat target for a fresh skull assignment when a fight begins
- clears dead skull targets during the post-release combat stage
- maintains one X backup target on another active combat mob without overwriting existing RTIs
- keeps the warrior pinned on the pull point until the target reaches melee, then releases the group back into normal combat

One intentional simplification is still in place:

- the support-bot hold is broader than the original role-only goal and currently affects all non-tank controlled bots, not only DPS and healer roles

## Command Surface

### Player Command

`pull`

Stage 1 behavior:

- the module reads the commanding player's current target
- if there is no valid hostile target, the command fails
- if the target is friendly, dead, invalid, or not attackable, the command fails
- if there is no valid main tank bot in the player's group or raid, the command fails
- if the main tank cannot execute the current warrior ranged-pull method, the command fails

### Command Routing

The module should intercept exact `pull` before `mod-playerbots` consumes it as the existing per-bot `pull` strategy command.

Reason:

- `mod-playerbots` already uses the word `pull`
- the new feature is a higher-level orchestration command
- reusing the same visible command is fine, but the new module must win command precedence for the exact group-pull case

Recommended rule:

- `mod-playerbot-bettercombat` handles exact `pull`
- anything else continues through normal `mod-playerbots` chat handling

## Existing Playerbot Hooks We Should Reuse

### Main Tank Detection

Use `PlayerbotAI::IsMainTank(...)` and the existing `main tank` value.

Current behavior:

- if the group has a maintank flag, that player wins
- otherwise the first alive tank in the group wins

That is good enough for stage 1.

### RTI / CC Compatibility

Do not replace or modify the existing raid target icon system.

Current behavior in `mod-playerbots`:

- `rti` and `rti cc` are already separate values
- `rti target` and `cc target` already drive attack and crowd-control logic

Stage 1 pull behavior should therefore:

- use the player's current target as the pull target
- leave CC icon selection untouched
- let the tank set skull on the active pull target when the staged wait ends
- allow marked RTI targets to be handled normally once combat has started and the pull wait state has released

### Combat Gating

Use a custom multiplier pattern similar to raid encounter multipliers.

Current code already shows that multipliers can:

- allow only a narrow action set for one phase
- suppress `AttackAction`, `DpsAssistAction`, `TankAssistAction`, and offensive `CastSpellAction`
- suppress selected actions by role

That is the correct extension pattern for stage 1.

## Shared Pull State

Stage 1 needs one transient shared state object per controlled group or raid.

Recommended in-memory object:

```text
PullSession
  ownerGuid
  groupGuid or raidGuid
  mainTankGuid
  pullTargetGuid
  stage
  startedAt
  pullShotAt
  releaseReason
```

Recommended stage enum:

```text
Idle
Acquire
MoveTankToPullRange
FirePullShot
WaitForMobArrival
Released
Failed
Cancelled
```

This state should live in module memory, not in `character_settings`.

Reason:

- it is short-lived combat orchestration
- it should vanish safely on logout, map change, wipe, target death, or command cancellation

## Stage 1 State Machine

### 1. Acquire

Triggered by player command `pull`.

Validation:

- player has a current target
- target is hostile and valid
- target is in world
- commanding player has at least one controlled bot in the same group or raid
- one of those bots is the main tank
- main tank can use the stage-1 ranged pull method

Success sets:

- `pullTargetGuid`
- `mainTankGuid`
- stage = `MoveTankToPullRange`

### 2. MoveTankToPullRange

The main tank moves until it is in valid ranged-pull distance for its equipped ranged attack.

Important detail:

- do not use stock `GetRange("shoot")` as the sole source of truth
- stage 1 should compute the usable pull range from the resolved ranged attack spell for the equipped ranged weapon
- use a small safety buffer so the tank stops slightly inside maximum range instead of oscillating at the edge

Recommended behavior:

- face the target
- move into ranged pull range
- do not start melee attack yet
- do not let the rest of the group engage

### 3. FirePullShot

Once in range, the main tank performs the ranged opener.

Stage-1 preferred opener:

- use the equipped ranged weapon attack path
- resolve the proper action from the ranged weapon type, matching existing playerbot logic for `shoot`, `shoot bow`, `shoot gun`, `shoot crossbow`, or `throw`

If the ranged shot succeeds:

- record `pullShotAt`
- stage = `WaitForMobArrival`

If it fails:

- stage = `Failed`
- clear session

### 4. WaitForMobArrival

This is the core new behavior.

The main tank:

- stops advancing
- holds position
- waits for the pull target to reach melee range
- should not chase the pull target during this stage

DPS bots:

- must not attack the pull target
- must not start generic offensive casts on the pull target
- must not tank-assist or dps-assist into the pull target during the wait state

Healers:

- must not heal the main tank during the wait state
- must not cast AoE or party heals that would incidentally heal the main tank during this stage
- may still heal themselves or other members if needed, unless later design chooses a stricter global hold

### 5. Release

The wait state ends when the pull target has effectively reached the tank.

Recommended release conditions for stage 1:

- the pull target is within melee range of the main tank, or
- the pull target has the main tank as victim and is within melee engagement distance, or
- the main tank is already melee attacking the pull target

On release:

- clear the pull session
- allow normal tank, DPS, and healer behavior to resume

### 6. Failure / Cancellation

The session should abort if any of the following happens:

- pull target dies
- pull target evades or disappears
- main tank dies
- commanding player loses target before the shot is taken and the session cannot continue
- main tank cannot reach pull position
- main tank cannot execute the pull shot
- group context breaks
- player explicitly cancels later through a future command

On failure:

- clear the session
- release all multipliers and temporary action locks
- do not leave bots stuck in a hold state

## Role Behavior Rules

### Main Tank

Stage 1 primarily supports tanks that can do a ranged-weapon pull.

That means stage 1 is not yet the full final solution for every tank class.

Examples:

- warrior tank with bow, gun, crossbow, or thrown weapon: supported
- warrior tank without a working ranged-weapon path but with `heroic throw`: supported as a fallback
- paladin, druid, or death knight tank without a valid ranged-weapon pull: not supported by stage 1

Future stages can extend this further with class spell pull openers like:

- `avenger's shield`
- `faerie fire (feral)`
- `death grip` or similar spell-based pull logic

For stage 1, if the chosen main tank cannot do either the ranged-weapon path or the warrior `heroic throw` fallback, the command should fail loudly rather than silently doing the wrong thing.

### DPS

During `WaitForMobArrival`, suppress:

- `DpsAssistAction`
- `TankAssistAction` for non-main-tanks if needed
- generic `AttackAction` on the pull target
- offensive `CastSpellAction` against the pull target

The design goal is simple:

- the pull target should keep travelling to the waiting tank
- no bot should peel the mob off that line before it reaches melee

### Healers

During `WaitForMobArrival`, suppress healing on the main tank.

Because AoE heals can also transfer healing threat to the tank pull:

- group or AoE heals that include the main tank should also be blocked in stage 1

Recommended exception:

- self-preservation heals remain allowed when the healer is directly threatened

## Technical Design Direction

### Module Responsibilities

`mod-playerbot-bettercombat` should provide:

- command interception for exact `pull`
- pull-session ownership and lifecycle
- custom values or session lookups accessible from bot AIs
- pull-specific multipliers
- one or more custom actions for main-tank movement and pull-shot execution

### Suggested Internal Pieces

Recommended first-pass components:

- `PullSessionMgr`
  - stores active group pull sessions
- `PullCommandScript`
  - intercepts player chat command `pull`
- `PullMultiplier`
  - suppresses DPS and healer actions during wait state
- `PullMainTankAction`
  - moves tank into range, performs ranged pull, then holds
- `PullStateValue`
  - exposes whether the bot is in an active staged pull and what role it has in that session

### Integration Strategy

Prefer adding temporary custom strategies or multipliers over patching large parts of stock `mod-playerbots`.

Reason:

- keeps the feature isolated in this new module
- makes rollback easier
- lowers merge pain with upstream `mod-playerbots`

## Edge Cases

### Existing CC Marks

Do not consume or rewrite RTI marks.

If the player targets a mob that is also RTI-marked:

- the pull command still uses the player's target
- later combat logic can continue using stock RTI handling

### Multiple Tanks

Stage 1 uses the single resolved `main tank`.

Off tanks do not pull in stage 1.

### No Ranged Pull Available

Fail the command and report why.

That is better than letting the tank run into melee early and breaking the whole point of the feature.

### Premature Aggro

If another bot gets aggro during the wait state:

- stage 1 should still release cleanly once the wait condition is broken
- no bot should remain hard-locked after combat has obviously started

### Target Reset / Evade

Clear the session immediately.

## Acceptance Criteria For Stage 1

The feature is correct when all of the following are true:

1. A player types `pull` with a valid hostile target selected.
2. The group's resolved main tank bot moves into valid ranged-pull distance.
3. The main tank uses a ranged pull opener.
4. The main tank does not chase immediately after the ranged shot.
5. DPS bots do not attack before the mob reaches the tank.
6. Healers do not heal the main tank during the wait-for-arrival phase.
7. Once the mob reaches melee range of the main tank, normal combat resumes.
8. Existing RTI CC behavior still works after the pull starts.
9. Failure cases clear cleanly without leaving bots stuck.

## Recommended Next Step After This Doc

Implement stage 1 in the smallest useful vertical slice:

1. exact `pull` command interception
2. shared pull session manager
3. main tank ranged pull action
4. wait-state multiplier for DPS and healer suppression
5. clean release and failure handling

After that works, the next expansion should be class-based pull openers for non-ranged tanks.
