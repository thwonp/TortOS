# Turbo

**X and Y are turbo A and turbo B.** Hold one down and it presses the button
repeatedly for you. It applies to a whole system rather than to one game.

Nine of the eleven systems have it: NES, Master System, TurboGrafx-16, Game
Boy, Game Boy Color, Game Boy Advance, Game Gear, Neo Geo Pocket and Neo Geo
Pocket Color.

The settings database holds one entry per system, keyed `turbo.<tag>` in the
library scope, with a value like `x:a~3,y:b~3`. That reads *X acts as A, pressed
three frames and released three*, so about ten presses a second at 60 Hz. Lower
is faster; `1` is a press every other frame and almost certainly too fast for
anything. A system with no entry plays with X and Y doing nothing, which is what
they did everywhere before this existed.

The shipped values are compiled into the launcher and seed the database on
first run. `tortos.elf --dump` prints them.

**Game Boy Advance also has turbo L and R, on L2 and R2,** and none of the above
applies to it: that is mGBA's own Turbo L and Turbo R, which the core declares on
the RetroPad's L2 and R2 and Diatom passes straight through. Found upstream
2026-09-30 and checked in a GBA game by Eric. It is not in the settings database and has no speed to
set here. On Game Boy and Game Boy Color, also mGBA, there is no L or R, so the
two buttons do nothing.

## Why only nine

**Turbo needs two spare face buttons, and Genesis and SNES leave fewer.**

The Brick has four face buttons, and their printed labels are what matters
here - the evdev names on this shell are crossed, and reasoning from those has
produced a wrong answer twice (see the note above the button table in
`src/platform.c`).

| physical | printed | free on a two-button console | free on Genesis |
|---|---|---|---|
| west  | Y | yes - turbo B | **no**, it is Genesis A |
| south | B | no, the console's B | no, Genesis B |
| east  | A | no, the console's A | no, Genesis C |
| north | X | yes - turbo A | yes, and it is the only one |

Turbo maps `x:a~3,y:b~3`, so it wants north AND west. A two-button console -
NES, Game Boy, Master System and the rest - uses south and east and leaves both
of those spare. **A three-button Genesis already takes west**, leaving one free
button where turbo needs two. SNES uses all four.

Measured on the device 2026-09-06 with Streets of Rage 2: west is Genesis A
(special), south is B (attack), east is C (jump), and north does nothing.

This page previously said the exclusion was because "a Genesis six-button pad
and a SNES pad use X and Y for real". True of a six-button pad, but not the
binding constraint - the core presents three buttons, and three is already one
too many.

## Why PC Engine is on the list anyway

The PC Engine core has a turbo of its own: a hotkey on buttons III and IV that
latches turbo for I and II. We do not use it.

One interaction across nine systems beats two, and diatom's works without the
core's help. The PC Engine shipped a two-button pad and the core defaults to one
(`pce_fast_default_joypad_type_p1 = "2 Buttons"`), so III to VI do not exist and
X and Y go nowhere, exactly as on the rest of the list.

**The one thing that would break that**: setting that core option to 6 Buttons
turns X and Y into real buttons III and IV, and turbo would be taking them.

## Where the pulsing happens

Diatom does it, not the emulator core, which is why it behaves the same on all
nine rather than only on the one core that happens to implement turbo. TortOS
sends the map just after RUN, because RUN resets the map to identity and
anything sent before it would be discarded by the launch it was meant for.

`turbo_period` is half a cycle in frames - see diatom's `src/env.c` and its
ADR-0028.
