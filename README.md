# Lawrec

Business sources live here. The K230 SDK supplies platform libraries, toolchains,
Buildroot and image packaging. Migration preserves the current application logic;
the UI/service refactor is still incomplete.

## Layout

- `big/`: RT-Smart camera, display and AI application.
- `little/src/`: Linux UI, control, RTSP, recording and shared protocol.
- `little/`: Buildroot package integration and init script.
- `docs/`: existing design and development records (historical paths may be stale).
- `tools/`: Docker-only build entrypoints.
- `out/`: generated files; `out/legacy/` holds previous build artifacts.
- `baseline/`: existing rollback archive.

## Build

Run from this directory:

```sh
export K230_SDK_ROOT=/home/atlantis/k230_sdk
bash tools/build.sh little
bash tools/build.sh big
```

The default board is `k230_canmv_lckfb_defconfig`. Override `LAWREC_BOARD` when
needed. The SDK must already have its dependencies built. Docker uses the caller's
UID/GID. Existing root-owned SDK outputs may need their ownership repaired first.

Big-core output: `out/big/lawrec.elf`.
Little-core output: `$K230_SDK_ROOT/output/$LAWREC_BOARD/little/buildroot-ext/target/app/lawrec/`.
These commands do not deploy to a board or rebuild the SD image.

## SDK integration

The SDK's `src/reference/business_poc/lawrec` and
`src/little/buildroot-ext/package/lawrec` are compatibility symlinks to this project
and `little/`. There is only one editable copy of the business sources.
If either repository is relocated, recreate these symlinks. Both directories must
be mounted into Docker; the supplied build scripts do this automatically.

Keep shared small-core headers in `little/src/common/` for now to preserve the
Buildroot source layout. Hardware drivers and board configuration stay in the SDK.
