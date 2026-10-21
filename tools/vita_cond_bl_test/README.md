# vita_cond_bl_test

Standalone Vita app that answers one question: **which `bl<cond>` instructions
does the Vita module loader leave intact?**

Background: on a real device the J2ME VM's `bleq invoke_pending_entries` was
loaded as `0affff42` (link bit cleared, a plain `beq`) while the file image
(ld, `.velf`, `.self`) says `0bffff42`; `blne` and an unconditional `bl` next to
it were fine. See `PROJECT_MEMORY.md` (v02.58 and the follow-up).

## What it tests

45 sites = 15 conditions (`eq ne cs cc mi pl vs vc hi ls ge lt gt le` + `al`
as control) x 3 variants:

| variant | callee |
|---|---|
| A | global symbol in the same file, **before** the sites (backward, like the real bug) |
| F | global symbol in the same file, **after** the sites (forward) |
| E | global symbol in another object file |

Each branch targets a *global* symbol so the linker emits a relocation
(`R_ARM_JUMP24` for the 42 conditional ones, `R_ARM_CALL` for the 3
unconditional ones) - the same pair as in the real image. `cond = 0xF`
(`NV`) is not tested: with cond `1111` the encoding is `BLX <imm>`, not a
conditional `BL`.

For every site it checks, on the **loaded** image:

1. static: opcode/cond/L bit of the word in memory, and that the branch target
   still equals the callee address;
2. dynamic: the site is executed with its condition true and `lr` preset to a
   failure label; the callee records the `lr` it was entered with. If the link
   bit were lost the callee returns to the failure label (no hang).

## Build

```
export VITASDK=/usr/local/vitasdk
cmake -S tools/vita_cond_bl_test -B /tmp/condbl_build
cmake --build /tmp/condbl_build        # -> /tmp/condbl_build/cond_bl_test.vpk
```

Title id `CONDBL001`, separate from the player (`J2ME00001`).

## Run / read the result

Install the VPK, start it, press any button to exit. The screen and
`ux0:data/cond_bl_test.log` show one row per site:

```
 var  cond  loaded-word   L cond tgt  run  lr(obs vs exp)
  A    eq    0bffff38     1  0   ok   PASS  ........
```

`<<<` marks a site that is wrong. The summary line splits the failures into
"L bit cleared", "wrong cond/opcode", "wrong target" and "call did not link";
the last line lists conditions with any bad site.

Reading it:

* all 45 fine -> the loader does not touch these; the real bug needs a
  different trigger (size of the image, distance, position in the segment, ...).
* only `eq` bad -> EQ is special (the real bug reproduced).
* several conditions bad -> the rule is broader than EQ; the list says which.
* a variant differs (A vs F vs E) -> it depends on where the target is.

## Validation done off-device

`ld`, `.velf` and `.self` all carry the intended word for every site (checked).
The test logic was run under Unicorn (ARM emulation, no loader): 45/45 pass;
clearing the L bit of chosen sites is detected at exactly those sites and the
test does not hang. That validates the harness, not the loader - the loader
verdict comes only from running it on the device.
