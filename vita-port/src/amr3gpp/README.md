# src/amr3gpp - 3GPP AMR-NB reference decoder (vendored)

`vita_amr.c` needs an AMR-NB decoder and vitaSDK has none (`sceAudiodec` only
covers AT9 / MP3 / AAC / CELP). Writing ACELP from scratch is out of scope, so
the **3GPP TS 26.104 floating-point reference codec** is used as-is.

## Provenance

- Copied from <https://github.com/unjello/foo_input_amr> (`3gpp/` directory),
  which is the unmodified TS 26.104 reference decoder plus a foobar2000 front
  end. That repository is released under **The Unlicense** (public domain), see
  its `LICENSE` file.
- The code itself is the AMR floating-point speech codec specification
  (TS 26.104, REL-4 V4.5.0 / REL-5 V5.2.0, 2003-06), i.e. the code 3GPP
  publishes for implementers.
- Files: `interf_dec.c/.h` (bitstream <-> parameter interface, also handles the
  AMR *storage* format we feed it), `sp_dec.c/.h` (speech decoder),
  `interf_rom.h` / `rom_dec.h` (tables), `typedef.h` (types).
- Only the **decoder** is included; the encoder is not. No external
  dependencies beyond `<stdlib.h>` / `<stdio.h>` / `<string.h>`.

## Local modifications (4, all marked in the source)

1. `sp_dec.c` + `interf_dec.c`: every `malloc(sizeof X)` is now
   `calloc(1, sizeof X)`. The reference code reads parts of its state that
   `Decoder_amr_reset()` never initialises (e.g. `D_plsfState.past_r_q`), which
   produced reads of uninitialised heap (`0xBEBE...`) that ASAN flagged as wild
   multiplications and, on a signal path, garbage output.
2. `sp_dec.c`, `Decoder_amr()`: one `memcpy` of the `old_exc` history
   (`(PIT_MAX + L_INTERPOL) << 2` bytes from `old_exc + L_SUBFR`) is
   `memmove`. Source and destination overlap by design; `memcpy` with
   overlapping ranges is undefined behaviour (ASAN: `memcpy-param-overlap`).
3. `sp_dec.c`, `Lsf_lsp()`: the table index `lsf[i] >> 8` is clamped to
   `[0, 63]`. A truncated or corrupted frame can drive the dequantiser outside
   `cos_table[]` (65 entries) and the original code then dereferences far
   outside it - on the Vita that is a hard crash of the whole emulator from one
   bad file. Clamping costs at most one wrong frame.
4. `typedef.h`: `Word8/Word16/Word32` are `int8_t/int16_t/int32_t` instead of
   `char/short/long`. On a 32-bit ARM target `long` is 32 bits so behaviour is
   identical there, but the original header says "valid for PC" - with a 64-bit
   `long` (host unit test) the codec's arithmetic breaks.

## Not modified

Everything else is byte-identical to upstream. Do not reformat or "clean up"
these files: the diff against upstream is the whole point of the four notes
above. If a fix is ever needed, add it to this list and to
`PROJECT_MEMORY.md` (v02.88 entry).
