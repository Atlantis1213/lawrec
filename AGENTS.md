# Development rules

- Business source of truth is this project, not a second copy inside the SDK.
- Build only inside Docker using `bash tools/build.sh little` or
  `bash tools/build.sh big`. Do not invoke cross-compilers on the host.
- SDK dependencies are selected by `K230_SDK_ROOT`.
- Big core owns camera, display and hardware encoding. Little core owns RTSP
  networking and MP4 file handling. Do not launch demo binaries for business APIs.
- UI/service ownership refactoring is incomplete. Do not claim board validation
  based only on successful compilation.
- Preserve the big-core display chain when disabling camera preview output.
- Keep generated artifacts under `out/` or the SDK Buildroot output directory.
