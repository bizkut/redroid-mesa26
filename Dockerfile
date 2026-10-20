FROM ubuntu:24.04@sha256:68ee22004a9109b44973313b02ff7b1de250f1de0f5ecb628d73f579aaff33ac

ARG userid
ARG groupid
ARG username

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    bc bison build-essential ccache curl file flex fontconfig g++-multilib gcc-multilib \
    git-core glslang-tools gnupg lib32z1-dev libc6-dev-i386 libelf-dev libgl1-mesa-dev \
    libncurses-dev libssl-dev libxml2-utils lz4 ninja-build patchelf pkg-config \
    python3 python3-pip python3-venv rsync sudo unzip x11proto-core-dev \
    libx11-dev xsltproc zip zlib1g-dev zstd \
    && rm -rf /var/lib/apt/lists/*

RUN python3 -m venv /opt/mesa-python \
    && /opt/mesa-python/bin/pip install --no-cache-dir \
       meson==1.9.2 Mako==1.3.10 packaging==25.0 PyYAML==6.0.3

RUN existing_user="$(getent passwd "${userid}" | cut -d: -f1)"; \
    if [ -n "$existing_user" ]; then userdel --remove "$existing_user"; fi; \
    existing_group="$(getent group "${groupid}" | cut -d: -f1)"; \
    if [ -n "$existing_group" ]; then groupdel "$existing_group"; fi; \
    groupadd --gid "${groupid}" "${username}" \
    && useradd --create-home --uid "${userid}" --gid "${groupid}" "${username}" \
    && printf '%s ALL=(ALL) NOPASSWD: ALL\n' "${username}" >/etc/sudoers.d/android-builder

ENV HOME=/home/${username} \
    USER=${username} \
    PATH=/opt/mesa-python/bin:/usr/lib/ccache:/src/prebuilts/jdk/jdk17/linux-x86/bin:${PATH} \
    USE_CCACHE=1 \
    CCACHE_EXEC=/usr/bin/ccache \
    CCACHE_DIR=/ccache

USER ${username}
WORKDIR /src
ENTRYPOINT ["/bin/bash"]
