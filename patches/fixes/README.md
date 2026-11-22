# Fixes

Targeted correctness fixes found while playing. Each file names what it fixes and carries the
reasoning at the top: what broke, why the console never showed it, and why the fix is correct.

- `pause_object_race.c`: the pause menu overwrites gameplay_keep, and on unpause reloads every
  object, without waiting for the RCP. The console wins that race by timing; a host renderer does
  not. Both functions now flush the scheduler's task queue before the first overwrite.
- `anti_piracy_reads.c`: two of the retail game's boot ROM checks read memory this recompilation
  does not have (the collapse's bars crash, the fishing pond locks its reel); each takes the answer
  the boot ROM would have given.
- `rcp_watchdog.c` (2026-10-07): `Graph_TaskSet00` halts the game in its fault screen when a frame's
  graphics task takes over three seconds ("RCP is HUNG UP!!"). A renderer can legitimately take
  that long (a texture pack loading mid-game takes about four), so the game froze itself. The wait
  is kept and the timeout is gone, as in the reference project.
