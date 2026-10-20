# ReDroid Android 14 with Mesa 26

Reproducible source recipe for an x86_64-only Android 14 ReDroid image with Mesa 26 RadeonSI OpenGL ES and RADV Vulkan host-GPU support. The recipe pins every non-AOSP source revision, applies reviewable mailbox patches, builds Android in a bounded Docker environment, and packages an importable Docker image.

This repository contains source, manifests, patches, checksums, and build automation only. It does not contain Android images, APKs, game data, account data, native-bridge payload bytes, or build outputs.

## Qualified boundary

The current recipe has produced `redroid-rootfs:android14-mesa26` and passed a bounded hardware smoke on AMD Oberon/GFX1013:

| Surface | Observed result |
|---|---|
| Android | 14 / API 34, x86_64 with ARM64 native translation |
| OpenGL ES | Mesa 26.2.5 RadeonSI/ACO, GLES 3.2, exact `ff00ffff` draw/readback |
| Vulkan | Mesa 26.2.5 RADV, AMD `1002:13fb`, graphics queue, 4096-byte fill/readback |
| Application smoke | One ARM64 application cold-launched through native translation and reached its hardware-rendered first-run UI |

This is build and bounded operation evidence. It is **not** Vulkan conformance, general application compatibility, physical ARM64 qualification, or a security boundary.

## Pinned sources

| Component | Revision |
|---|---|
| AOSP | `android-14.0.0_r75` |
| ReDroid patch set | `a5c70c0dd89c12b5027c902ab4182f31d38f3bb7` |
| Mesa 26.2.5 | `ddaf85e353442d9d7f192fdcf1823e652e3f2ee3` |
| libdrm 2.4.133 | `cb4669afe87470752643f46c81bddb45b0db48e4` |
| gbm_gralloc | `a8874a18ad7884ea713f45047efad080fe973712` |
| Native-translation source image | `redroid/redroid@sha256:0a611199ba2e0b5d60af39b3327a517f6407231f4352114ed3bd3cbfe2be69aa` |

`local-manifests/redroid.xml` pins the remaining ReDroid projects. `native-bridge.sha256` verifies every extracted native-translation path before packaging. Host glibc files are never copied into the Android root filesystem.

The review branches [`bizkut/device_redroid@redroid-14.0.0-mesa26`](https://github.com/bizkut/device_redroid/tree/redroid-14.0.0-mesa26) and [`bizkut/device_redroid-prebuilts@mesa26-source-graphics`](https://github.com/bizkut/device_redroid-prebuilts/tree/mesa26-source-graphics) expose the two small ReDroid changes as normal Git history. Reproducible builds use the pinned upstream bases plus this repository's patch series; they do not depend on mutable fork branches.

## Host requirements

- Linux x86_64 with Docker Engine and Compose v2
- Android `repo`, Git, ccache, Python 3, `sha256sum`, and passwordless noninteractive `sudo` for loop-mount packaging
- A kernel capable of loop-mounting the generated ext4 Android images
- Recommended: 8 CPU threads, 32 GiB effective memory including swap, and at least 350 GiB free storage
- For runtime host GPU: `/dev/dri/card0`, `/dev/dri/renderD128`, matching kernel DRM support, and a trusted host

The build container pins Ubuntu 24.04 and its Python build tools. The large AOSP checkout, ccache, and Docker daemon remain host resources.

## Local build

```sh
git clone https://github.com/bizkut/redroid-mesa26.git
cd redroid-mesa26

git clone https://github.com/remote-android/redroid-patches.git "$HOME/workspace/redroid-patches"
git -C "$HOME/workspace/redroid-patches" checkout --detach \
  a5c70c0dd89c12b5027c902ab4182f31d38f3bb7

SOURCE_DIR="$HOME/workspace/redroid-a14-mesa26" \
UPSTREAM_PATCHES_DIR="$HOME/workspace/redroid-patches" \
./build-mesa26.sh prepare

SOURCE_DIR="$HOME/workspace/redroid-a14-mesa26" \
UPSTREAM_PATCHES_DIR="$HOME/workspace/redroid-patches" \
JOBS=8 ./build-mesa26.sh build

SOURCE_DIR="$HOME/workspace/redroid-a14-mesa26" \
./build-mesa26.sh package
```

Outputs:

- Docker image: `redroid-rootfs:android14-mesa26`
- Root filesystem archive: `build/redroid/mesa26/rootfs.tar`
- Build log: `build/redroid/mesa26/build.log`

`prepare` refuses a dirty upstream patch checkout, an unexpected patch commit, an existing unreceipted Android checkout, or changed inputs against an existing preparation receipt. Use a fresh `SOURCE_DIR` after changing manifests or patch series; the script will not discard local Android source work.

All resource knobs are environment variables: `JOBS`, `REPO_SYNC_JOBS`, `DOCKER_CPUS`, `DOCKER_MEM`, `DOCKER_SWAP`, `SOURCE_DIR`, `UPSTREAM_PATCHES_DIR`, `CCACHE_DIR`, `IMAGE_DIR`, `BUILDER_IMAGE`, and `ANDROID_IMAGE`.

## GitHub Actions

[`.github/workflows/build.yml`](.github/workflows/build.yml) has two lanes:

1. Every push and pull request runs syntax, ShellCheck, XML, native-bridge provenance, Compose, and GPU-probe compile validation on a GitHub-hosted runner.
2. A manual `workflow_dispatch` on `main` performs the complete Android build and packaging on a dedicated self-hosted runner.

A GitHub-hosted runner does not provide the persistent storage, loop-mount privilege, memory, or build duration required for a full AOSP image. The full job therefore requires a trusted Linux x64 runner with the custom label `redroid-builder`. It runs only when the repository owner manually dispatches the default branch; pull-request code never executes on the self-hosted runner.

Install the runner through **Settings → Actions → Runners**, add the `redroid-builder` label, then install the host requirements above. The runner account must access Docker and pass `sudo -n true`. Use a dedicated machine or VM: the build packages Android filesystems and must not share trust with untrusted workloads.

Optional repository variables:

| Variable | Default |
|---|---|
| `REDROID_SOURCE_ROOT` | `$HOME/.cache/redroid-mesa26/sources` |
| `REDROID_PATCHES_DIR` | `$HOME/.cache/redroid-mesa26/redroid-patches` |
| `REDROID_CCACHE_DIR` | `$HOME/.cache/redroid-mesa26/ccache` |
| `REDROID_REPO_SYNC_JOBS` | `4` |
| `REDROID_DOCKER_CPUS` | `8` |
| `REDROID_DOCKER_MEM` | `12g` |
| `REDROID_DOCKER_SWAP` | `36g` |

The workflow derives a source-cache directory from the manifests and patch content. Changed source recipes use a new checkout instead of deleting an older one. Successful runs retain the complete Docker image and rootfs archive on the dedicated runner and upload only the build log and provenance JSON to GitHub. Images are not automatically published.

## Run the image

The supplied Compose file is intentionally privileged and exposes ADB on loopback only:

```sh
docker compose up -d
adb connect 127.0.0.1:5558
```

Stop it without deleting its independent data directory:

```sh
docker compose down
```

Default data lives at `$HOME/.local/share/redroid-mesa26/data`. Override GPU device paths, resource limits, and Android geometry for the actual host before use. Never expose port 5555/5558 beyond a trusted loopback or authenticated tunnel.

## GPU smoke probe

`gpu-probe.c` is a bounded diagnostic, not a conformance suite. Build it for the Android image with the platform toolchain, then run it with a wall-clock timeout. It rejects known software renderers, requires an AMD non-CPU Vulkan device, checks one GLES pixel readback, and verifies one Vulkan transfer fill/readback.

## Licensing and attribution

Original orchestration and documentation are Apache-2.0; see [LICENSE](LICENSE). Patch files remain subject to the licenses and notices of the upstream projects they modify. See [NOTICE](NOTICE). ReDroid, Android, Mesa, libdrm, and native-translation payloads are third-party works and are not relicensed by this repository.
