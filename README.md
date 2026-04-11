# mod-playerbot-bettercombat

`mod-playerbot-bettercombat` is a new AzerothCore module reserved for extended playerbot combat-control features.

This repo is intentionally starting as a minimal scaffold so the module can be versioned and developed independently from `mod-playerbot-bettersetup`.

## Design Docs

- [docs/pull-command-design.md](docs/pull-command-design.md)

## Current State

- standalone module repository under `AzerothDev/modules`
- AzerothCore-compatible script loader scaffold
- gameplay commands: `pull`, `sap`, `automark`

## Current Functionality

`pull`

- uses the commanding player's current target
- resolves the controlled main tank bot in the player's current group or raid
- resolves class-priority pull openers for tank bots
- current pull opener priority:
  warrior: `heroic throw`, then ranged `Shoot` / `Throw`
  druid: `faerie fire (feral)`, then `faerie fire`, then `moonfire`
  paladin: `avenger's shield`, then `hand of reckoning`, then `exorcism`
  death knight: `death grip`, then `icy touch`
- moves that tank into the chosen opener's real spell window and stops slightly inside max range
- waits there until any pull-time CC bots are in position for their own spells
- then fires the selected opener and lets prepared CC bots cast immediately
- holds the tank in place until the target reaches melee
- holds the rest of the player's controlled bots in a temporary passive state during that wait window
- marks the pull target with skull when it reaches the tank in melee
- also keeps skull/X combat marks updated during normal controlled-bot combat even when no `pull` command was used
- prefers the main tank's current first target for a fresh skull assignment when combat starts
- clears skull when that skull target dies
- keeps one X assignment on another active unmarked combat target when possible, and promotes X to skull when the current skull target dies
- releases the group back into normal combat once the target reaches the tank in melee

`automark`

- toggles the automatic skull/X combat-mark maintenance on or off for the commanding player
- defaults to enabled
- when disabled, the module stops auto-placing skull on pull release and stops maintaining skull/X during combat

`sap`

- resolves controlled rogue bots from the player's current group or raid
- uses each rogue bot's configured `rti cc target`
- requires a live non-combat hostile target on that configured icon
- puts the rogue into stealth
- if the rogue knows `Distract`, moves to long range, casts `Distract` 2-3 meters behind the marked target from the rogue's approach angle, then continues in
- moves into `Sap` range, casts `Sap`, and returns the rogue toward the master

## Stage 1 Limitations

- tank pull support is currently focused on warrior, druid, paladin, and death knight openers
- the support-bot hold currently freezes all non-tank controlled bots during the wait window, not only DPS and healer roles
- pull state is transient and not persisted across relog, restart, or map transfer

## Planned Direction

The intent is to use this module for combat-side playerbot controls and behaviors that do not belong in setup/spec tooling.

## Remote

GitHub: `https://github.com/AzoghMartins/mod-playerbot-bettercombat`
