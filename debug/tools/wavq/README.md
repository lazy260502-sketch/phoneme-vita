# wavq - audio quality checks that run on the build host

Everything here runs without a Vita and without an emulator. They exist because the device logs have no
quality signal at all; every claim about sound quality in PROJECT_MEMORY (v03.12 .. v03.19) was measured with these.

| file | what it checks | how to run |
| --- | --- | --- |
| `test_resample.c` | `vita-port/src/vita_wav_resample.h` - the SHIPPED header: exact output length for 8..96 kHz, position monotonic / in range, incremental position drift, linear vs nearest SNR, fade-out chunk | `/usr/bin/gcc -B/usr/bin -O1 -I../../../vita-port/src test_resample.c -lm -o /tmp/tr && /tmp/tr` |
| `run_ringtest.sh` + `RingToneTest.java` + `SoundStub.java` | the REAL `vita-port/nokia_src/RingTone.java` on the host JVM: RTTTL, OTA (current bit layout), garbage rejected | `bash run_ringtest.sh` (needs JDK 8 at `/usr/lib/jvm/java-8-openjdk-amd64`) |

Notes
- `gcc` in `PATH` is the vitasdk ARM compiler; use `/usr/bin/gcc -B/usr/bin` for host programs.
- `SoundStub.java` is `Sound.java` reduced to the real `FREQ_TABLE` + `freqForNote` (the only thing `RingTone` uses), so the
  test does not need MMAPI on the host. If `FREQ_TABLE` in `nokia_src/Sound.java` changes, regenerate the stub.
- The OTA case in `RingToneTest` failed from v02.89 until v03.19 because it used the v02.88 bit layout; it now builds
  cmdLen / RTP / sound / songType / sequenceLength / pattern header / instrCount exactly like the parser reads them.
- Measuring the "buzz": Hann-windowed DFT of one steady note, power on the note's own harmonic lines vs the rest
  (= inharmonic residual). Shipped plain square: -13..-16 dB; integer PolyBLEP (v03.19): -30..-32 dB.
- Verifying ROMized Java reached the ELF: search the ELF for a stretch of the method's bytecode that has NO constant-pool
  operands (constants are rewritten by romgen), compared with the previous ELF. `blepSquare`: 2 hits vs 0
  (`15 05 04 78 15 05 15 05 68 10 0f 7a 64`).
