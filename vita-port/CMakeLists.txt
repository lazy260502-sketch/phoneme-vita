cmake_minimum_required(VERSION 3.16)

#==============================================================================
# vita-port - J2ME/MIDP on PS Vita (clean rebuild of the platform layer)
#
# Differences from midp-vita/CMakeLists.txt:
#   * sources are this directory's src/*.c (display/input/main/pcsl)
#   * libobj_no_main.a additionally drops:
#       - lfjport_fb_export.o  (empty lfjport_refresh -> src/vita_display.c)
#       - mastermode_export.o  (empty checkForSystemSignal -> src/vita_input.c)
#   * old vita_stubs.o is NOT linked (replaced by src/vita_pcsl.c)
#   * packaged internal.config/system.config come from ./config (240x320)
#==============================================================================

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(midp_vita C CXX)

include("${VITASDK}/share/vita.cmake" REQUIRED)

#==============================================================================
# Configuration
#==============================================================================
set(VITA_APP_NAME "J2ME Player")
set(VITA_TITLEID  "J2ME00001")
# 01.01 = b152: touch support + input debug logging
# 01.02 = b153: Scheduler.cpp VITA fix - event pump driven by timer tick even
#         when no Java thread is blocked in async native (audio paths sync)
# 01.11 = MEDIA_EVENT_SIGNAL routing enabled: midp_master_mode_events.o was
#         built 08-28 without -DENABLE_JSR_135 (make doesn't track CFLAGS),
#         so END_OF_MEDIA was dropped at the switch default case and the
#         music toggle deadlocked the VM. Rebuilt with USE_JSR_135=true
#         config; jump table slot 36 now routes to StoreMIDPEventInVmThread.
# 01.30 = b170: real network stack restored (vita_net.c) + net_poll moved
#         to JVMSPI_CheckEvents + per-call net logging (net_log.txt) +
#         REAL-DEVICE INSTALL FIX: drop MEMSIZE 128 from vita_create_self
#         - fself -m is a KiB budget where 0x1000-0x12800 is System-mode
#         only and 0 is the only legal value for a normal app; 128 (0x80)
#         produced an invalid SELF control info that Vita3K ignored but
#         real hw rejects with 0x8010113D on install. Also raised the
#         sceNetInit pool 64KB -> 1MB (DNS resolver lives in it; official
#         net_http_bsd sample uses 1MB).
# 01.32 = b171: LCDUI init hang fix chain -
#         (a) pcsl_file_* app0-fallback built from the RAW path produced
#             "app0:ux0:/..." (always ENOENT): skin PNGs in the VPK's
#             data/J2ME00001/lib/ were unreachable, chameleon came up
#             with ~30 missing images and UC WEB hung in init. The
#             fallback now swaps the "ux0:/data/" prefix for "app0:/".
#         (b) per-game appdb was set by the launcher but then silently
#             discarded: runMidlet unconditionally re-ran
#             midpSetAppDir(getApplicationDir()) = MIDP_HOME + "/appdb",
#             so every suite shared the one appdb and the appdb_XXXX
#             dirs stayed empty. runMidlet now keeps a launcher-provided
#             dir (new midpGetAppDir query); storage moved under rms/
#             (rms/appdb + rms/appdb_<TAG>) with a one-time rename
#             migration of the old shared appdb.
#
# v01.33 (2026-09-08): two runtime fixes driven by the first v01.32 log
#         capture:
#         (a) sockets never became non-blocking: fcntl(O_NONBLOCK) is a
#             NO-OP on sceNet descriptors (newlib's fd IS the sceNetSocket
#             id), so connect() blocked in-syscall and froze the whole VM
#             for 27s until ETIMEDOUT (net_log errno=116 + Vita3K
#             sceNetConnect 0x8041013C at +27s). Non-blocking is now set
#             with sceNetSetsockopt(SCE_NET_SO_NBIO) at socket creation
#             (client + server), restoring the EINPROGRESS -> select() ->
#             NETWORK_*_SIGNAL wake chain the port was designed around.
#         (b) vm_output.log rendered one-char-per-line: CLDC's
#             PrintStream.write(int) invokes JVMSPI_PrintRaw PER CHARACTER
#             (length==1) and the debug hook appended '\n' per call.
#             JVMSPI_PrintRaw now writes the bytes verbatim (fwrite).
#         Note: the sceNetInit 0x80410110 (EBUSY) + sceNetResolverCreate
#         stub warnings in Vita3K are benign: newlib's gethostbyname
#         lazily re-inits the net stack on the second launch round; the
#         first init in vita_net_early_init() already succeeded.
#
# v01.34 (2026-09-08): RMS directory enumeration was a NULL stub.
#         RecordStore.listRecordStores() -> getNumberOfStores ->
#         pcsl_file_openfilelist() returned NULL -> OUT_OF_MEM_LEN ->
#         OutOfMemoryError raised from native inside UC's startup
#         state restore (observed as ao.a() NPE one frame up). First
#         launch worked (creates go through pcsl_file_open/exist,
#         both real); every subsequent launch died - deleting the
#         appdb dir "fixed" it because the listing then legitimately
#         returned empty. Implemented openfilelist/getnextentry/
#         closefilelist with sceIoDopen/Dread/Dclose following the
#         reference POSIX contract (root + match-prefix split, full
#         path results). This also un-breaks midp_remove_suite's file
#         cleanup loop and rmsdb_remove_record_stores_for_suite,
#         which iterate the same way.
#
# v01.35 (2026-09-08): silence the misleading "file in use" noise from
#         midp_remove_suite's cleanup loop. Vita3K maps SCE errno onto
#         the wrong Windows errno table when it logs io_error_impl, so a
#         plain ENOENT (0x80010002 = 0x80010000 | 2, "file does not
#         exist") shows up as "Error code: 32 (another program is using
#         this file)". The cleanup loop unlinks every enumerated entry
#         unconditionally, so a target that vanished (or is only a
#         filename prefix of a store) legitimately returns ENOENT.
#         pcsl_file_unlink now treats "already gone" as success (probed
#         with sceIoGetstat, no hardcoded code), so storage_delete_file
#         stops printing the noise and the loop no longer aborts early
#         on a vanished entry.
#
#         (net) vita_net.c: g_handles[] was a BSS global (zero-filled),
#         so na_create() never found an fd==-1 free slot and returned
#         NULL for EVERY non-blocking (EINPROGRESS) connect - the
#         "connect EINPROGRESS -> select -> NETWORK_*_SIGNAL" wake chain
#         silently died and real connects to web servers never completed
#         (net_log: socket_open OK but no connect result for reachable
#         hosts; only unreachable WAP proxy 10.0.0.172 logged the
#         immediate ETIMEDOUT errno=116). Explicitly initialised the
#         table to fd=-1.
#
# v01.36 (2026-09-08): the v01.35 ENOENT-tolerance patch was NOT enough.
#         Verified against the user's exact Vita3K build (v0.2.1
#         496939b6, io.cpp remove_file): the "Error code: 32" line is
#         Windows' ERROR_SHARING_VIOLATION message verbatim - a REAL
#         sharing violation, not an errno mis-mapping. Vita3K's
#         open_file() holds a FILE* (_wfopen, no FILE_SHARE_DELETE) for
#         every open fd, so a remove fails with Error 32 whenever ANY
#         handle in-process still refers to the file; Vita3K then
#         returns 0x80010002 for BOTH "not exist" and "in use", which
#         is what misled v01.35. POSIX allows unlinking open files
#         (phoneME's cleanup loops rely on it), Windows does not. Fix:
#         an open-handle registry in vita_pcsl.c - pcsl_file_open
#         registers, pcsl_file_close unregisters, and pcsl_file_unlink,
#         when the remove failed and the file still exists, evicts all
#         same-path handles (sceIoClose + fd=-1: the POSIX "name gone"
#         state) and retries the remove once.
#
# v01.37 (2026-09-08): REVERTED the v01.36 eviction. Making the remove
#         succeed turned out to be WRONG: midp_remove_suite's cleanup
#         loop targets the appdb entries of the RUNNING temp suite
#         (FFFFFFFF* = UC's live RMS data), and the sharing violation
#         had been accidentally PROTECTING that data across launches.
#         v01.36 evicted fds still held by Java code -> RMS files came
#         back half-written -> second launch broke (NPE, ~1 fps).
#         pcsl_file_unlink now reports the failure (file survives);
#         the registry + evict helper stay in place unused for a
#         future targeted fix (evict only on real uninstall).
#
# v01.38 (2026-09-08): init-slowness fix + noise halving. (a) The
#         per-open debug_log() (open+write+close of debug_log.txt for
#         EVERY pcsl_file_open, two lines on success) multiplied UC's
#         init I/O by ~3x - hundreds of opens during RMS/config/skin
#         restore each costing 3 extra syscalls plus a Vita3K trace
#         line per syscall. Disabled behind VITA_FILE_DEBUG (default
#         0). (b) pcsl_file_unlink no longer falls back to the app0
#         copy when the ux0 file exists but is locked: the app0 remove
#         always failed too, doubling the "file in use" log noise with
#         zero benefit. Net effect: UC data-init is much faster and
#         the remaining sharing-violation lines are one group per
#         protected suite file (expected, harmless).
# v01.40: EBADFD (0x80010051) seek storm fix
#   - VitaFileHandle.path was 256 bytes: deep UC cache paths were
#     silently truncated; pcsl_file_truncate's close+reopen then opened
#     a nonexistent path, left fd=-1, and every later seek on the
#     still-live handle returned EBADFD - UC lost its local cache and
#     re-downloaded everything ("too slow"). path now 600 bytes (same
#     as the abs_path buffers).
#   - truncate reopen failure now recovers a usable fd (no O_TRUNC)
#     so the handle never stays half-dead.
# v01.41: Vita3K O_TRUNC blind spot - the REAL I/O adaptation gap
#   - Vita3K's translate_open_mode() (io/src/filesystem.cpp) has NO
#     SCE_O_TRUNC branch: every sceIoOpen maps to "rb+" and O_TRUNC
#     is silently dropped. Our close+reopen(O_TRUNC) truncate NEVER
#     shrank anything under the emulator - the saved prefix was
#     written back over the first `size` bytes and the stale tail
#     survived forever. RMS compact "succeeded" while garbage record
#     headers lived past the new logical size -> slow/damaged second
#     launches; sizeofopenfile kept reporting the OLD size.
#   - pcsl_file_truncate now swaps via remove+create+write-back and
#     VERIFIES the new file is 0 bytes before writing (a failed
#     remove under Vita3K = sharing violation now reports failure
#     instead of silently corrupting).
#   - pcsl_file_open emulates O_TRUNC the same way for the
#     OPEN_READ_WRITE_TRUNCATE path (suite temp files).
#   - pcsl_file_seek/read/write/sizeofopenfile fail fast on dead
#     handles (fd<0) with a one-shot stderr breadcrumb, killing the
#     EBADFD syscall noise at its source.
# NOTE: real-device 0x8010113D still unresolved; eboot.bin verified
#   byte-identical to a clean `vita-make-fself -s -c` (no -m) and the
#   SFO matches VitaSDK defaults - needs A/B testing on hardware.
# v01.41a: fd leak fix in the O_TRUNC open recovery (close the new fd
#   before free on the failure path) + held-file stderr breadcrumbs.
# v01.42: long-session memory leaks - the 6h Vita3K crash hunt
#   - menu_fb: vita_menu_run() memalign'd 2MB EVERY round and the
#     static pointer overwrite leaked the old block (comment claimed
#     "once per process" but the code ran per round). Allocate once,
#     reuse across rounds.
#   - icon textures: restore_kept() parks last round's decoded icons
#     in icon_pix[]; both scan_games paths decoded straight into the
#     slot, overwriting (and leaking) the parked texture. Free before
#     decode.
#   - build.sh: `make | tail` exit code came from tail - a failed
#     make printed "Build successful" and shipped a stale VPK. Now
#     pipefail.
# v01.43: kill the residual "file in use" log triplet at the source -
#   pcsl_file_unlink pre-checks the open-handle registry and SKIPS the
#   sceIoRemove when one of OUR live handles holds the path (Windows/
#   Vita3K cannot delete an in-process-open file anyway). Same honest
#   failure for the caller, but Vita3K no longer logs
#   "Cannot remove file / Error code: 32 / io_error_impl" every round
#   for the live FFFFFFFF suite data.
# v01.44: pcsl_file_getnextentry no longer returns subdirectories.
#   The b194 log proved the remaining "Error code: 32" came from the
#   cleanup loop unlinking the appdb/FFFFFFFF DIRECTORY itself (it
#   matched the iterator's prefix and was handed back as a file).
#   Directories are skipped now (same filter getsizeoffilelist uses).
# v01.45: the b195 stderr/vita3k pair exposed TWO residual issues.
#   (a) Root cause of the 23:55 Error-32: an EMPTY-NAMED record store
#       gets NO .db/.idx suffix (buildSuiteFilename ignores the
#       extension when nameLen==0), so its db handle AND idx handle sit
#       on the SAME path (legal on POSIX). pcsl_file_truncate's
#       remove+create swap closes only its own fd - the twin handle
#       still anchors the name, Vita3K refuses the remove, and the
#       swap must be aborted. Same guard added to pcsl_file_open's
#       O_TRUNC emulation. Both now check vita_handles_held BEFORE
#       touching the name and fail fast (no doomed sceIoRemove).
#   (b) vita_report_held deduped breadcrumbs by PATH only, so the
#       later "truncate blocked" for a path already reported as "unlink
#       blocked" was swallowed and the stderr trail went cold. Dedup
#       key is now op+path.
# 01.51 (2026-09-11): "delete rms -> first launch fine, save a bookmark,
#   second launch wedged in init". The damaged part is the record store's
#   BLOCK CHAIN, not its header: bytes that are not a block header sit in
#   the LIVE data region (a stale tail survives there because Vita3K drops
#   SCE_O_TRUNC and pcsl_file_truncate is a logical clamp that dies with
#   the handle), so every walk in the RMS index advances past the end of
#   the data area and NO record is found - the MIDlet never finishes
#   restoring its state. The header alone still passed the v01.50 check.
#   (a) RecordStoreImpl.isBlockChainConsistent(): read-only chain check
#       (every block positive, inside the data area, walk lands exactly on
#       dbSize). compactRecords() now refuses to move data on an
#       inconsistent chain - it used to relocate garbage and shrink
#       data_size, the step that turned a readable store into a bricked one
#       at CLOSE time, i.e. exactly between launch 1 and launch 2 - and the
#       constructor resets a structurally broken store to a pristine empty
#       one (the automatic equivalent of deleting the store, the only
#       manual action that made the MIDlet start again).
#   (b) RecordStoreIndex.getFreeBlock(): never return an offset inside a
#       block that overruns the data area; report "no free block" so the
#       record is appended at the end instead of overwriting live data.
#   (c) vita_pcsl.c pcsl_file_seek(): report the REAL position again. The
#       v01.47 clamp told the caller the file ended at the pending logical
#       size while the fd had in fact moved past it, splitting the position
#       the file cache tracks from the position the handle is at. Hiding a
#       stale tail belongs to the read clamp and sizeofopenfile, which
#       still do it.
# 01.52 (2026-09-11): "second launch: records and bookmarks gone, same as
#   first launch". The v01.51 chain-reset ERASED the user's data: the
#   hexdump of the wiped store showed version=1 next_id=1 data_size=0 -
#   the exact signature values of the v01.51 reset block - over a 5856
#   byte file whose record blocks were still physically intact. Erasing
#   was the wrong recovery: the mismatch the chain check detects is
#   caused by the HEADER lagging the block writes (midp_file_cache holds
#   small header writes in memory while the block data goes to disk;
#   Vita3K drops SCE_O_TRUNC so a stale tail also survives), so the
#   blocks themselves are almost always complete.
#   (a) RecordStoreImpl.salvageStore() replaces BOTH reset blocks: scan
#       the physical file to EOF block by block (NOT to the header's
#       data_size - the header lies), dedupe by record id (a stale tail
#       holds the OLD copy; compact moves data down so the lowest offset
#       is newest), rewrite the live blocks densely from the top, rebuild
#       the header from what was actually recovered (next_id never
#       regresses), commitWrite() then truncate. A store with nothing
#       salvageable ends up empty - identical to v01.51 - but everything
#       recoverable is kept.
#   (b) Third damage shape detected at open: header claims an EMPTY
#       store while live blocks are physically present
#       (physicalFileHasLiveBlocks probe, payload-verified so a garbage
#       header past EOF cannot re-trigger it forever). This is exactly
#       what a v01.51-wiped store looks like, so this very device's
#       bookmarks are recoverable on the first launch of 01.52.
#   (c) isBlockChainConsistent() now also cross-checks num_live (a lost
#       header write after successful block writes made the old check
#       pass while the index found no records).
# 01.60 (2026-09-14): JSR 75 (javax.microedition.io.file / FileConnection).
#   A Vita-local subsystem under samples/j2me/jsr75 - the spec's interface
#   signatures only, every line of implementation written here. 9 Java
#   sources (5 public javax.microedition.io.file types, FileStore,
#   FileConnectionImpl, FileSystemRegistryImpl, the "file" GCF Protocol)
#   plus 21 natives in jsr75/src/share/native/jsr75_file.c, wired into the
#   MIDP build by jsr75/build/cldc_application/subsystem.gmk and
#   USE_JSR_75=true in phoneme-midp/build_vita.sh - no shared phoneME file
#   was touched, ROMImage/nativeFunctionTable pick the natives up
#   automatically because ROMGEN synthesises Java_<class>_<method>.
#
#   Root policy: one root only, the launcher's cwd ("ux0:/data/J2ME00001"),
#   so JSR 75 and the PCSL layer resolve the same paths by construction.
#   FileSystemRegistryImpl reports it as "file:///data/".
#
#   Byte I/O goes through the port's PCSL file API; the directory, time and
#   volume natives go to sceIo* directly, because the pcsl_directory group
#   v01.59 added lives in the executable (vita_pcsl.c) and is not in the
#   PCSL set libmidp.so links against - the .so link reported them as
#   undefined until they were replaced.  A missing second piece: the
#   shared-object step of the makefile also fails on the pre-existing
#   _rom_linkcheck_mffd_false, which the real (CMake) executable link
#   resolves; JSR 75 does not change that state.
#
#   Verified: javac clean (source 1.3), classes.zip carries all 4 packages,
#   ROMImage_01.cpp carries the 21 ROMized natives references,
#   nativeFunctionTable.cpp 45 mentions, and nm on midp_vita shows the 21
#   Java_com_sun_midp_jsr075_* symbols.  Not yet run on Vita3K/real
#   hardware - the FileConnection runtime behaviour is untested.
set(VITA_VERSION  "01.60")
# 01.61 (2026-09-14): JSR 75 - listRoots() handed out a URL instead of a
#   root name.  JSR 75 says a root is "a root name ending with '/'" that
#   the caller concatenates with "file:///" to build a URL; returning
#   "file:///data/" made every caller that follows the documented recipe
#   ask for "file:///file:///data/", which resolvePath() rightly rejects
#   (root name != "data") - the MIDlet sees an Alert and exits, i.e. the
#   "launched then immediately done" symptom, before it ever opens a
#   connection.  FileSystemRegistryImpl now returns "data/" and keeps
#   ROOT_URL ("file:///data/") for the protocol layer; no other JSR 75
#   path changed.
#
#   Same version, diagnostic-only: System.out tracing on the three JSR 75
#   entry points (listRoots, Protocol.openPrim, FileConnectionImpl's
#   constructor/listEntries) plus startApp enter/return/throw in
#   MIDletStateHandler.  startApp exceptions are re-thrown into
#   handleException(), which calls printStackTrace() on System.err - and
#   CLDC's System.err *is* System.out (System.java), so an exception can
#   never be silent in ux0:/data/vm_output.log; when that log shows no
#   trace the MIDlet ended normally and the tracing is what tells us how
#   far it got.
set(VITA_VERSION  "01.61")
# 01.62 (2026-09-14): JSR 75 - the spec's own "is it there?" probe.
#   microedition.io.file.FileConnection.version was never defined, and that
#   is exactly how a file manager decides whether JSR 75 exists before
#   calling into it.  MiniXplorer reaches startApp, returns from it
#   normally, yet never issues a single Connector/FileConnection call - the
#   [JSR75] tracing was live in the v01.61 run and stayed silent - and
#   leaves a screen whose only soft key is "Done", which phoneME produces
#   from AlertLFImpl alone, i.e. an Alert the MIDlet itself raised.  The
#   property now comes from config/system.config, the port-owned
#   application-property file: midp_properties_dynamic.c's
#   getSystemProperty() searches only that set, which is also why
#   internal.config's "PS Vita" does not shadow the "j2me" the Connector
#   resolution needs.
#
#   Diagnostic-only: Alert title/text and the MIDlet state loop's destroy /
#   exit paths now log, so the next run says both what is on screen and
#   whether the MIDlet is still alive.  Both sets of tracing come back out
#   later.
set(VITA_VERSION  "01.62")
# 01.63 (2026-09-14): JSR 75 - resolvePath() rejected the spec's own URL
#   syntax.  JSR 75 spells a file URL "file://<host>/<root>/<path>", the
#   host of the local device being "localhost", and a MIDlet that follows
#   the documented "file://localhost/" + root recipe (MiniXplorer does
#   exactly that with the name it just got from listRoots()) was answered
#   with IllegalArgumentException "not a file URL" - the URL did not start
#   with the hard-coded "file:///" prefix.  Every directory open then
#   failed, and MiniXplorer's own error path appended the root name again
#   and again, so the log shows an unbounded "file://localhost/data/data/
#   data/..." cascade.  resolvePath() now parses the authority and accepts
#   both "localhost" and the empty one; every other host still names a
#   device this single-root port does not expose.  No other JSR 75 path
#   changed.
#
#   The headless Alert seen in the same logs ("This application does not
#   use the screen and runs in the background.", ResourceConstants
#   LCDUI_DISPLAY_HEADLESS) is phoneME's own CldcMIDletStateListener /
#   HeadlessAlert notification, not a MIDlet diagnostic: it is only
#   brought to the foreground when startApp() returns with
#   Display.getCurrent() == null, which is why the earlier run's "Done"
#   screen was a symptom of the MIDlet dying, not its cause.
set(VITA_VERSION  "01.63")
# 01.64 (2026-09-14): JSR 75 - diagnostics out, fixes in.  v01.63 was
#   verified in Vita3K: MiniXplorer starts, listRoots() is consumed, and the
#   directories it could not open now open.  The temporary tracing that made
#   that possible is gone - the [ALERT] lines in Alert, the [MIDLET]
#   startApp/destroyApp/state-loop lines in MIDletStateHandler, the [JSR75]
#   lines in Protocol / FileConnectionImpl / FileSystemRegistryImpl - so the
#   only JSR 75 sources left modified are the two that carry real fixes:
#   FileSystemRegistryImpl (root name from listRoots, authority parsed by
#   resolvePath) and the javadoc of the javax FileSystemRegistry facade.
#   Note that MIDletStateHandler's [MIDLET] creating/created/FAILED/startSuite
#   lines are NOT tracing: they date from the 2026-08-31 baseline commit
#   (8a04358) and stay.
#
#   Keep the lesson: adding printlns to a large AOT-compiled method moves its
#   literal pool and can hit the 12-bit limit - that is what crashed romgen -
#   so future diagnostics belong in small methods or on code paths romgen
#   does not compile.
set(VITA_VERSION  "01.64")
# 01.65 (2026-09-14): two things a file manager MIDlet needs.
#
#   (1) The JSR 75 roots are the storage volumes, not the launcher's data
#   directory.  listRoots() hands out "ux0/", "imc0/", "uma0/" (the last
#   two only when the card answers a directory probe) and "app0/", and
#   file:///ux0/data resolves to the absolute path ux0:/data.  The single
#   "data" root is gone - the data directory is reachable as
#   ux0/data/J2ME00001 - so MiniXplorer can browse the whole device
#   instead of one directory of it.  availableSize()/totalSize() now take
#   the target path and report the capacity of the volume it lives on,
#   and FileStore.isDirectory() recognises a volume root even though
#   sceIoGetstat does not always set IFDIR on one (it opens the root
#   instead, which also answers whether the volume is mounted).
#
#   (2) The UI font is a J2FB v2 bank: the Latin sections come from
#   DejaVu Sans at 20 px with 8-bit coverage and per-glyph advances, the
#   CJK sections stay 1-bit DengXian at 18 px.  Latin is proportional now
#   ('.' is 5 px, 'A' is 12) and every glyph is placed against the
#   baseline, so descenders and punctuation sit where they belong instead
#   of hanging from the top of the cell.  The bank is generated by
#   tools/fontgen.c + tools/gen_font.sh and is the only font asset in the
#   VPK: the 16 MB font.ttf the old build shipped is no longer needed, so
#   the package went from 13.7 MB to 3.6 MB.
set(VITA_VERSION  "01.65")
# 01.66 (2026-09-14): ideographs are rasterised at 20 px too, not 18.
#
#   The v01.65 bank baked the CJK sections from DengXian at 18 px while
#   the line box is 22 rows with the baseline 18 rows down.  An
#   ideograph's ink is about 0.85 em, so 18 px gave only 15 rows of ink;
#   placed on the baseline it covered rows 5..19 and left 5 blank rows
#   above it.  Chinese therefore looked small and sat sunk at the bottom
#   of the line, and the pen advanced the 18 px the face asks for, which
#   is 2 px tighter than the cell.  Latin was unaffected - its ink hugs
#   the baseline - which is why only Chinese looked wrong.
#
#   At 20 px the ink lands on rows 3..20 (inside the cell: the tallest
#   ideographs reach row 20 and nothing is clipped), the section advance
#   comes out at the 20 px cell width again, and the strokes survive the
#   1-bit threshold better.  Only the CJK sections changed - the header,
#   the Latin advance tables and the Latin bitmaps are byte identical to
#   the v01.65 bank, so English is bit-for-bit the same.
set(VITA_VERSION  "01.66")
# 01.67 (2026-09-14): real-hardware boot crash fix - PIC veneers.
#   Both real-hw coredumps (v01.65 and v01.66, fw 3.65 + plugins) died
#   BEFORE main(): the loader slides the code segment (+0x72000 observed)
#   and the RW segment (+0x120000) and applies the ELF relocations, but
#   ld's default long-range ARM->Thumb interworking veneers carry an
#   absolute literal with NO relocation entry (38 veneers, 0 relocs).
#   The 4th .init_array ctor branches through ____aeabi_atexit_from_arm,
#   jumps to the stale link-time address and data-aborts - which is why
#   no log file (boot_log/crumb/net_log/vm_output) ever appeared.
#   -Wl,--pic-veneer turns all 38 into pc-relative sequences; verified
#   reloc count unchanged, Vita3K unaffected (it never slides segments).
set(VITA_VERSION  "01.67")
# 01.68 (2026-09-15): real-hw black screen after boot fix - CDRAM fbs.
#   The pic-veneer fix (01.67) got the app past static ctors on real hw
#   (boot_log.txt now appears) but the screen stayed black: both the
#   native menu framebuffer and the LCDUI vita_fb were memalign()ed
#   heap buffers, i.e. normal CACHEABLE user memory. On real hardware
#   the Cortex-A9 D-cache keeps the CPU writes in-cache while the
#   display controller scans main memory, so the panel never sees a
#   single pixel. Vita3K does not emulate caches (same class of
#   emulator-only success as the veneer bug). Both buffers now come
#   from sceKernelAllocMemBlock(USER_CDRAM_RW) via the new
#   vita_fbmem.h helper - the mapping is UNCACHED on the CPU side, the
#   pattern every official SDK sample (debugScreen/camera/ime) uses.
set(VITA_VERSION  "01.70")
# 01.71 (2026-09-15): ToneTest hang diagnostics - instrument the three
#   decision points of the "one beep then dead" freeze. Facts so far:
#   vm_output.log ends at "stg1: playTone returned" and every timed
#   waiter after that (2.5 s stage timer, 1 Hz heartbeat, 60 s
#   watchdog) never fires, yet an earlier session logged button
#   samples while the app appeared hung - so the VM thread MIGHT still
#   be pumping. Three suspects need separating:
#   H1 stuck in a blocking native before the ANI wait (repaint path),
#   H2 wall clock (gettimeofday == Os::java_time_millis source) froze,
#   H3 stuck inside ANI_WaitForThreadUnblocking.
#   Probes (all lightweight, all to ux0:/data/J2ME00001/):
#   - input heartbeat now carries ms= + CE=/ani= counters, so one line
#     proves pump liveness AND clock liveness at the same instant
#   - ani_mark.log is a watchdog slot: every ANI enter/leave pwrites
#     over byte 0; the last state written is where the VM thread sits
#   - audio_debug.log now APPENDS (was TRUNC: every hard restart
#     destroyed the previous session's tone records - the ToneTest
#     audio evidence was erased by the next app's startup)
#   - tone_player_thread logs req/done with chunk counts, and any
#     sceAudioOutOutput error code
set(VITA_VERSION  "01.71")
# 01.72 (2026-09-16): hang forensics watchdog - v01.71 verdict first:
#   log4 proved ani_mark ends "OUT" (not inside the ANI wait -> H3 out),
#   the pump froze for good at CE #115 (~4.4 s) in the last round, and
#   the tone thread finished every chunk (audio innocent). But the
#   heartbeat samples only once per 1024 pump calls (~40 s), so H2
#   (clock freeze) was never actually excluded - the hang lands at
#   2-10 s, between samples. What remains: (a) VM thread blocked in a
#   kernel wait, or (b) VM thread running hot (Java/GC loop, never
#   blocks, never pumps). ONE snapshot tells them apart.
#   New vita_watchdog.c: low-prio thread watches vita_ce_count; when it
#   stalls >3 s during a MIDlet round it snapshots the VM and tone
#   threads via sceKernelGetThreadInfo (status + waitType + waitId -
#   the wait class names the blocking primitive: io/mutex/cond/delay/
#   semaphore) and takes two gettimeofday readings 100 ms apart (d==0
#   -> clock frozen -> H2 on the spot). One report per hang, appended
#   to ux0:/data/J2ME00001/watchdog.log.
set(VITA_VERSION  "01.72")
# 01.73 (2026-09-16): watchdog v2 - log5 lesson. The v01.72 watchdog stayed
#   SILENT while the pump froze with the exact log4 signature (ani_mark
#   "OUT", pump dead after touch events). watchdog.log did not even exist.
#   Two failure modes, both fixed:
#   1. silent startup: thread-create failure returned without a trace, and
#      nothing proved the thread ever ran. Now every outcome writes a line:
#      "[WD] alive tid=... start rc=..." at startup, "[WD] create FAILED"
#      on error.
#   2. clock-blind detection: the 3 s stall threshold was measured with
#      gettimeofday - if H2 (clock freeze) is real, the threshold can
#      never be reached and the watchdog is blind to its own target
#      scenario. Detection now counts poll ITERATIONS (12 x 250 ms),
#      clock-free; the dual gettimeofday readings stay in the report as
#      H2 evidence (d==0 -> frozen clock).
set(VITA_VERSION  "01.73")
# 01.74 (2026-09-16): log6 verdict - [WD] create FAILED rc=0x80028023 on
#   every boot: SCE_KERNEL_ERROR_ILLEGAL_PRIORITY. Real hardware rejects
#   0x10000300 (Vita3K accepted it - emulator gap, again). Watchdog
#   priority lowered to 0x10000150, inside the band this project has
#   verified on hardware for months (cldc_ticker / j2me_tone at
#   0x10000100; numeric higher = lower priority, so the watchdog stays
#   the most pre-emptable thread in the process).
set(VITA_VERSION  "01.74")
# 01.75 (2026-09-17): code-review pass on vita_watchdog.c against the
#   vitasdk headers (no behaviour change on the happy path):
#   - thread stack 0x2000 -> 0x4000 (matches j2me_tone, the project's
#     verified floor; a stack overflow gets the thread kernel-killed,
#     i.e. a silently dead watchdog)
#   - HANG report samples BOTH clocks now: gettimeofday AND
#     sceKernelGetSystemTimeWide - if H2 is real, WHICH one froze names
#     the broken layer (newlib/sceRtc chain vs kernel time)
#   - cpuAffinityMask uses SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT
#   - dropped unused <psp2/kernel/processmgr.h> include
set(VITA_VERSION  "01.75")
# 01.76 (2026-09-17): code-review pass on the audio path + dead-file purge:
#   - alog() spinlock is now ATOMIC: the old "while (lock)...; lock = 1;"
#     was check-then-set, racy on the Vita's 3 user cores - and alog IS
#     called concurrently (VM thread + tone thread). A garbled line in
#     audio_debug.log would poison the hang-hunt evidence. Now
#     __sync_lock_test_and_set/__sync_lock_release (ARMv7 ldrex/strex).
#   - j2me_tone thread: bare-0 affinity ->
#     SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT (matches watchdog).
#   - DELETED src/vita_audio.c: dead file, never in the build; its
#     sceAudioOutOpenPort call misused param 2 as "channel count" (it is
#     samples-per-call) - a landmine if anyone ever compiled it in.
set(VITA_VERSION  "01.76")
# 01.77 (2026-09-17): Graphics/input review round (no IO round - that was
#   01.76; memory audit came back clean, no code change needed there).
#   - vita_display.c flip_to_display: after the NEXTFRAME submit it now
#     calls sceDisplayWaitVblankStart() before handing the buffer back
#     as the new back buffer. NEXTFRAME only takes effect at the next
#     vblank; until then scanout still reads the old front, so a fast
#     repaint loop could blit into the just-swapped buffer before the
#     flip landed and tear the frame. menu_flip() always paired the two;
#     display side now matches. Also caps refresh at 60 Hz (fine for a
#     software blit).
#   - flip_to_display now checks sceDisplaySetFrameBuf's return code and
#     logs it once per process to stderr (menu_flip already did; this is
#     how the 01.69 IMMEDIATE 0x80290006 on fw 3.65 was found).
#   - lfjport_ui_finalize: comment documenting the one-frame scanout
#     window on release (left as-is; runs only at VM shutdown).
#   - vita_input.c ring_push: comment nailing down the SPSC invariant
#     (exactly one producer AND one consumer, both the VM thread today).
set(VITA_VERSION  "01.77")
# 01.78 (2026-09-17): TV-style tab menu (vita_menu.c rewrite of the
#   main loop; scan/install/dialog internals untouched).
#   Left tab column (已安装/未安装/文件/设置/退出) + right content area.
#   - 已安装: classic list OR 4x3 icon grid (SQUARE or 设置 tab
#     toggles; persisted in ux0:/data/J2ME00001/menu.cfg, one byte
#     'l'/'g'). Grid supports dpad left/right/up/down-by-row and taps.
#   - 未安装: scans inbox/ at menu start and after every install
#     (this IS the "auto scan + cache" - the jars stay on disk, the
#     scan is a cheap dopen walk). X installs all, tap installs one.
#   - 文件: sandboxed browser rooted at ux0:/data/J2ME00001 (dirs
#     first with a virtual "..", .jar rows highlighted; X/tap on a
#     jar installs it via the same rename+cfg path as inbox).
#   - 设置: game view toggle + ux0 free/total (sceAppMgrGetDevInfo) +
#     games/inbox counts + JIT hint + data dir.
#   - 退出: X or tap quits to launch.cfg/bundled tests (TRIANGLE on
#     the games tab still works).
#   - Tabs switch with L/R shoulder or by tapping the tab column;
#     touch normalized via sceTouchGetPanelInfo exactly like
#     vita_input.c (real hw 1920x1088 -> 960x544).
#   - Messages auto-expire after 4 s instead of sticking forever.
#   Untouched on purpose: game scan/cache/dialog, install_inbox,
#   icon cache, CDRAM fb lifecycle, heartbeat/crumb diagnostics.
set(VITA_VERSION  "01.79")
# 01.79 (2026-09-18): real-hw STARTUP crash fix - omit the frame pointer.
#   See the long comment at the CMAKE_C_FLAGS block below: the 9-17
#   coredump pinned the fault at vita_menu_run+0x134 (`str [r7,#812]`)
#   with r7=0x81000000 = the henkaku user-syscall number base. taiHEN
#   plugin stubs (oclockvita/Framecounter were in the process) load the
#   syscall id into r7 for `svc 0` and never restore it; at -O0 every
#   function in the port kept r7 as its frame pointer, so the first
#   hooked call inside the menu startup chain corrupted the FP and the
#   next local write hit read-only .text. -fomit-frame-pointer removes
#   the dependency entirely.
# 01.78h (2026-09-17): quit STILL hung on v01.78g. New forensics on the
#   unstripped binary (nm + objdump) nailed the crash site: PC was
#   vita_menu_run's EPILOGUE `ldmia.w sp!, {r4,r5,r7,r8,r9,sl,fp,pc}`
#   (0x8100682a in b255), SP=0x80000328 = entrySP-32 = the saved-register
#   block itself, and EVERY popped register read 0 while crumb.log showed
#   30000+ healthy heartbeats and the exit breadcrumbs written fine. So
#   the block was zeroed sometime DURING the session and only gets read
#   by that final pop - patching the exit path can never help. Also
#   disproven: stack overflow (main thread stack is 0x18000 = 96 KB in
#   the module params; usage ~3 KB) and the v01.78g "PNG rowbytes +
#   batch free" theory. Fix: the menu now leaves via
#   longjmp(vita_menu_escape, 1); main arms it with setjmp before the
#   call (jmp_buf in .bss, out of the walker's reach) and reads the
#   answer from vita_menu_result - no epilogue pop executes at all.
#   Forensics kept: a volatile sentinel array in the frame checked by
#   the 1 s heartbeat prints "menu sen BAD" the moment any similar
#   zero-writer strikes again, bracketing it to one heartbeat window.
# 01.78f (2026-09-17): fixed the 设置 tab stealing the games cursor
#   (user report: "on 设置 the UP/DOWN keys do nothing, they actually
#   move the installed-games list").
#   The dpad cursor dispatcher had no tab==3 branch, so psel fell back
#   to &sel/&top (the installed-games cursor) and nent stayed
#   game_count. Now tab 3 gets its own cursor (setsel/settop, nent =
#   SET_N) and tab 4 (退出, no list) is marked has_cursor=0 so the
#   dpad moves nothing there. The scroll clamp only runs when the
#   current tab actually has a list.
# 01.78e (2026-09-17): fixed the empty 文件 tab (user report: "the Vita
#   root and partitions show nothing"). Root cause: fe_scan called
#   sceIoDopen("/") and returned EARLY when it failed, so the partition
#   probe that was supposed to be the fallback never ran. Now the root
#   list is built by probing fe_devices[] (ux0:/ur0:/uma0:/imc0:/
#   grw0:/xmc0:/xmc1:) with dir_exists first, then merging whatever
#   sceIoDopen("/") does enumerate (deduped via fe_has) - the tab can
#   no longer end up empty. Root rows also show "<free> / <total> MB"
#   per partition via sceAppMgrGetDevInfo.
# 01.78d (2026-09-17): user request round 3.
#   - 文件 tab now browses from the Vita pseudo-root ("/" lists
#     ux0:, ur0:, uma0:, ...) instead of the data dir sandbox; ".."
#     stops at "/". fe_scan appends the right separator so entries
#     read "/ux0:" style paths. Installing a jar from ANOTHER
#     partition falls back from sceIoRename (EXDEV) to a copy - the
#     source is kept (a jar on uma0: is the user's archive, not an
#     inbox drop). Tab-2 tap row math fixed to the y=80 content
#     origin (was 64 since the cwd line was added).
#   - 设置 tab is a real option list now: rows with a cursor
#     (UP/DOWN + setsel), X or tap cycles the value. Options:
#     游戏视图 (列表/网格) and JIT 编译 (关闭/仅首轮/每轮). Both
#     persist - menu.cfg moved to a "view=/jit=" line format (the
#     old single-char file still reads), and the JIT choice is
#     written into launch.cfg line 4 so the main loop's
#     read_jit_policy picks it up next round. Info block below the
#     options keeps ux0 capacity, counts, the v01.45 JIT warning and
#     the data dir.
# 01.59 (2026-09-14): JSR 75 groundwork - the PCSL directory layer.
#   pcsl_directory.h declares 8 entry points this port never implemented
#   (grep pcsl_directory_ in vita_pcsl.c was empty), so anything calling
#   is_directory/mkdir/rmdir/getfreesize/gettotalsize/get_attribute/
#   set_attribute/get_time would have failed at link time. They are real
#   now: sceIoGetstat/sceIoMkdir/sceIoRmdir with the same app0: fallback
#   pcsl_file_exist uses, sceAppMgrGetDevInfo for the volume sizes (the
#   "ux0:"-style prefix is extracted from the path so a per-volume query
#   works), and a libc-free days_from_civil conversion for st_mtime -
#   SceIoStat keeps a SceDateTime struct, not epoch seconds, so the first
#   attempt did not even compile. READ/WRITE attributes answer 1 (Vita
#   has no per-file permission bits), EXECUTE/HIDDEN answer 0, and
#   set_attribute accepts READ/WRITE as a no-op. Nothing calls them yet -
#   javax.microedition.io.file is the next step - so this build behaves
#   exactly like 01.58.
# 01.58 (2026-09-14): JIT policy knob. The compiler was always compiled into
#   the VPK (Globals.hpp:231 product(bool, UseCompiler, true); 29 Compiler
#   symbols in the binary) but runMidlet.c unconditionally passed "-int",
#   so the JIT never ran after v01.46. Now launch.cfg line 4 accepts
#   "jit=0|1|2" (default 0 = interpreter every round = v01.46 behaviour):
#   1 = JIT on round 1 only, 2 = JIT on every round. vita_main.c owns the
#   value in the global vita_jit_policy; runMidlet.c reads it through the
#   weak symbol of the same name and injects "-int" only when the policy is
#   0, so non-Vita builds and the standalone libmidp.so link keep the old
#   behaviour. "-int" is sticky (UseCompiler is a process global,
#   Arguments.cpp:129) which is why jit=1 yields interpreter from round 2 on.
#   jit=2 is the minimal reproducer for the still-unfixed round-2 crash.
# 01.57 (2026-09-14): Real-device coredump (psp2core-...-eboot.bin.psp2dmp)
#   forensics: faulting PC 0x810bb25a = strge r3,[r5] inside
#   pte_osSemaphoreCreate with r5 (the pHandle out-param) == NULL. That is
#   a NULL store AFTER a successful sceKernelCreateSema - i.e. a caller
#   passed a NULL out-param, the classic newlib lazy FILE-lock init
#   signature, hit from a VM worker thread's first stdio write. The
#   JDWP/exception path was ruled out (_debugger_active is only set by
#   connect_java_debugger, and USE_ON_DEVICE_DEBUG=0). Mitigations: every
#   hot print path is now stdio-free (vita_crumb.c = sceIo append channel;
#   JVMSPI_PrintRaw and pcsl_print_chars both route through crumb_append),
#   and the linker --wrap=pte_osSemaphoreCreate logs EVERY create with its
#   caller address and refuses the NULL-pHandle call, so the next device
#   run names the culprit in ux0:/data/J2ME00001/crumb.log.
# 01.46 (2026-09-10, phoneme-midp 6525f4a): Second-launch UC crash (JIT)
#   isolated. PC=0x85f05fd8 was inside the heap compiler_area (JIT code
#   cache); the faulting ldr r4,[r5,#0x404] is the standard JIT prologue
#   GP read of _compiler_stack_limit, but the fault address (0x4E5060004)
#   did not match r5+0x404 -> execution had already jumped into bad code.
#   runMidlet now passes the standard "-int" VM option on every round
#   (UseCompiler=false, process-global) so the whole JIT path is off
#   while the in-process-restart JIT issue is investigated. Perf trade,
#   root cause still open.

find_program(Java_JAR_EXECUTABLE
    NAMES jar
    PATHS $ENV{JAVA_HOME}/bin /usr/lib/jvm/*/bin /opt/jdk*/bin
    DOC   "Path to the jar executable"
)
if(NOT Java_JAR_EXECUTABLE)
    message(FATAL_ERROR "jar executable not found. Set JAVA_HOME or add JDK bin to PATH.")
endif()

# Project paths
set(PORT_DIR      ${CMAKE_CURRENT_SOURCE_DIR})
set(J2ME_DIR      ${PORT_DIR}/..)
set(CLDC_DIR      ${J2ME_DIR}/phoneme-cldc)
set(MIDP_DIR      ${J2ME_DIR}/phoneme-midp)
set(MIDP_BUILD    ${MIDP_DIR}/build/vita_arm)
set(CLDC_DIST     ${CLDC_DIR}/build/vita_arm/dist)

set(MIDP_OBJ_DIR  ${MIDP_BUILD}/obj/arm)
set(PCSL_LIB_DIR  ${MIDP_BUILD}/pcsl/vita_arm/lib)

link_directories(
    ${MIDP_OBJ_DIR}
    ${PCSL_LIB_DIR}
    ${CLDC_DIST}/lib
)

# v01.79 REAL-HW STARTUP CRASH FIX (taiHEN plugin stubs clobber r7):
#   The 9-17 coredump faulted at `str.w r3, [r7, #812]` in
#   vita_menu_run with r7=0x81000000 = the henkaku user-syscall base;
#   taiHEN plugin stubs (oclockvita/Framecounter were in the process)
#   load the syscall id into r7 for `svc 0` and never restore it. At
#   -O0 (empty CMAKE_BUILD_TYPE) every function kept r7 as its frame
#   pointer, so one hooked call in the menu startup chain smashed the
#   FP and the next local write hit read-only .text. See the v01.79
#   version note above for the full forensics.
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -Wall -Wno-unused-parameter -fomit-frame-pointer")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -fpermissive -Wall -Wno-unused-parameter -fomit-frame-pointer")

#==============================================================================
# Include Directories
#==============================================================================
set(MIDP_INCLUDE_DIRS
    ${MIDP_BUILD}
    ${MIDP_DIR}/src/ams/example/jams/include
    ${MIDP_DIR}/src/ams/ams_base/include
    ${MIDP_DIR}/src/ams/example/ams_common/include
    ${MIDP_DIR}/src/ams/nams/include
    ${MIDP_DIR}/src/ams/appmanager_ui_resources/include
    ${MIDP_DIR}/src/ams/suitestore/common_api/include
    ${MIDP_DIR}/src/ams/suitestore/recordstore_api/include
    ${MIDP_DIR}/src/ams/suitestore/internal_api/include
    ${MIDP_DIR}/src/ams/suitestore/installer_api/include
    ${MIDP_DIR}/src/ams/suitestore/secure_api/include
    ${MIDP_DIR}/src/ams/suitestore/task_manager_api/include
    ${MIDP_DIR}/src/ams/suitestore_javacall/include
    ${MIDP_DIR}/src/core/log_base/include
    ${MIDP_DIR}/src/core/string/include
    ${MIDP_DIR}/src/core/global_status/include
    ${MIDP_DIR}/src/core/storage/include
    ${MIDP_DIR}/src/core/vm_services/include
    ${MIDP_DIR}/src/configuration/properties_port/include
    ${MIDP_DIR}/src/events/eventsystem/include
    ${MIDP_DIR}/src/events/input_port/include
    ${MIDP_DIR}/src/events/eventqueue_port/include
    ${MIDP_DIR}/src/events/eventqueue/include
    ${MIDP_DIR}/src/events/slavemode_port/include
    ${MIDP_DIR}/src/events/mastermode_port/include
    ${MIDP_DIR}/src/highlevelui/keymap/include
    ${MIDP_DIR}/src/highlevelui/lcdlf/include
    ${MIDP_DIR}/src/highlevelui/lfjport/include
    ${MIDP_DIR}/src/lowlevelui/graphics/gx_putpixel/include
    ${MIDP_DIR}/src/lowlevelui/graphics/gx_putpixel/native
    ${MIDP_DIR}/src/lowlevelui/graphics/include
    ${MIDP_DIR}/src/lowlevelui/image_api/include
    ${MIDP_DIR}/src/lowlevelui/graphics_api/include
    ${MIDP_DIR}/src/core/kni_util/include
    ${MIDP_DIR}/src/security/crypto/include
    ${MIDP_DIR}/src/security/file_digest/include
    ${MIDP_DIR}/src/security/secure_random_port/include
    ${MIDP_DIR}/src/lowlevelui/putpixel_port/include
    ${MIDP_BUILD}/generated
    ${MIDP_BUILD}/pcsl/vita_arm/inc
    ${CLDC_DIST}/include
    ${CLDC_DIR}/src/vm/cpu/arm
    ${CLDC_DIR}/src/vm/share/natives
)

include_directories(
    ${PORT_DIR}/src
    ${MIDP_INCLUDE_DIRS}
)

#==============================================================================
# Prepare libobj_no_main.a: drop members superseded by this port layer
#==============================================================================
set(LIBOBJ_NO_MAIN ${CMAKE_CURRENT_BINARY_DIR}/libobj_no_main.a)

add_custom_command(
    OUTPUT  ${LIBOBJ_NO_MAIN}
    COMMAND ${CMAKE_COMMAND} -E copy ${MIDP_OBJ_DIR}/libobj.a ${LIBOBJ_NO_MAIN}
    COMMAND arm-vita-eabi-ar d ${LIBOBJ_NO_MAIN} runMidlet_md.o
    COMMAND arm-vita-eabi-ar d ${LIBOBJ_NO_MAIN} lfjport_fb_export.o
    COMMAND arm-vita-eabi-ar d ${LIBOBJ_NO_MAIN} mastermode_export.o
    COMMAND arm-vita-eabi-ar d ${LIBOBJ_NO_MAIN} gxjport_text.o
    DEPENDS ${MIDP_OBJ_DIR}/libobj.a
    COMMENT "Preparing libobj_no_main.a (dropping runMidlet_md, lfjport_fb_export, mastermode_export)"
    VERBATIM
)

add_custom_target(libobj_target DEPENDS ${LIBOBJ_NO_MAIN})

#==============================================================================
# Executable: our port layer first, then the MIDP archive
#==============================================================================
add_executable(${PROJECT_NAME}
    src/vita_main.c
    src/vita_menu.c
    src/vita_font.c
    src/vita_icon.c
    src/vita_display.c
    src/vita_input.c
    src/vita_pcsl.c
    src/vita_net.c
    src/vm_rom_stubs.c
    src/vita_storage.c
    src/vita_audio_javacall.c
    src/vita_media_notify.c
    src/vita_sms_javacall.c
    src/vita_checkevents.c
    src/vita_watchdog.c
    src/vita_crumb.c
)

# Size of the bundled jar, baked into the binary for the runtime
# copy-integrity check (must be defined BEFORE the gen_version target)
set(MIDLET_JAR ${PORT_DIR}/build/midlet/jar/Hello.jar)
file(SIZE ${MIDLET_JAR} HELLO_JAR_SIZE_N)

# Build-time version header (git build number + hash), always regenerated
set(VERSION_HEADER ${CMAKE_CURRENT_BINARY_DIR}/vita_version.h)
add_custom_target(gen_version ALL
    COMMAND ${CMAKE_COMMAND}
        -DVERSION_FILE=${VERSION_HEADER}
        -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}/..
        -DAPP_VER=${VITA_VERSION}
        -DAPP_NAME=${VITA_APP_NAME}
        -DHELLO_JAR_SIZE=${HELLO_JAR_SIZE_N}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenVersion.cmake
    BYPRODUCTS ${VERSION_HEADER}
    COMMENT "Generating vita_version.h")
add_dependencies(${PROJECT_NAME} gen_version)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

add_dependencies(${PROJECT_NAME} libobj_target)

# Link order matters: the port-layer objects MUST be seen by the linker
# BEFORE ${PCSL_LIB_DIR}/libpcsl_multimedia.a. That archive is a
# gen_stubs.py-generated all-stub file (javacall_media_* return
# NOT_IMPLEMENTED); with --allow-multiple-definition the FIRST
# definition wins, so an archive-first order silently overrides this
# port's real audio implementation (v01.12-v01.14 "Failed to play
# tone" root cause - the KNI layer saw the stub's NOT_IMPLEMENTED and
# the real code in vita_audio_javacall.c never executed, which is also
# why audio_debug.log never appeared).
# target_link_options() always lands BEFORE the object files, so the
# whole-archive block + library group must live in target_link_libraries,
# which CMake orders after its own objects.
target_link_libraries(${PROJECT_NAME}
    -Wl,--whole-archive
    ${LIBOBJ_NO_MAIN}
    ${MIDP_OBJ_DIR}/runMidlet.o
    -Wl,--no-whole-archive
    -Wl,--allow-multiple-definition
    # v01.67 REAL-HW BOOT CRASH FIX: on a real console the loader slides
    # the code segment (observed +0x72000 on fw3.65, plugins present) and
    # the RW segment independently (+0x120000), and it applies the ELF's
    # relocation tables - EXCEPT that ld's default long-range ARM->Thumb
    # interworking veneers (____X_from_arm, "ldr pc,[pc,#-4]; .word abs")
    # carry NO relocation entry for their literal, verified on the v01.66
    # ELF: 38 veneers, 0 relocs. The 4th .init_array ctor (the C++
    # _GLOBAL__sub_I for Frame::_in_gc_state) ends in "b ____aeabi_atexit_
    # from_arm", so at boot the jump lands on the stale link-time absolute
    # address -> wild code -> data abort BEFORE main() (hence no boot_log/
    # crumb/net_log/vm_output files ever appear; Vita3K loads at a fixed
    # 0x81000000 so the emulator never reproduces it). --pic-veneer makes
    # ld emit position-independent veneers ("adr/ldr pc-relative" or
    # movw/movt pairs that DO get relocation records).
    -Wl,--pic-veneer
    # v01.57 crash-hunt: intercept the pthread-emulation semaphore init
    # (the v01.56 coredump faulting PC). __wrap_pte_osSemaphoreCreate in
    # vita_crumb.c logs every create and refuses the NULL-pHandle call;
    # references inside libpthread.a resolve to __real_*. NOTE: --wrap
    # only affects references from objects linked into this executable -
    # exactly the callers we care about (newlib FILE-lock init, VM/ANi
    # pthread usage). Wrapper must come before -lpthread.
    -Wl,--wrap=pte_osSemaphoreCreate
    -Wl,--start-group
    ${PCSL_LIB_DIR}/libpcsl_multimedia.a
    -lcldc_vm
    -lcldc_vm_ani
    -lcldc_vm_anix
    -lcldc_vmtest
    -lpthread
    -lstdc++
    -Wl,--end-group
)

target_link_libraries(${PROJECT_NAME}
    pcsl_string
    pcsl_file
    pcsl_memory
    pcsl_network
    pcsl_print
    pcsl_escfilenames
    png
    z
    SceDisplay_stub
    SceCtrl_stub
    SceTouch_stub
    SceNet_stub
    SceNetCtl_stub
    SceLibKernel_stub
    SceSysmodule_stub
    SceAppMgr_stub
    SceGxm_stub
    SceAudio_stub
    # v01.67: sceKernelAllocMemBlock/GetMemBlockBase for the CDRAM
    # framebuffers (vita_fbmem.h)
    SceSysmem_stub
    m
)

#==============================================================================
# VPK packaging (config copies carry the 240x320 virtual screen)
#==============================================================================
# MEMSIZE deliberately NOT set: fself -m <kib> has no legal value for a
# normal app except 0 (see vita-make-fself usage); passing 128 wrote an
# invalid memory budget into the SELF control info -> real-device install
# fails with 0x8010113D (Vita3K never checks it).
vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME})

# NOTE (v01.52): the jar output MUST live OUTSIDE the classes/ directory.
# It used to be ${MIDP_BUILD}/classes/midp_system.jar while the package
# command is "jar cf <out> -C classes ." - i.e. the output sat INSIDE the
# tree being packaged. The old directory-mtime DEPENDS almost never
# re-ran the rule, which hid the bug; the v01.51 glob-every-.class fix
# made it re-run on every build, and each run swallowed the previous
# jar into itself (exponential growth: 1.2 MB -> 138 GB observed, and
# classes.zip then packaged that monster too - 30 GB more).
set(MIDP_SYSTEM_JAR ${MIDP_BUILD}/midp_system.jar)

# NOTE (v01.51): depending on the classes *directory* is not enough. Its
# mtime only changes when an entry is added or removed, so recompiling an
# existing .class file (javac overwrites in place) left a stale
# midp_system.jar behind - which is how a v01.51 change managed to be
# absent from the VPK while every step reported success. Depend on the
# class files themselves instead.
file(GLOB_RECURSE MIDP_SYSTEM_JAR_CLASSES ${MIDP_BUILD}/classes/*.class)
file(GLOB_RECURSE MIDP_SYSTEM_JAR_RESOURCES
    ${MIDP_DIR}/src/highlevelui/lcdlf/lfjava/resource/*)

add_custom_command(
    OUTPUT  ${MIDP_SYSTEM_JAR}
    COMMAND ${Java_JAR_EXECUTABLE} cf ${MIDP_SYSTEM_JAR} -C ${MIDP_BUILD}/classes .
            -C ${MIDP_DIR}/src/highlevelui/lcdlf/lfjava/resource .
    DEPENDS ${MIDP_SYSTEM_JAR_CLASSES} ${MIDP_SYSTEM_JAR_RESOURCES}
            ${MIDP_BUILD}/classes
    COMMENT "Repackaging midp_system.jar with cldc + midp classes + chameleon skin resources"
    VERBATIM
)
add_custom_target(midp_system_jar ALL DEPENDS ${MIDP_SYSTEM_JAR})

if(NOT EXISTS ${MIDLET_JAR})
    message(STATUS "Hello.jar missing at ${MIDLET_JAR}, will try build_jar.sh")
    execute_process(
        COMMAND bash ${PORT_DIR}/build_jar.sh
        WORKING_DIRECTORY ${PORT_DIR}
        RESULT_VARIABLE JAR_BUILD_RES
    )
    if(NOT EXISTS ${MIDLET_JAR})
        message(FATAL_ERROR "Hello.jar could not be built (see build_jar.sh output).")
    endif()
endif()

# Flatten chameleon skin PNGs into data/J2ME00001/lib/ for the
# filesystem fallback path: ResourceHandler.getSystemResource reads
# getConfigRoot + "<imageIdentifier>.png" (e.g. lib/stringitem.image_lnk.png)
file(GLOB VITA_SKIN_PNGS
    ${MIDP_DIR}/src/highlevelui/lcdlf/lfjava/resource/skin/*.png)

set(VITA_SKIN_FILE_ARGS "")
foreach(_png ${VITA_SKIN_PNGS})
    get_filename_component(_name ${_png} NAME)
    list(APPEND VITA_SKIN_FILE_ARGS FILE ${_png} data/J2ME00001/lib/${_name})
endforeach()

vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
    VERSION  ${VITA_VERSION}
    NAME     ${VITA_APP_NAME}
    FILE     ${PORT_DIR}/assets/sce_sys/icon0.png                         sce_sys/icon0.png
    FILE     ${PORT_DIR}/assets/sce_sys/livearea/contents/bg.png          sce_sys/livearea/contents/bg.png
    FILE     ${PORT_DIR}/assets/sce_sys/livearea/contents/startup.png     sce_sys/livearea/contents/startup.png
    FILE     ${PORT_DIR}/assets/sce_sys/livearea/contents/template.xml    sce_sys/livearea/contents/template.xml
    FILE     ${PORT_DIR}/config/internal.config          data/J2ME00001/lib/internal.config
    FILE     ${PORT_DIR}/config/internal.landscape.config data/J2ME00001/lib/internal.landscape.config
    FILE     ${PORT_DIR}/config/system.config            data/J2ME00001/lib/system.config
    FILE     ${MIDP_SYSTEM_JAR}                  data/J2ME00001/midp_system.jar
    FILE     ${PORT_DIR}/config/fontbitmap.bin    data/J2ME00001/fontbitmap.bin
    FILE     ${MIDLET_JAR}                       data/J2ME00001/Hello.jar
    ${VITA_SKIN_FILE_ARGS}
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.vpk
        DESTINATION .
        RENAME ${VITA_TITLEID}.vpk)

# DO NOT patch e_entry in the velf. In the SCE layout e_entry is NOT the
# program entry - it is the sce_module_info locator: (segment<<30)|offset.
# vita-elf-create sets it correctly (e.g. 0x364740 -> .sceModuleInfo.rodata
# at 0x81364740). The kernel derives the real entry from module_info's
# module_start field; patching e_entry to _start makes Vita3K parse
# .text bytes as sce_module_info_raw in load_imports and crash with a
# read violation right after "Linking SELF" (b137->b146 startup crash).

