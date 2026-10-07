# Turbo

**Turbo is chosen in the game, per button, with the Turbo Assign hotkey**
(plorpos-tkh). Bind it under Hotkeys in the in-game menu. Then, in a game:

- Turbo Assign, then a button: that button is turbo - held, it is pressed three
  frames and released three, about ten times a second at 60 Hz. The press that
  picks it never reaches the game.
- The same again: that button is normal again. Any number can be turbo at once.
- Turbo Assign again before picking: cancelled.
- Turbo Assign held two seconds: every turbo button is cleared.

A, B, X, Y, L1, R1, L2 and R2 can be turbo. Directions, START and SELECT
cannot. A notice says what happened each time.

**It lasts for the game.** The in-game menu and Muse leave it alone; quitting
ends it, and the next game starts with none. Nothing is written to the
settings database.

## Why X and Y are not turbo any more

Until plorpos-tkh, X and Y were a fixed turbo A and turbo B on the nine systems
with two face buttons (`turbo.<tag>` = `x:a~3,y:b~3` in the library database),
and on Game Boy Advance mGBA's own Turbo L and Turbo R sat on L2 and R2. Both
took buttons the player wants for hotkeys. The fixed maps are gone - the
launcher drops any `turbo.<tag>` rows a card still has - and on Game Boy
Advance L2 and R2 are mapped to nothing, so mGBA's turbo never sees them.

## Where the pulsing happens

Diatom does it, not the emulator core, so it behaves the same on every system.
Diatom reports Turbo Assign to the launcher (`TURBO arm=1|0`, `btn=<name>`,
`clear=1`; diatom ADR-0045) and keeps nothing itself. The launcher holds the
game's turbo buttons and answers each change with the whole map, `b:b~3` per
turbo button, plus `l2:none,r2:none` on Game Boy Advance. It sends that map
just after RUN too, because RUN resets the map to identity.

`turbo_period` is half a cycle in frames - see diatom's `src/env.c` and its
ADR-0028.
