# Development Log

## Status

Offline development only. No hardware acceptance has been performed for this repository.

### 2026-10-03: Independent Demo Skeleton

- Changes: new repository, plan, frozen board adapters, three program build
  targets, fixed-width request/status protocol, asynchronous one-page LVGL UI,
  Unix socket command fixture and focused protocol/socket tests.
- Decision: retain three owners (vision/media/UI). Old networking, identity DB,
  playback and complex recovery are not imported. Real media commands return
  ENOSYS until integration; only explicit --mock can fabricate switch states.
- Validation: Docker `bash tools/build.sh big`, `little`, `test` passed. Three
  RISC-V executable targets and two LVGL libraries linked; protocol and socket
  tests checked lengths/version/values/IDs, four mock switches, idempotence and
  stopping RTSP while record remains enabled (mock control only). Initial Linux
  build exposed the SDK wrapper's /opt/toolchain dependency and an unnecessary
  freetype link; fixed Docker mount and omitted freetype (frozen config disables it).
  Final Linux build log: out/build-little.log. No board connection.
- Unverified/risks: camera, channel concurrency, KPU, OSD blending, audio PTS,
  RTSP/MP4 and UI hardware operation. Current targets are not a functional package.
- Next: implement checked VB/three-channel camera/display ownership and SDK IPC,
  then the single MobileRetinaFace model and OSD.

### 2026-10-03: Three-Channel Camera Candidate

- Changes: one Camera owner with checked VB, GC2093, VICAP and connector/layer
  calls. CHN0=800x480 YUV preview, CHN1=1280x720 planar RGB AI,
  CHN2=1280x720 YUV encode; explicit independent pool sizes and shared stream
  block constants (692224 bytes, 30 blocks). Common pool budget 47666 KiB excludes
  AI tensors, OSD and SDK metadata. Implemented shutdown error retention.
- Decision: bind CHN0 before VICAP init/start as in SDK sample_vicap. Keep binding
  alive during preview OFF; only gate layer1. Reserve user dump output only for
  RGB CHN1 using kd_mpi_vicap_set_dump_reserved. No claim this fixes the old
  hardware black screen; avoids copying its dynamic bind/unbind design.
- Validation: Docker big cross-build passed with real camera/MAPI SDK symbols.
  MAPI's built-in registration also requires libvvi (SDK dependency, no VVI demo
  pipeline added). Docker test passed with a narrow API fixture checking
  RGB/YUV formats/buffer sizes, pre-start binding, idempotent preview toggle,
  start/config failure cleanup and retaining VB when stream stop fails.
  Logs: out/build-camera.log, out/test-camera.log. Docker verify passed the
  ELF64/RISC-V/little-endian header predicates and all frozen byte checks;
  log out/verify-camera.log retains readelf's SDK section-table warning
  (section 326 sh_entsize=8, expected 24). This is not a full ELF structural pass
  or loader acceptance; do not hide it or alter the passing firmware.
- Unverified/risks: actual CHN2 delivery, three-channel concurrent hardware FPS,
  VO layering and shutdown. SDK status/AI model/OSD and real media not yet
  integrated. SDK baseline includes pre-existing CRLF/trailing spaces;
  deliberately preserved rather than reformatted during import.
- Next: SDK IPC between media and vision; initialize one MobileRetinaFace model,
  AI2D/KPU timings and rotated OSD overlay, then shared video/audio sources.

### 2026-10-03: Preview Report Triage And AI/Control Integration

- Changes: corrected VICAP CHN0 to YUV semiplanar 420 while retaining the SDK
  VO layer's distinct YVU planar convention. Old code used the VO enum for
  capture and logged an ISP default-format fallback. The new pipeline retains
  its pre-start binding and layer-only preview switches. Recorded the old board
  report in acceptance without claiming a hardware root cause or repair.
- Changes: added one model owner, AI2D letterbox, KPU runtime, nine-head face
  decoder, bounded NMS and clockwise portrait mapping; dedicated OSD2/CHN5
  boxes, landmarks and count with two private buffers. Added SDK IPC snapshots
  and main-thread command ACK; Linux forwards preview/AI/status, leaving codecs
  explicitly ENOSYS. UI keeps failed-operation feedback across polling.
- Safety: SDK buffers remain allocated after an unconfirmed OSD disable, including
  insert-success/enable-failure. Shutdown skips camera/VB/MAPI teardown when OSD
  ownership is retained. Successful service polls preserve asynchronous AI errors.
- Decision: preserve all frozen display/touch/kernel bytes and their existing
  vblank behavior. CHN0 NOTREADY and a CHN1 frame do not establish visible preview;
  vblank warning alone does not establish a UI crash. No SDK mutation, board
  connection, deployment or old-project edits.
- Build issue: initial Linux IPC link failed because slave/lib uses GCC12 vector1/Zve
  attributes, incompatible with Linux's GCC10/V2.6 linker. Inspected archives in
  Docker and matched the old Linux build's explicit host/lib selection. Kept
  toolchain/SDK unchanged; no metadata stripping or incompatible-link suppression.
- Validation: Docker `bash tools/build.sh big`, `little`, `test`, `verify` passed.
  Pure decoder test compares all 4200 priors with SDK anchors and checks logits,
  NMS, padding, NaN/Inf and portrait mapping. Narrow camera fixture checks capture
  versus VO formats/order; IPC fixture checks malformed body/module, busy slot,
  delayed ACK, shutdown and retained errors; OSD fixture checks layer, transparent
  pixels and resource retention. These are not hardware simulations or acceptance.
  Logs: out/build-integration.log, out/build-ipc-fixed.log,
  out/test-integration.log and out/verify-integration.log.
- Remaining: SDK header 'KMDL' warning and the pre-existing ELF section sh_entsize
  issue (now section 2305) remain. Verify passes header predicates/frozen hashes,
  not full ELF structural or loader validation. Actual model loading, three-channel
  delivery, KPU, OSD scanout/blending, vblank events and preview remain unverified.
- Next: one shared H.264/G.711A source and bounded independent RTSP/MP4 queues;
  real media, final page rendering and deployment bundle remain unfinished.

### 2026-10-03: Shared Encoder/Audio And Bounded Consumers

- Changes: one process-lifetime MediaSource owns VENC0 (VI CHN2 YUV) and
  mono-right inner-codec AI/AENC0. First subscriber starts both codecs; last
  subscriber stops/releases them. Two independent video/audio queue pairs share
  immutable copied frames. Control buttons remain explicitly ENOSYS until real
  RTSP/MP4 workers exist; no fake streaming or recording states were added.
- Media: H264/1280x720/30 FPS/4000 kbps/GOP30; G711A/8000/mono/320 samples.
  Validate pack count/length, complete Annex-B access units, headers and monotonic
  per-track PTS. Cache SPS/PPS and make joining IDRs self-contained. Source counters
  measure received callbacks, not network/file success. VENC callback bound is
  1 MiB, matching SDK read_venc_data; each consumer video queue 90 frames/8 MiB,
  audio 100 packets/128 KiB. Live overflow resets a whole GOP and schedules IDR
  outside callback locks; record overflow fails only that consumer.
- Timing evidence: SDK sample_audio _test_aenc_timestamp explicitly logs us;
  SDK mp4_format divides input time_stamp by 1000. Keep source timestamps in us,
  one wall-clock mapping for both RTSP tracks, and 125-us G711A sample clipping.
  Actual AV alignment and first-sample PTS interpretation are not proven offline.
- Ownership: only initialize Linux MAPI client and its existing readiness flag,
  never remote media/VB. Callback data is copied before the SDK unmaps it.
  SDK stop/unregister/deinit run outside the delivery lock; retain flags and
  first cleanup error if release fails, reject new owners and avoid destructing
  a potentially live SDK callback target at service exit.
- SDK detail: VENC init creates a local FIFO even after a failed remote init;
  AENC start creates its local reader before checking remote START. Track these
  attempts and perform cleanup. SDK itself has unchecked pthread_create/FIFO/mmap
  returns and some overwritten internal errors; application checks cannot prove
  those internal paths safe. No SDK/kernel sources were modified. Real consumers
  still need first-frame deadlines and explicit worker error feedback.
- Validation: Docker `bash tools/build.sh little`, `test`, `verify` passed.
  Source fixture checks one codec initialization for two owners, CHN2/mono params,
  immutable copies, independent dequeues, header/IDR recovery, malformed and startup
  callback errors, partial-start cleanup, positive SDK errors and stop-failure
  retention. Stop fixture joins a callback thread to catch lock-order deadlocks
  (10-second test-only deadline). Pure frame/time test checks bounds, GOP recovery,
  finish/discard, independent failures, RTP epoch mapping and audio clipping.
  Logs: out/build-source-final.log, out/test-source-final.log, out/verify-source.log.
  An initial subword atomic-exchange link error was removed by using a word-sized
  IDR flag; no new libatomic/runtime dependency. Initial rebuild had transient
  make clock-skew warnings; final build completed without them.
- Remaining: SDK ELF section-table warning persists; verify still checks header
  predicates and frozen source hashes, not loader compatibility. No board, network,
  kernel, model or touch changes. Actual codec/audio callbacks and simultaneous
  AI/encode/preview operation remain unverified. This is not a deployable full Demo.
- Next: live555 RTSP and MP4 worker consumers, wire their asynchronous operations
  and measured stats into service/UI, then render/package for final board acceptance.

### 2026-10-03: Real Asynchronous RTSP Consumer

- Changes: feed-backed nonblocking live555 sources, H264/PCMA subsessions and
  one RTSP worker. The Unix service now dispatches actual RTSP ON/OFF instead of
  ENOSYS; UI shows asynchronous busy and still allows an active RTSP STOP after
  a vision polling failure. Record remains ENOSYS until its actual worker exists.
- Decisions: bind IPv4 8554 before codecs, reject occupied IPv4 even if IPv6
  could succeed; require an accepted complete IDR/header config and audio within
  three seconds. SDP uses cached SPS/PPS; static RTP payload 8 means PCMA/8000/mono
  without requiring an explicit rtpmap line. New track sources resync only live
  queues and schedule IDR outside callbacks. One reused source per track avoids
  clients competing for queue frames. No reconnect, retry or touch/kernel changes.
- Correctness: SDK AUs can contain multiple slices. Override the discrete
  framer's default every-VCL marker with the exact final-NAL boundary; preserve
  one AV epoch and source PTS rather than dequeue time. NAL overflow is an error,
  not truncation. Worker-owned event-loop watch avoids cross-thread live555 calls.
  Stop closes server/client/source objects before detaching codecs, joins at exit,
  and retains SDK cleanup errors. Source callback stalls fail after three seconds.
- Validation: Docker `bash tools/build.sh little`, `test`, `rtsp`, `verify`.
  Real SDK live555 is built natively in read-only-SDK/network-none Docker, with
  a narrow synthetic codec adapter. Loopback DESCRIBE/SETUP/PLAY/interleaved RTP
  checks SDP H264/90000 + PCMA payload 8, exact audio bytes and multislice marker,
  shared PTS, no NAL truncation, idempotent start, stop/port release, occupied-port
  failure before codec attach and first-frame deadline. Test execution is seconds,
  not a soak. Logs: out/build-rtsp.log, out/test-rtsp.log,
  out/test-rtsp-regression.log, out/verify-rtsp.log.
- Unverified/risks: synthetic transport is not H264 decoding, VLC acceptance or
  hardware AV alignment. Camera/preview and DRM vblank on the old board remain
  unproven; no board was accessed or updated. The previously recorded big ELF
  section-table warning persists; header/hash verification is not loader acceptance.
- Next: real MP4 worker from the independent record feed, simultaneous control,
  metrics, actual LVGL render and a verified application bundle remain required.

### 2026-10-03: Independent MP4 Recorder And Real Decode Check

- Changes: actual asynchronous record control, SDK libmov muxer owner and
  worker-only file I/O. Record and RTSP subscribe to the same VENC/AENC owner,
  with separate queues. UI exposes both independent states/busy/stop operations.
  Clips start on IDR, stop at 15s of PTS or a user STOP, drain accepted tails,
  align to the first IDR epoch and clip G711A leading/trailing samples. Only
  successful finalization renames .mp4.part to .mp4; failed parts remain logged.
- Decisions: bypass kd_mp4 wrapper (ignored write/close errors and incorrect
  G711U insertion path), not the SDK container implementation. Use actual
  libmov/libflv with checked file callbacks and no FASTSTART relocation. A
  small build-local end-track extension fixes stock last stts delta=1ms versus
  estimated mdhd duration and its uninitialized-sentinel assertion order.
  SDK stays read-only; touch/display baseline bytes remain unchanged.
- Timing: audio is held until video confirms its full packet window; final tail
  uses 125us sample clipping. Writer API takes milliseconds, retaining a common
  epoch with <=1ms rounding. End track timestamps derive from actual written bytes
  and video interval. Fixed H264 source has no reordered B frames (PTS=DTS).
- Validation: Docker `bash tools/build.sh little`, `media`, `rtsp`, `test`,
  `verify`. Native real SDK muxer/readback checks H264 and PCMA (not PCMU),
  monotonic PTS, exact clipping bytes, first IDR and final durations; /dev/full
  exercises both write and moov-close failure. A narrow codec adapter checks
  one shared startup, idempotence, each stop order and absent-frame timeout.
  Automatic recording consumes 15s of accelerated PTS (450 video frames and
  120000 audio samples), not a long wall-clock test.
- Sample evidence: container has no preinstalled ffmpeg and SDK test.mp4 files
  are empty. Built test-only x264 library/sample generator and minimal native
  FFmpeg from available SDK sources into out; no downloads or board access.
  FFprobe verifies 1280x720 H264 + pcm_alaw/8000/mono; independent FFmpeg -xerror
  decode verifies the 0.5s clipped file and 15s automatic file. Test tools/sample
  files are not shipped. Logs: out/build-record.log, out/test-media.log,
  out/test-record-regression.log, out/test-record-rtsp.log, out/verify-record.log.
- Unverified/risks: hardware encode/audio clocks, actual camera/AI/RTSP/MP4
  concurrency, VLC, LCD blending and shutdown remain board gates. Native sample
  bytes are x264-generated, not a capture from this pipeline. Docker builds
  intermittently report generated-file times about 1.8s in the future; logs
  retain those warnings. Changed business objects were rebuilt and actual
  tests/decode executed; this does not replace the final clean package build.
  Known big ELF section-table warning remains unresolved; header predicates are
  not loader acceptance. No claim the old board preview has been repaired.
- Next: metrics and actual LVGL render, resolve big ELF loader/structure concern,
  then commit-based package/startup/model/library metadata and final scope audit.

### 2026-10-03: Actual LVGL Page, Measured Metrics And Big ELF Layout

- Changes: spacious portrait page module used by both demo_ui and native LVGL
  render. Header/metrics/four buttons are separated from a transparent preview
  center. Socket calls remain off the LVGL thread; busy/error/media STOP handling
  is retained. Real callback FPS/kbps, source generation resets and media process
  CPU/RSS are wired after vision status merge. Wire version becomes 2/84 bytes;
  rebuild all programs together. Missing CPU/RSS shows --; VB is a budget only.
- Findings/fix: real render caught an overflowing preview label; shortened it.
  SDK example linker wildcard *(*.got) included .rela.got, giving .got the wrong
  type/entry size. An application-side derived script uses exact patterns, explicit
  RX/RW page-start segments and proper TLS. The SDK loader maps non-PROGBITS
  allocated sections as data; place the remaining all-zero R_RISCV_NONE section
  beyond the text page to avoid mapping overlap. No SDK mutation or stripped
  warning metadata. A verifier checks these static loader constraints.
- Validation: Docker tools/build.sh all, test, ui and verify passed. Parser/rate
  checks cover process names with spaces/parentheses, malformed data, restart,
  idle and saturation; RSS is actually sampled from the test process's /proc.
  Three PNGs are real SDK LVGL + actual page renders, with bounds/transparency
  and button command tests. running.png was visually inspected. readelf now has
  no invalid sh_entsize warning; the structural checker rejects the old malformed
  RELA layout. Logs: out/build-metrics-ui.log, out/test-metrics.log,
  out/test-ui.log, out/verify-ui-elf.log, out/build-elf.log.
- Unverified/risks: screenshots use synthetic grid/stats, not the camera or LCD.
  Hardware frames, blending, KPU, codec clocks, Linux DRM vblank and runtime ELF
  loading remain board gates. The old lawrec bind/dump logs do not prove a new
  Demo pass or its black-screen root cause. Frozen touch/panel checks pass.
- Next: clean commit-based application package, dependency/model/provenance
  metadata and startup/stop instructions, then concise whole-Demo acceptance.

### 2026-10-03: Application Bundle And Scoped Startup Tools

- Changes: democtl for fixed socket commands/status; clean-worktree package
  entrypoint with fresh cross-build output per commit/UTC. Bundle scripts collect
  exact model bytes and recursive Linux DT_NEEDED libraries from the same SDK,
  including its matching private loader. Per-program strong dynamic symbols and
  version names must resolve; host SDK RPATHs are replaced by $ORIGIN paths.
  Hashes/source paths/compiler/image/commit/verification logs accompany the archive.
- Decisions: application-only archive, no firmware/SD image, installation, SSH or
  network edits. User starts vision manually first. Linux scripts register only
  their own PID/start-time/command identities, stop UI before media and never
  SIGKILL an MP4 writer. Model/third-party provenance and redistribution limits
  are explicit; native x264/FFmpeg are not shipped.
- Validation: Docker little and verify passed including democtl. bundle-check
  validates POSIX/shell syntax, rejects unrelated/reused live PIDs without signals,
  resolves actual Linux binary/library symbol-version closures and deliberately
  rejects a missing liblvgl. Native real MP4/decode regression passed again after
  source metrics changes (0.5s and 15s H264/PCMA; fake codecs only). Logs:
  out/build-ctl.log, out/verify-package-tools.log, out/test-bundle.log,
  out/test-final-media.log. After $ORIGIN relink, out/build-final-linux.log,
  out/verify-final-linux.log and out/test-final-bundle.log passed again. A dirty
  worktree package request was rejected before Docker/build (out/test-package-dirty.log).
  No board accessed.
- Pending: run the fresh package command after committing these tools; record
  the actual archive/hash and clean-build result in the next log entry. All
  hardware gates remain pending; packaging is not an installation or camera fix.

### 2026-10-03: Package Compiler-Provenance Correction

- First clean build from b836570 completed all ELF/frozen/dependency/version-name
  checks, but archive creation stopped at compiler metadata: the script assumed
  a Buildroot target-prefix compiler name that this SDK does not provide. No
  successful archive was reported and no deployment occurred.
- Fix: derive both compiler paths from the fresh CMake-generated compiler
  descriptors, not from an inferred target triple/sysroot directory. Retain
  the partial release's logs under out/releases for traceability.
- Verification: bundle-check validates the helper's syntax and executes both
  actually configured compilers with --version in Docker. Final fresh package
  creation will be recorded only after it succeeds.

### 2026-10-03: Offline Demo Delivery Complete, Board Acceptance Pending

- Actual clean package command: `bash tools/build.sh package`, exit 0. Source
  commit d598a920604739f3793a02d12d8a5fae19cec062; clean Git worktree at build.
  Fresh output: out/releases/d598a9206047-20261003T045524Z, not the earlier
  incomplete b836570 release. The archive is an application bundle (~11 MiB),
  not an SD image and not something to flash.
- Archive: lawrec-demo-d598a9206047.tar.gz. SHA256:
  f4e00d0664b0e2ec7e9b76829461ca7d0c055f43dc8f6a69ed2a229da0f301a1.
  Programs: vision.elf, media_service, demo_ui, democtl. Ten private dynamic
  libraries/loader, verified single model, scoped scripts, docs, provenance,
  real compiler paths/versions and per-file SHA256SUMS are included. Package
  creation verifies all file hashes before tar; no host compiler or board access.
- Clean build/verify/runtime logs are in that release and archive meta/. No
  clock-skew or ELF section-size warnings in this build. The SDK nncase header's
  existing multicharacter model-tag warning remains visible; no suppression or
  SDK source edits. Both compiler-version commands executed in Docker.
- Final short offline checks: out/test-final.log (protocol/socket/camera/AI/
  IPC/OSD/queues/PTS/metrics/source), out/test-final-rtsp.log (real live555
  synthetic RTP transport), out/test-final-media.log (actual SDK muxer/readback
  and independent H264/PCMA decode), out/test-ui.log (actual LVGL rendering),
  out/test-compiler-provenance.log (scripts/dependencies/version names). All
  passed. Synthetic camera/codec fixtures are clearly labeled, not hardware runs.
- Scope audit: all plan modules have real code/control/build wiring: independent
  preview/RGB AI/encode channels; one model AI2D/KPU/NMS/OSD; one H264/PCMA source
  with independent RTSP/15s MP4; asynchronous one-page LVGL + socket/SDK IPC;
  measured rates/times/queues/media CPU/RSS/VB budget; deployable candidate and
  start/stop/VLC/model/library/checksum instructions. Business C++/headers total
  2837 lines, excluding tests, SDK and frozen adapter. No WiFi, settings, playback,
  face identity DB or general recovery framework is imported.
- Remaining: **every board gate is pending**. Only a short joint preview/AI/
  RTSP/record/sound/normal-stop pass is required, not a soak. In particular, the
  old lawrec CHN0 dump/vblank report does not certify a fix of its black screen;
  camera delivery, RGB/VENC concurrency, LCD blending, KPU runtime, audio clock,
  actual ELF loading and VLC still require actual board observations.
- This final documentation commit changes no application bytes. The bundle's
  checklist was captured just before packaging; meta/ contains actual completed
  clean-build/verification evidence. Old repository user changes remain intact.

### 2026-10-03: Chinese UI For The 3.1-Inch Panel

- Changes: Chinese home page and operation/error messages. Compact 96px header,
  372px transparent preview band, two short metric columns and four 216x104px
  controls. Control/title fonts are 32px; primary values/state 24px; secondary
  feedback 20px. Detailed AI stages, rates, queues, media CPU/RSS/VB and full
  error codes move into a Chinese details modal, not a crowded permanent header.
  Details/return are local UI operations; the modal blocks underlying commands.
- Fonts: application-only generated subsets from SDK SimSun + Montserrat using
  cached lv_font_conv 1.5.3/Node in network-none Docker, both mounted read-only.
  Fonts are committed C assets; ordinary builds have no Node/old-project
  dependency. Collect Chinese characters from page.cpp automatically. Frozen
  adapter, original 16px font, portrait touch mapping and panel/PHY are unchanged.
- Validation: tools/build.sh fonts, ui, little and verify passed in Docker.
  Actual LVGL PNGs cover idle/running/busy/error plus details/error-details.
  Check every visible character for a real glyph, large touch target dimensions,
  transparent preview, bounds and existing busy/error STOP behavior. Opening,
  closing and clicking behind the modal emit no media command. Running/details
  screenshots were visually inspected. Logs: out/build-cn-fonts.log,
  out/test-ui-cn.log, out/build-ui-cn.log, out/verify-ui-cn.log.
- Remaining: physical readability/finger touch on the 3.1-inch LCD still needs
  the short board acceptance. Render background/stats are synthetic. No SSH,
  board update, media change, timing change or long regression was performed.
  The previous d598a92 archive has the English page; create a new clean package
  from this UI commit before deploying the Chinese version.

### 2026-10-03: Chinese UI Application Candidate Packaged

- Clean `tools/build.sh package` from 1cfd6b830686 completed, including full
  cross-build, ELF/frozen byte checks, exact model hash and Linux dependency/
  symbol-version closure. Actual Chinese code/font assets are in demo_ui;
  ordinary build/package needs neither Node nor the converter cache.
- New archive: out/releases/1cfd6b830686-20261003T091916Z/
  lawrec-demo-1cfd6b830686.tar.gz. Its actual SHA256 is recorded in that release's
  archive.sha256; bundled per-file SHA256SUMS were checked before archive creation.
  Detailed logs are in the release's build.log, verify.log and runtime.log.
- The previous English archive remains untouched; use this new package for
  Chinese UI acceptance. No board connection or installation. Only UI-focused
  short rendering/glyph/interaction checks were run; media stress was not repeated.
- This documentation-only follow-up does not change the candidate's application
  bytes. Physical readability/touch and all prior hardware gates remain pending.

### 2026-10-04: Chinese Startup And Short Acceptance Guide

- Translated docs/deployment.md into Chinese, including startup/shutdown order,
  package prerequisites, safety notes and one short functional acceptance pass.
  Acceptance steps now use the four actual Chinese UI button names.
- Validation: git diff --check passed; fenced shell commands match HEAD exactly.
  Documentation only: no build, board access or application archive update.
  Hardware acceptance remains pending.

### 2026-10-04: Application Uploaded At Explicit User Request

- The user explicitly requested board upload, overriding the repository's
  default offline-only workflow for this transfer. Used root@192.168.123.74;
  uploaded the existing Chinese UI archive from release 1cfd6b830686 into
  /sharefs and extracted into the previously absent /sharefs/lawrec-demo.
- Local archive.sha256 and remote archive SHA256 checks passed. Board BusyBox
  tar rejected -z, so used local gzip -dc piped over SSH to remote tar -xf -.
  Remote sha256sum -c SHA256SUMS passed for every bundled file.
- No application started/stopped, old application overwritten, system library,
  firmware or network changed. The archive and its bundled documentation are
  unchanged; source-only Chinese documentation updates are not in this archive.
  Hardware functional acceptance remains pending.

### 2026-10-04: Fix Connector Close Aborting Vision Startup

- Board report: connector power/init succeeded, close reported -4096, vision
  exited before capture setup and Linux start.sh failed. SDK mpi_connector.c
  declares kd_mpi_connector_close as k_s32 but only calls close(fd), without a
  return statement. Treating that undefined return as a status aborted startup.
- Application now calls POSIX close(fd) directly, translates its errno, keeps
  an earlier init error and marks successful display initialization before
  close so cleanup still tracks it. No SDK, panel, timing or touch changes.
- Added tools/build.sh camera for only the ownership/cleanup mock. Checks
  bypassing the defective wrapper, real close failure, retaining init failure,
  and startup after cleanup. Docker camera, big and verify passed; git diff
  --check passed. Mock/ELF checks do not prove board startup or frame delivery.
- Uploaded out/big/vision.elf as /sharefs/lawrec-demo/bin/vision-connector-fix.elf
  via scp -O to root@192.168.123.74, continuing the user-requested transfer.
  Remote SHA256 verified: 8f3f4c4724f4a007a744c329d7ec50e42d93d8126f42f9efa0f14da878b9ebe1.
  No process launched or stopped; original vision.elf/archive remain intact.
  Deployment guide documents the temporary path. User must retry big startup,
  then Linux start.sh only after ready result=0; hardware acceptance pending.

### 2026-10-04: Demo Branch Publishing And Pending IPC Startup

- User selected the existing Atlantis1213/lawrec repository's demo branch.
  Configured origin to https://github.com/Atlantis1213/lawrec.git; ls-remote
  found no existing demo branch. Publish local main to remote demo without
  modifying the original main branch or force-pushing. SDK/models/out artifacts
  remain untracked and excluded from this source transfer.
- Before the publishing request, Docker tools/build.sh test and ui passed;
  results are in out/test-startup-fix.log and out/test-ui-startup-fix.log.
  Board logs confirm connector-close fix, VICAP start and model/OSD init, but
  IPC startup still fails. This is not a full hardware acceptance pass.
  User confirmed big-core serial is COM5, not COM12. Debugging paused to publish.

### 2026-10-04: Board IPC, Shutdown And Real MP4 Repairs

- User authorized active debugging through root@192.168.123.74 and COM5.
  All compilation/fixtures still run inside the read-only SDK Docker. No SDK,
  frozen adapter, panel/PHY, firmware, networking or system libraries changed.
- Root causes repaired: SDK TRY_CONNECT incorrectly treats pending peer result
  1 as an error; preserve the fd only after a driver state check. Missing
  /dev/tty now falls back to nonblocking stdin, so q works. Scope the cancellable
  SDK connect wrapper to kd_mapi_msg, preventing shutdown from waiting forever
  for a Linux peer; driver/protocol ABI and cancellation mocks cover these paths.
- An inactive permanent VI0->VO1 bind retained capture buffers. Preview starts
  OFF/unbound; ON enables then binds, OFF unbinds before disabling, leaving
  RGB/encode capture live. Commit the final configured VO layer state; add serial
  v to inspect live VO/VB reports, without a large automatic boot dump.
- Real encoder IDRs change SPS/PPS IDs while dimensions stay fixed. The former
  byte-equality rejection stopped recording with -EPROTO and left .mp4.part.
  Extend only the private libmov build copy with avc3 tagging and keep in-band
  parameter sets. A generated alternating-ID x264 fixture and independent
  FFmpeg decode verify this is decodable, not just accepted by our muxer.
- Commands/results: tools/build.sh all, test, camera, verify, media, rtsp, ui and
  bundle-check passed during this task. Final baseline-restored all/test/verify
  passed; logs out/build-final.log, test-final.log, verify-final.log. The short
  fixture checks are not substitutes for the actual board results below.
- Board with preview OFF: ready=0 and Linux start/control succeed; AI ~30.3fps,
  AI2D ~1.9ms/KPU ~8ms/post ~0.8ms. H264 1280x720 ~30fps/~4Mbps and mono G711A
  8kHz/320-byte packets run concurrently with AI. Actual live555 TCP reception
  passed: 30 H264 VCL NALs, 27 PCMA packets (out/board-rtsp-client.log).
- Real 15s recording completed 455 frames/120000 audio samples while RTSP was
  stopped then restarted. Record auto-finalized while RTSP remained running.
  File /sharefs/lawrec_records/clip-19700101T014954Z-WIQLZe.mp4, ~8MB; downloaded
  out/board-15s-avc3.mp4. LAWREC_CLIP=out/board-15s-avc3.mp4 tools/build.sh
  clip-check independently decoded H264/avc3 15.000s and alaw 15.001s. Evidence:
  out/board-avc3-short.log and out/board-clip-decode.log. Board clock is unset.
- Remaining failure: preview ON accumulates 45 queued VO frames, exhausts shared
  VB blocks, times out RGB dump (0xa0158010), then fails VENC allocation. Also
  reproduced without Linux UI. DSI errors=0 and VO scan timestamp ~58.77Hz do
  not prove queue consumption. Layer ordering and VTTH line430 experiments did
  not help; line430 and startup report removed, original VTTH threshold restored.
  No alpha-field workaround: this SDK video-layer struct has no alpha member.
  Next investigation is the big-core VO consumption/interrupt-release path;
  kernel root cause and visible preview repair are not confirmed.
- Latest Linux stop.sh then serial q completed; shutdown IPC/OSD/camera/MAPI=0
  and msh returned. Business IPC deliberately has no reconnect/replay: restart
  both cores, not only media. One client attempt overlapped a restart and timed
  out; no claim that every reconnection scenario or subjective playback passed.
  Face view was empty: visible boxes/landmarks, physical Chinese touch/readability
  and VLC picture/microphone checks still pending. No long/stress acceptance.
