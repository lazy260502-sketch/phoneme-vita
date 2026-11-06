# fpmodel - run the VM's real floating-point code without a Vita

A tiny ARM/VFP interpreter (`armsim*.py`) that executes **machine code taken straight from the built ELF**:
the interpreter's `bc_impl_*` bytecode handlers and the `jvm_*` FP helpers they call. No emulator is installed on
the build host, and Vita3K cannot run the unit-level question "does `dmul` return `a*b`?", so this fills the gap.

    debug/tools/fpmodel/run.sh          # calibration + 40 FP bytecode cases against vita-port/build/cmake/midp_vita
    FPELF=path/to/other.elf debug/tools/fpmodel/run.sh

## Why it exists

log123: `(int)(.95 * 240)` printed 0 on the VM while javac's folded constant printed 228. The reason was an ABI
mismatch between `Interpreter_arm.o` (integer registers) and the hard-float C helpers (d0/d1) - see
`phoneme-cldc/src/vm/os/vita/vita_fp_overlay.S` (+ `patch_interp.sh`, run by `rebuild_vm.sh`) and PROJECT_MEMORY
"v03.10" / "v03.11". This model is how that was found and verified: 6/40 interpreter cases correct before the fix,
40/40 after; Math.sin/cos/tan for |x| > pi/4 were wrong until v03.11 (the C++ kernels called the integer-protocol
`jvm_d2i`), 42/42 Math cases and 50/50 hard-float d2i/f2i cases after.

## What a result means

* **Calibration** (`armcal.py`) runs GCC-compiled reference functions through the model and compares with Python.
  If it does not print `PASSED (0 mismatches)`, nothing else is trustworthy.
* **Cases** (`fpcases.py`) push operands on a modelled Java stack exactly the way the handlers expect, run the real
  handler from the ELF, and compare with IEEE-754 / Java semantics.
* The model **aborts on any instruction it does not implement** instead of skipping it. An `ABORT` is "unknown",
  never a pass and never a proven bug.

## Known limits (do not over-claim)

* Not a general emulator: only the instructions that occur in these routines are implemented. Thumb code is not
  supported, so `jvm_l2d` / `jvm_l2f` (which call libgcc's Thumb `__aeabi_l2d/l2f`) ABORT on the final ELF. Their
  register marshalling was verified separately with a hard-float-contract stand-in (10/10), the libgcc part was not run.
* Every instruction added later must be checked against an independent reference. Four model bugs were found this way
  during the investigation (`movw/movt`, `rsbs` carry, `ubfx`, literal-pool reads) and each had first looked like an
  ELF bug. When a case fails, compare with a GCC-compiled copy of the same C source before blaming the ELF.
* Models floats/doubles with Python/IEEE; FPSCR exception flags, rounding modes and NaN payloads are not modelled.
* Says nothing about the JIT, threads, GC, or anything that is not pure register-level FP.
