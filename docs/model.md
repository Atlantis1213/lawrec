# Model And Third-Party Materials

The only deployed model is SDK `src/big/kmodel/door_lock/retinaface.kmodel`:

```text
SHA256 082f76bec6db39ee7a9c4c83c2d8196fdfa0ba487b7d07831647df01dee8d808
input  uint8 NCHW RGB 1x3x320x320
output nine float32 NCHW heads; MobileRetinaFace, 4200 anchors
```

The application package copies these exact locally supplied SDK bytes to
`models/retinaface.kmodel`; packaging refuses a different hash. Git contains no
model. Vision does not need mbface, a face database or old lawrec assets.

Reference ONNX: SDK `src/big/nncase/examples/models/mobile_retinaface.onnx`,
SHA256 `b0abe364a3f82ce1714589479494ef440d9fdf3f02218a77cfd20bc7679df336`.
Reference compiler script: nncase/examples/scripts/mobile_retinaface.py, RGB,
swapRB=false, built-in mean [123,117,104], std=1. This is provenance from the
installed SDK, not a reproduced compilation or verified hardware numerical result.

Local package use does not establish external redistribution rights. Consult
the SDK/model's original license and supplier terms before publishing the model
or an application archive. No new license is granted for third-party materials.

The package also contains selected SDK runtime binaries (including its matching
glibc loader) and static SDK code linked into applications. `meta/runtime.tsv`
records each dynamic binary's actual source/hash. SDK/LVGL/live555/nncase/MP4/
RT-Thread and toolchain libraries retain their upstream licenses; source paths
and adaptation notes are in sdk-baseline.md. LVGL and board-adapter copyright
headers remain in the repository; the derived linker script is Apache-2.0.
For distribution, separately audit source/license/notice obligations against
the exact local SDK, particularly glibc/libstdc++ and live555. This demo bundle
is an internal development artifact, not a cleared redistributable SDK release.

Native x264 and FFmpeg are offline test tools only; neither is shipped.
