# Demo Development Rules

- Follow docs/plan.md. Complete preview, face detection/OSD, shared H.264/G.711A,
  RTSP, MP4 and one touch page. No WiFi/DHCP/ARP, identity DB, playback or recovery framework.
- All builds/tests run through tools/build.sh in K230 SDK Docker. Never compile
  on the host, connect to the board, deploy, reflash, or mutate SDK sources.
- vision owns camera/VB/VO/AI/OSD; media_service owns codecs/queues/RTSP/MP4;
  demo_ui owns LVGL/input. No duplicated hardware initialization.
- Preserve compat/lvgl_port and patches/st7701.baseline.c byte-for-byte. Keep
  27 MHz/div21, PHY n3/m52/voc0x1f/hs0xb5, 4 lanes, panel sequence and VTTH=1.
- SDK_ROOT is external, no SDK/toolchain/model/binary/out files in Git. Explicit
  source lists for business code; SDK LVGL sources are third-party, not business.
- Keep new glue files roughly below 500 lines. Only useful focused offline tests.
- Record work, actual commands/results and remaining risks in docs/devlog.md.
  Commit each verified task using explicit paths; no automatic push/history edits.
- Offline compile, mock, and library tests never prove hardware success. Never
  mark the full goal complete before every offline deliverable is present.
