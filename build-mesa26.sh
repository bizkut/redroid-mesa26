#!/usr/bin/env bash
set -euo pipefail

HERE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
if [[ -e $HERE/../../../.git ]]; then
    ROOT=$(CDPATH='' cd -- "$HERE/../../.." && pwd)
else
    ROOT=$HERE
fi
SOURCE_DIR=${SOURCE_DIR:-$HOME/workspace/redroid-a14-mesa26}
UPSTREAM_PATCHES_DIR=${UPSTREAM_PATCHES_DIR:-$HOME/workspace/redroid-patches}
IMAGE_DIR=${IMAGE_DIR:-$ROOT/build/redroid/mesa26}
CCACHE_DIR=${CCACHE_DIR:-$HOME/.cache/redroid-mesa26-ccache}
JOBS=${JOBS:-8}
REPO_SYNC_JOBS=${REPO_SYNC_JOBS:-4}
DOCKER_CPUS=${DOCKER_CPUS:-8}
DOCKER_MEM=${DOCKER_MEM:-12g}
DOCKER_SWAP=${DOCKER_SWAP:-36g}
BUILDER_IMAGE=${BUILDER_IMAGE:-redroid-mesa26-builder:android14-mesa26}
ANDROID_IMAGE=${ANDROID_IMAGE:-redroid-rootfs:android14-mesa26}
NATIVE_BRIDGE_IMAGE=${NATIVE_BRIDGE_IMAGE:-redroid/redroid@sha256:0a611199ba2e0b5d60af39b3327a517f6407231f4352114ed3bd3cbfe2be69aa}
NATIVE_BRIDGE_MANIFEST="$HERE/native-bridge.sha256"
AOSP_TAG=android-14.0.0_r75
PATCH_COMMIT=a5c70c0dd89c12b5027c902ab4182f31d38f3bb7
export GIT_COMMITTER_NAME=${GIT_COMMITTER_NAME:-ReDroid Mesa 26 Builder}
export GIT_COMMITTER_EMAIL=${GIT_COMMITTER_EMAIL:-redroid-mesa26@localhost}

fail() { printf '%s\n' "$*" >&2; exit 1; }
fingerprint() {
    {
        (
            cd "$HERE"
            find local-manifests patches -type f \
                \( -name '*.xml' -o -name '*.patch' \) -print0 |
                sort -z | xargs -0 sha256sum
        )
        (
            cd "$UPSTREAM_PATCHES_DIR"
            find "$AOSP_TAG" -type f \
                \( -name '*.xml' -o -name '*.patch' \) -print0 |
                sort -z | xargs -0 sha256sum
        )
    } | sha256sum | cut -d' ' -f1
}
apply_series() {
    local project=$1 directory=$2 patch
    for patch in "$directory"/*.patch; do
        [[ -f $patch ]] || continue
        git -C "$SOURCE_DIR/$project" am --whitespace=nowarn "$patch"
    done
}
prepare() {
    [[ $(git -C "$UPSTREAM_PATCHES_DIR" rev-parse HEAD) == "$PATCH_COMMIT" ]] ||
        fail "Expected redroid-patches commit $PATCH_COMMIT"
    [[ -z $(git -C "$UPSTREAM_PATCHES_DIR" status --porcelain) ]] ||
        fail 'The upstream patch checkout must be clean.'
    mkdir -p "$SOURCE_DIR"
    SOURCE_DIR=$(realpath "$SOURCE_DIR")
    local marker="$SOURCE_DIR/.repo/redroid-mesa26-prepared" expected
    expected=$(fingerprint)
    if [[ -f $marker ]]; then
        [[ $(cat "$marker") == "$expected" ]] || fail 'Prepared inputs changed; use a fresh SOURCE_DIR.'
        printf 'Source already prepared; not syncing (sync would discard detached patch commits).\n'
        return
    fi
    [[ ! -d $SOURCE_DIR/.repo ]] || fail 'Existing checkout without preparation receipt; use a fresh SOURCE_DIR to avoid discarding local work.'
    (cd "$SOURCE_DIR" && repo init -u https://android.googlesource.com/platform/manifest -b "$AOSP_TAG" --depth=1)
    mkdir -p "$SOURCE_DIR/.repo/local_manifests"
    cp "$HERE"/local-manifests/*.xml "$SOURCE_DIR/.repo/local_manifests/"
    (cd "$SOURCE_DIR" && repo sync -c -j"$REPO_SYNC_JOBS" --no-tags --no-clone-bundle)
    local directory project
    while IFS= read -r directory; do
        project=${directory#"$UPSTREAM_PATCHES_DIR/$AOSP_TAG/"}
        apply_series "$project" "$directory"
    done < <(find "$UPSTREAM_PATCHES_DIR/$AOSP_TAG" -type f -name '*.patch' -printf '%h\n' | sort -u)
    apply_series device/redroid "$HERE/patches/device_redroid"
    apply_series device/redroid-prebuilts "$HERE/patches/device_redroid-prebuilts"
    apply_series system/libvintf "$HERE/patches/system_libvintf"
    if [[ -d $HERE/patches/mesa26 ]]; then
        apply_series external/mesa26 "$HERE/patches/mesa26"
    fi
    printf '%s\n' "$expected" > "$marker"
}
build() {
    [[ $JOBS =~ ^[1-9][0-9]*$ ]] || fail 'JOBS must be a positive integer.'
    [[ -d $SOURCE_DIR/.repo ]] || fail 'Run prepare first.'
    mkdir -p "$IMAGE_DIR" "$CCACHE_DIR"
    docker build --build-arg userid="$(id -u)" --build-arg groupid="$(id -g)" \
        --build-arg username="$(id -un)" -t "$BUILDER_IMAGE" "$HERE"
    docker run --rm --cpus="$DOCKER_CPUS" --memory="$DOCKER_MEM" --memory-swap="$DOCKER_SWAP" \
        -v "$SOURCE_DIR:/src" -v "$CCACHE_DIR:/ccache" "$BUILDER_IMAGE" -lc \
        "source build/envsetup.sh && lunch redroid_x86_64_only-ap2a-userdebug && m -j$JOBS" \
        2>&1 | tee "$IMAGE_DIR/build.log"
}
package() {
    local output="$SOURCE_DIR/out/target/product/redroid_x86_64_only"
    [[ -f $output/system.img && -f $output/vendor.img ]] ||
        fail 'Build system.img and vendor.img first.'
    [[ -f $NATIVE_BRIDGE_MANIFEST ]] || fail 'Native bridge manifest is missing.'
    mkdir -p "$IMAGE_DIR"

    local stage bridge_container='' digest path extra
    local -a bridge_paths=(
        system/bin/arm64
        system/etc/binfmt_misc
        system/lib64/arm64
    )
    while read -r digest path extra; do
        [[ $digest =~ ^[0-9a-f]{64}$ && -n $path && -z ${extra:-} &&
           $path == system/* && $path != *..* ]] ||
            fail 'Native bridge manifest contains an unsafe row.'
        bridge_paths+=("$path")
    done < "$NATIVE_BRIDGE_MANIFEST"
    [[ ${#bridge_paths[@]} -eq 95 ]] ||
        fail 'Native bridge manifest has an unexpected file count.'

    stage=$(mktemp -d)
    cleanup() {
        if [[ -n $bridge_container ]]; then
            docker rm -f "$bridge_container" >/dev/null 2>&1 || true
        fi
        sudo umount "$stage/vendor" 2>/dev/null || true
        sudo umount "$stage/system" 2>/dev/null || true
        sudo rm -rf "$stage/native-bridge" 2>/dev/null || true
        rmdir "$stage/vendor" "$stage/system" "$stage" 2>/dev/null || true
    }
    trap cleanup EXIT
    mkdir "$stage/system" "$stage/vendor" "$stage/native-bridge"
    sudo mount -o loop,ro "$output/system.img" "$stage/system"
    sudo mount -o loop,ro "$output/vendor.img" "$stage/vendor"

    bridge_container=$(docker create "$NATIVE_BRIDGE_IMAGE")
    docker export "$bridge_container" |
        sudo tar --extract --no-recursion --xattrs --same-owner --same-permissions \
            -C "$stage/native-bridge" "${bridge_paths[@]}"
    docker rm "$bridge_container" >/dev/null
    bridge_container=
    (
        cd "$stage/native-bridge"
        sha256sum -c "$NATIVE_BRIDGE_MANIFEST"
    )
    [[ $(readlink "$stage/native-bridge/system/lib64/libnb.so") == libndk_translation.so ]] ||
        fail 'Native bridge libnb.so link is invalid.'
    [[ -x $stage/native-bridge/system/bin/arm64/app_process64 &&
       -x $stage/native-bridge/system/bin/ndk_translation_program_runner_binfmt_misc_arm64 ]] ||
        fail 'Native bridge executables are unavailable.'

    sudo rm -f "$IMAGE_DIR/rootfs.tar"
    sudo tar --xattrs --xattrs-include='*' -cf "$IMAGE_DIR/rootfs.tar" \
        -C "$stage" vendor \
        -C "$stage/system" --exclude='./vendor' . \
        -C "$stage/native-bridge" system
    docker import --platform=linux/amd64 \
        -c 'ENTRYPOINT ["/init","qemu=1","androidboot.hardware=redroid"]' \
        "$IMAGE_DIR/rootfs.tar" "$ANDROID_IMAGE"
    cleanup
    trap - EXIT
    printf 'Packaged %s with pinned ARM64 native bridge from %s\n' \
        "$ANDROID_IMAGE" "$NATIVE_BRIDGE_IMAGE"
}
case ${1:-} in
    prepare) prepare ;;
    build) build ;;
    package) package ;;
    *) printf 'Usage: %s {prepare|build|package}\n' "$0" >&2; exit 2 ;;
esac
