# SDK and Hardware Baseline

Captured from the existing workspace, without SDK or board modification.

- Old repository: `/home/atlantis/lawrec`, master,
  `7c51e46bb177cd9f962b6877d48400dff41b759d`.
- Old worktree has modified `docs/当前状态.md` and untracked
  `docs/lawrec_development_plan.md`; both preserved. Plan was copied into docs/plan.md.
- SDK: `/home/atlantis/k230_sdk` resolves to `/home/atlantis/Workspaces/SDKs/k230_sdk`.
  Not a Git repository. Do not invent an SDK commit/version.
- Board: `k230_canmv_lckfb_defconfig`.
- Docker: `ghcr.io/kendryte/k230_sdk`, image ID
  `sha256:1f9f6be7e7bf6fdbc2c718a224e0cf23366ddc7e1e4b5dc3083635b3f02fd22c`.
- LVGL: SDK's existing v8.3.1 extraction, no network download.
- SDK and toolchains are mounted read-only in the Docker entrypoint.

## Frozen Adaptations

The following source checksums identify the passing panel/input baseline, not
proof of current camera/AI/media operation. No new touch diagnostics are added.

| File | SHA256 |
| --- | --- |
| `patches/st7701.baseline.c` = SDK `src/big/mpp/kernel/connector/src/st7701.c` | `e71f9f6105bbf00fa73202891c1ec8e7c076337c4a3549e607873e00f122cbf2` |
| `compat/lvgl_port/lv_conf.h` | `65c0f127eb6a50f86e3fc93a2f661ecbec7e017787553a42ac8dbfd37880f776` |
| `compat/lvgl_port/k230/lv_port_indev.c` | `8ec6ef46baac1fd2360eae851ebafdf7af7cd8d4fcf6bd642f5991e78d1523aa` |
| `compat/lvgl_port/k230/lv_port_disp.cpp` | `99cd0d4db3116a83344c0e1732c1d03ae4d85029bac1bd8d9a2ec6177e63753b` |
| SDK `src/big/mpp/userapps/src/connector/mpi_connector.c` | `c0985f3b38fbd7b7bb0c9bb5e74e9ba5b51378feecec3c0f5c3c5ba99da993a9` |
| SDK Linux `drivers/input/touchscreen/edt-ft5x06.c` | `b5ba2b37bf0d4d819ad041fbb622d4f9182af6a311655053fec2ece525be2536` |
| SDK kernel `libvicap.a` | `1eecf42cd02ca3c5f40dc87bd57eb0dba59aa246697201e8133a63f609465134` |
| SDK LVGL `lvgl.h` | `c02cc2657e6d8babae3ce8049b85866f5678244c4568cc9529ead0c0004d6c31` |
| SDK `.config` | `6fe7d0e29dd9362ea418085b84838b90bdf195bc3e95f8b46e3a254301162912` |
| SDK Buildroot `.config` | `2f07968d1b7f6e8815aea882d46763a13014aaa2f5bbb69dd6ff99c88c9f2c60` |

Adapter files and font are copied unchanged with original copyright headers.
`st7701.baseline.c` is a reference snapshot, not compiled or auto-installed.
Keep panel power/reset/init solely in vision. The candidate must use 27 MHz/div21,
324 Mbps, n3/m52/voc0x1f/hs0xb5, existing four-lane PHY and normal VO/VTTH=1.
Linux input keeps existing X reflection. No firmware, clock or touch changes.

The SDK's existing binaries have no comprehensive provenance. Final packaging
must also capture every shipped application/library/model hash and explicitly
state that selected file hashes are not a complete SDK backup.

## IPC Archive Selection

This local SDK's directory names/config.mk do not reliably identify the actual
prebuilt compiler variant. Follow inspected archive metadata and the old Linux
build's explicit selection, not an assumption that Linux must use slave/lib:

- Linux-compatible ipcmsg/host/lib/libipcmsg.a: GCC 10.2 Xuantie V2.6,
  rv64imafdc/xthead attributes, SHA256
  `d1b1ecae0434cc4376fa238560355bf201c07a0499bb1144e138d6b6972759a2`.
- ipcmsg/slave/lib/libipcmsg.a: GCC 12 prerelease, vector 1.0/Zve attributes,
  SHA256 `37b59409b696ef5c1f15ab7b12c1c2b4967f630d07fd77e63a341abb26a73342`.
  Linking it with the existing Linux V2.6 toolchain fails (unknown Zve extension).

Linux now links the compatible host archive, as the old project does. No SDK
files, compiler versions or ELF attributes were changed/stripped. Big integration
also uses the existing host archive; successful linkage does not prove either
core's IPC transport or runtime ABI on hardware.

## Application Linker Script

`big/link.lds` derives from SDK nncase/examples/cmake/link.lds (RT-Thread,
Apache-2.0), preserving startup symbols and 0x200000000 entry. It replaces
wildcards matching relocation metadata as GOT, collects small/read-only data,
retains TLS, and defines separate page-start RX/RW load segments. The actual
SDK load_elf implementation maps non-PROGBITS allocations as writable, so the
correctly typed, all-zero RELA metadata lives after the text page boundary.
SDK/toolchain sources and binaries are unchanged. Structural loader assumptions
are checked by Docker verify; on-board program loading remains pending.

## Private MP4 Build

Linux media compiles the existing CDK `middleware/mp4_format/src/libmov` and
`libflv` sources instead of the unchecked high-level kd_mp4 wrapper. Library
source copies live only in `out/*/sdk-mp4`; CRLF is normalized there for a checked
CMake end-track extension. The patch records explicit final PTS boundaries for
stts and track duration, and fixes the stock sentinel assertion's evaluation
order. The original SDK sources and libmp4.a are not modified or installed.

Native x264 and FFmpeg are offline test tools only, copied from SDK big MPP
middleware sources to `out/tests/native`, configured and built in Docker. No
network download, host compilation or board update is used. Their binaries and
generated black-video/G711A sample files are not part of the deployment package.
