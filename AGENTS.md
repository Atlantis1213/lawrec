# Development rules

- Current user scope (2026-10-02): this is a functional Demo. Prioritize short
  real-board preview/RTSP/record/playback/settings checks. Do not require 1h/2h/8h
  long tests, 50-cycle gates or production-grade DHCP/memory policies for Demo
  completion. Keep failures visible and essential resource/data safety intact.
- Business source of truth is this project, not a second copy inside the SDK.
- Build only inside Docker using `bash tools/build.sh little` or
  `bash tools/build.sh big`. Do not invoke cross-compilers on the host.
- Temporary CLOCK/PHY diagnostic firmware uses `bash tools/build.sh touch-clock-phy`
  (also Docker-only). It deliberately skips panel/VO init; never deploy it as a
  normal release. Preserve the connector-only test until touch diagnosis ends.
- The next DSI/panel diagnostic uses `bash tools/build.sh touch-dsi-panel` and
  reuses the archived CLOCK/PHY test ELF unchanged; its firmware skips VO init.
- VO/VTTH isolation uses `bash tools/build.sh touch-vo-vtth-off`, reuses the same
  ELF and restores VO scanning while requesting VTTH off. This is diagnostic
  firmware, not a release or proof that the hardware interrupt is masked.
- VO configuration isolation uses `bash tools/build.sh touch-vo-config`, reuses
  the same ELF, keeps VO init/parameters and skips only the final VO enable.
  Cold boot before testing; skipping enable does not undo earlier hardware state.
- Timestamp isolation uses `bash tools/build.sh touch-timestamp`, reuses the
  same ELF and adds only kd_vo_timestamp_enable while keeping VO enable skipped.
  Its logs confirm the call, not a hardware timestamp state transition.
- DSI pattern isolation keeps the TIMESTAMP_ONLY firmware unchanged and builds
  only the application with `bash tools/build.sh big` (dsi_test_mode=1).
  Archive the new ELF separately; no image rebuild or VO enable for this test.
- LCKFB timing candidates are archived separately (27000 and 24750 kHz).
  Current source is restored to 27000 kHz / divider 21 per user request.
- The separate PHY lane test uses `bash tools/build.sh touch-phy-2lane` to rebuild
  MPP/RTT and the image, reuses the exact archived 24750 kHz ELF, and changes only
  ST7701 PHY lane count to two. Keep VO enable skipped and touch code unchanged.
- Archived panel-sequence test uses `bash tools/build.sh touch-panel-lckfb`.
  Keep the archived 27 MHz ELF, restore K_DSI_4LAN, replace only the 480x800
  init body with the user-supplied sequence. DSI/VO logic and touch remain intact.
  This is diagnostic firmware; improvement alone does not rule out touch issues.
- User reported the panel-sequence pattern test normal and requested UI integration.
  Current source now restores the normal big-core entry, dsi_test_mode=0, VO enable
  and VTTH=1. Keep 27 MHz / 324 Mbps, divider21, PHY n3/m52/voc0x1f/hs0xb5,
  K_DSI_4LAN and the exact passing panel command body. Do not restore diagnostics
  or modify touch thresholds/filtering without a new isolation request.
  `bash tools/build.sh touch-ui` builds both apps in Docker, explicitly regenerates
  ROMFS, then builds MPP/RTT and a whole-card UI integration candidate. Check exact
  ELF bytes inside the RTT payload, not just the root/bin staging copy.
  Latest verified image is out/touch-ui/20261001T155329Z/artifacts/sysimage-touch-ui.img.
  The earlier 20261001T152743Z candidate has stale ROMFS and must not be flashed.
  Both cores retain manual startup for board acceptance. The user chose whole-card
  reflashing; do not substitute live partition writes or auto-reboot over SSH.
- Portrait X reflection and preview diagnostic updates (2026-10-02) are app-only,
  archived in out/touch-preview/20261001T164338Z. Mapping lives in liblv_drivers.so;
  updating only the UI executable is insufficient. Old touch-ui SD images do not
  contain this increment. On 2026-10-02 the user approved backup and app/library
  synchronization to root@192.168.123.74. Big serial list_process confirmed no
  business app before installation. Do not change network/kernel or reflash SD;
  the user starts the big app on serial before we start the little UI.
  Keep the passing clock/PHY/panel/kernel state unchanged. Focused controller tests
  use `bash tools/test.sh preview`; successful binding is not proof of camera frames.
- SDK dependencies are selected by `K230_SDK_ROOT`.
- Big core owns camera, display and hardware encoding. Little core owns RTSP
  networking and MP4 file handling. Do not launch demo binaries for business APIs.
- V1 uses one Linux business process (UI + control + media). Standalone service
  is disabled. Do not claim board validation based only on compilation.
- Playback SDK demux runs in the same executable's private --mp4-demux helper;
  it owns only parsing, never MAPI/VB/DRM. Keep the 8-second exchange deadline,
  bounded frames, read-only fd checks and child reaping. This is crash isolation,
  not a security sandbox. Use `bash tools/test.sh demux` for real-SDK/mock parser
  tests or `bash tools/test.sh playback` for the focused worker mock.
- IPv4 boot setup uses the same binary's --ipv4-stage/--ipv4-boot modes without
  DRM/media init. Install little/S45wifi together with the matching UI. Rootfs
  packaging must restore the target S45wifi after the SDK's legacy overlay.
- Saving IPv4 settings never changes the live link. Keep pending, staged boot,
  and applied runtime policies distinct; do not silently run DHCP in static mode.
- Apply Now changes only the live IPv4 policy after explicit second confirmation.
  Hold the boot/application lock through recovery, mark uncertain before network
  mutation, and clear it only after successful apply/verification and commit.
  Do not treat local setup success as remote reachability or deploy over sole SSH.
- DHCP uses BusyBox udhcpc without -q; its background client outlives the UI.
  Stop only the verified job through pidfd, and require a successful lease hook
  before reporting acquisition. Deploy matching UI and S45wifi together.
- DHCP lease status is v2 (error/duration/IP/hook-entry CLOCK_BOOTTIME/boot UUID).
  Untimed legacy records are unverified; never infer a lease from file mtime or
  process liveness. Preserve the hook-entry stamp through slow application,
  expiry/boot checks, final job/pidfd validation and one-second passive polling.
  Network page STATUS is passive (~5 seconds); never auto-scan/join/renew.
  Keep scan_generation distinct so lease updates cannot replay old scans.
  Current BusyBox ignores hook exit status; no project-specific udhcpc change
  has been approved/installed yet. Observer expiry is not DHCPDECLINE or actual
  address withdrawal/reacquisition. Focused checks: `bash tools/test.sh dhcp`.
- IPv4 candidate probing is fail-closed: static Apply Now checks before marker/
  DHCP stop; boot/rollback and new DHCP lease hooks probe before IP mutation.
  Keep udhcpc's attached -a2000 option; hook failures are not DHCPDECLINE proof.
  ARP transport tests use `bash tools/test.sh arp-transport` in a network-none
  Docker namespace, never host networking. No probes guarantee future freedom.
- Recording directory uses the legacy one-line lawrec-record-dir file through
  settings APIs; Save freezes the current process snapshot before persistence.
  Enforce the 80-byte directory / 128-byte SDK filename bound, fail closed on
  malformed config, and never follow directory symlinks or auto-create/format.
  Targeted storage/settings/playback checks use `bash tools/test.sh storage`.
- Media settings writes schema v2, reads v1 with 30-FPS defaults without rewriting.
  Target FPS is 15/30 at fixed 1280x720/source30; the first subscriber sets shared
  VENC0 parameters, later subscribers must reuse them and actual hardware PTS.
  Save requires confirmation and UI restart; do not hot-apply encoding settings.
  Older binaries need their v1 config restored too. Focused configuration/encoder
  tests use `bash tools/test.sh media-settings`; they do not measure board FPS.
- Preserve the big-core display chain when disabling camera preview output.
- Keep generated artifacts under `out/` or the SDK Buildroot output directory.
- `bash tools/package.sh --snapshot` creates an offline applications-only dev
  bundle without committing dirty work. Default RC mode still requires clean Git.
  Package source/content/mode and selected SDK display/touch drift must fail closed;
  do not edit source during packaging. Ship S45wifi, S99lawrec and both LVGL/input
  libraries with the matching UI/big app. Package checks do not prove kernel/board
  compatibility; SDK is unversioned and selected inputs are not a full SDK archive.
  No install/autostart/network mutation is performed. Test: `bash tools/test.sh package`.
- Settings previews use `bash tools/render-ui.sh` in network-none Docker with
  real LVGL/pages and fixture backends. Inspect keyboard/modal screenshots, not
  just initial pages; preview success is not hardware acceptance. Regenerate
  Chinese subsets with `bash tools/update-ui-fonts.sh` when adding UI copy.
- File browsing uses constant-memory filename cursor pages (four UI entries), not
  the legacy truncated text list. Delete confirmation freezes identity/size/mtime
  and revalidates; this is not atomic against external rename/unlink races. Never
  show/delete .part or symlink entries. Focused real-FS checks: `bash tools/test.sh files`.
  Playback panels must leave y=200..469 transparent for big-core video. Worker
  inactive is not media lease release; show retained resource protection. Preview
  fixtures verify UI, not real decoding/audio/SD I/O; no seek/speed added here.
- Preview EXIT request/failure is not a closed display. Preserve the control
  reservation until a matching successful EXIT, drain both media consumers,
  handle timeout on the UI thread without queue dependence, and invalidate old
  requests across IPC generations. Use `bash tools/test.sh ui-preview-ipc` for
  production consumer/control mock coverage; it is not camera/transport evidence.
- UI startup and changed IPC generations reserve display until a matching
  read-only DISPLAY_QUERY confirms readiness/occupancy after local consumers stop.
  Never auto-resume media or apply a superseded query. Orphan playback display
  is a separate guard, not cleared by preview EXIT; generic orphan codec recovery
  remains outstanding. Upgrade both apps; the big IPC owner handles registration,
  reconnect, handle-pinned sends and stop. Keep display/touch/kernel unchanged.
- Explicit DISPLAY_RECOVER uses confirmation and the single pending display
  transaction. Prepare under control lock; reject live preview/media, poisoned
  playback and ANY retained media lease. Big display rollback keeps occupancy
  on failure; only a matching successful recovery clears display guards. It is
  not orphan codec/VB cleanup. Preserve original layer geometry and never alter
  connector/clock/PHY/touch. Maintenance screenshots use fixture recovery only.
- Recording links generated adaptations of the fingerprinted SDK mp4_format.c,
  mov-writer.c, mov-stts.c and mov-elst.c through record/prepare_mp4.cmake.
  Preserve checked writer/
  stdio/close errors, failed fast-start cleanup and the 2-MiB conversion bound.
  Use `bash tools/test.sh record-io` for real-SDK/failure tests in Docker. Review
  SDK changes before changing the fingerprints; never silently use the old
  unchecked archive wrapper or remove healthy fast-start to mask failures.
- RTSP/record diagnostics use queue-local steady-clock residence, /proc snapshots
  and worker-owned MP4 sample-index statistics, roughly every 10 seconds. Keep
  validity bits, cumulative history across close, and reset at subscription.
  Index bytes are not total RSS/VB; source callback attempts are not received RTP.
  No automatic unsegmented memory-limit policy has been approved. Preserve the
  default behavior; recommend explicit segmentation, not an invisible fallback.
- Normal recording stop drains audio to the last accepted video PTS plus one
  nominal frame period, with a 250-ms queue wait budget before detachment.
  Keep queue finish/discard semantics distinct, SDK stop outside callback/state
  locks, sticky delivery errors and RTSP's shared audio ownership. Clip G711A
  with a retained packet cursor at startup/IDR/stop; wait for IDR audio coverage
  before publishing. Finish a validated in-flight video frame if stop arrives,
  then admit no next frame. End-track metadata must precede track destruction.
  Container timing is still milliseconds and sample allocation assumes SDK PTS
  names the first sample; do not claim board AV synchronization from unit tests.
- Shared audio/video source errors are sticky until all owners leave. Check known
  callback failures before accepting a startup subscription; never hold callback
  locks across SDK teardown. Quarantine retains the first normalized cleanup
  error and logs every raw SDK failure. Test: `bash tools/test.sh media-lifecycle`.
  Network operation feedback is separate from passive connection/lease status;
  refreshing status must not erase saved/validation results or initiate recovery.
- RTSP source queues complete H264 access units, not individual NAL slots; retain
  full backing-buffer bytes until the last NAL is consumed. Keep 4-MiB input,
  8-MiB/count queue bounds, bounded parsing, whole-IDR congestion recovery and
  sticky delivery errors. LIVE requires parsed source input, not callback attempts;
  neither source counters nor afterGetting NALs prove RTP/VLC reception. G711
  sink's temporary smaller packet limit must be exception-safe and restored before
  H264 PLAY. Test: `bash tools/test.sh rtsp-source` (real live555 source, not board).
- Playback keeps delivery and first cleanup errors distinct. stop_wait returns
  retained-resource cleanup failure even after the worker exits; clean teardown
  after a bad file is not a stop failure. Shutdown logs all three worker waits
  and the media owner mask and exits nonzero on unconfirmed release. No UI-only
  restart claim or automatic remote codec reset. Test: `bash tools/test.sh playback`.
