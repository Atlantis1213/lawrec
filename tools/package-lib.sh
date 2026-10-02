#!/usr/bin/env bash
# Shared packaging checks. No build, deployment or board mutation here.

lawrec_source_manifest() (
    set -euo pipefail
    local root=$1 dest=$2 path
    mkdir -p "$dest"
    : > "$dest/SOURCE.sha256"
    : > "$dest/SOURCE.modes"
    : > "$dest/SOURCE.paths"
    cd "$root"
    git ls-files --cached --others --exclude-standard -z | sort -zu > "$dest/list.tmp"
    while IFS= read -r -d '' path; do
        [[ -e "$path" || -L "$path" ]] || continue
        case "$path" in
            .env.example) ;;
            .env|.env.*|*/.env|*/.env.*|.codex/*|.ssh/*|*/auth.json|auth.json|*/credentials.json|credentials.json|*.pem|*.key|*/wpa_supplicant.conf|wpa_supplicant.conf)
                echo "Refusing credential-like source path: $path" >&2; exit 1 ;;
        esac
        [[ -f "$path" && ! -L "$path" ]] || { echo "Source must be regular files: $path" >&2; exit 1; }
        sha256sum -- "$path" >> "$dest/SOURCE.sha256"
        stat -c '%a %n' -- "$path" >> "$dest/SOURCE.modes"
        printf '%s\0' "$path" >> "$dest/SOURCE.paths"
    done < "$dest/list.tmp"
    rm "$dest/list.tmp"
    [[ -s "$dest/SOURCE.sha256" ]] || { echo 'Empty source snapshot' >&2; exit 1; }
)

lawrec_assert_same_source() {
    local before=$1 after=$2
    cmp "$before/SOURCE.paths" "$after/SOURCE.paths" &&
        cmp "$before/SOURCE.sha256" "$after/SOURCE.sha256" &&
        cmp "$before/SOURCE.modes" "$after/SOURCE.modes" || {
            echo 'Source changed during packaging; no package will be published.' >&2
            return 1
        }
}

lawrec_sdk_capture() (
    set -euo pipefail
    local sdk=$1 dest=$2 path
    mkdir -p "$dest"
    for path in .config README.md \
        src/big/nncase/examples/cmake/Riscv64.cmake \
        src/big/nncase/examples/cmake/link.lds \
        src/big/mpp/kernel/connector/src/st7701.c \
        src/big/mpp/userapps/src/connector/mpi_connector.c \
        src/little/linux/arch/riscv/boot/dts/kendryte/k230_canmv_lckfb.dts \
        src/little/linux/drivers/input/touchscreen/edt-ft5x06.c; do
        [[ -s "$sdk/$path" && ! -L "$sdk/$path" ]] || { echo "Missing/linked SDK input: $path" >&2; exit 1; }
        mkdir -p "$dest/$(dirname "$path")"
        cp "$sdk/$path" "$dest/$path"
    done
    cd "$dest"
    find . -type f ! -name SDK.sha256 -print0 | sort -z | xargs -0 sha256sum > SDK.sha256
)

lawrec_stage_apps() {
    local root=$1 target=$2 dest=$3 name
    mkdir -p "$dest/app/lawrec" "$dest/lib" "$dest/etc/init.d" "$dest/docs" || return 1
    [[ -x "$target/app/lawrec/ui/ui" && -s "$root/out/big/lawrec.elf" ]] || return 1
    # Dereference library links so the bundle has no dependencies outside itself.
    cp -a "$target/app/lawrec/ui" "$dest/app/lawrec/" || return 1
    cp "$root/out/big/lawrec.elf" "$dest/lawrec.elf" || return 1
    chmod 755 "$dest/lawrec.elf" || return 1
    for name in liblvgl.so liblv_drivers.so; do
        [[ -s "$target/usr/lib/$name" ]] || return 1
        cp -L "$target/usr/lib/$name" "$dest/lib/$name" || return 1
        chmod 755 "$dest/lib/$name" || return 1
    done
    for name in S99lawrec S45wifi; do
        cmp "$root/little/$name" "$target/etc/init.d/$name" || return 1
        cp "$root/little/$name" "$dest/etc/init.d/$name" || return 1
        chmod 755 "$dest/etc/init.d/$name" || return 1
    done
    [[ -s "$root/docs/v1_验收与部署.md" && -s "$root/docs/持续开发验收清单.md" ]] || return 1
    # Keep relative links in the deployment/checklist documents usable, without
    # copying ignored local logs, credentials or generated files from docs/.
    (
        cd "$root" || exit 1
        while IFS= read -r -d '' name; do
            [[ "$name" = *.md && -f "$name" && ! -L "$name" ]] || continue
            cp --parents -- "$name" "$dest/" || exit 1
        done < <(git ls-files --cached --others --exclude-standard -z -- docs)
    ) || return 1
    cp "$root/tools/verify-app-package.sh" "$dest/verify.sh" || return 1
    chmod 755 "$dest/verify.sh" || return 1
}

lawrec_package_manifest() (
    set -euo pipefail
    cd "$1"
    [[ -z "$(find . -type l -print -quit)" ]] || { echo 'Bundle symlinks are not allowed' >&2; exit 1; }
    find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS
)
