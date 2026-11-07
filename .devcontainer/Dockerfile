# PlatformIO development image. PlatformIO downloads its own toolchains and
# frameworks per platform (espressif32, ststm32, raspberrypi, ...), so the base
# image only needs Python, git and a few host libraries.
FROM docker.io/library/debian:trixie

ARG USERNAME=dev
ARG USER_UID=1000
ARG USER_GID=1000

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        python3 python3-venv python3-pip \
        git curl ca-certificates sudo \
        build-essential \
        libusb-1.0-0 libudev1 usbutils \
        bash-completion less nano \
    && rm -rf /var/lib/apt/lists/*

# Create a non-root user matching the host UID/GID (rootless Podman maps it via
# --userns=keep-id). If the base image already has a user/group at those IDs,
# drop it first.
RUN set -eux; \
    existing_user="$(getent passwd "${USER_UID}" | cut -d: -f1 || true)"; \
    if [ -n "${existing_user}" ] && [ "${existing_user}" != "${USERNAME}" ]; then userdel -r "${existing_user}" || true; fi; \
    existing_group="$(getent group "${USER_GID}" | cut -d: -f1 || true)"; \
    if [ -n "${existing_group}" ] && [ "${existing_group}" != "${USERNAME}" ]; then groupdel "${existing_group}" || true; fi; \
    groupadd --gid "${USER_GID}" "${USERNAME}"; \
    useradd --uid "${USER_UID}" --gid "${USER_GID}" -m -s /bin/bash "${USERNAME}"; \
    usermod -aG dialout,plugdev "${USERNAME}"; \
    echo "${USERNAME} ALL=(root) NOPASSWD:ALL" > "/etc/sudoers.d/${USERNAME}"; \
    chmod 0440 "/etc/sudoers.d/${USERNAME}"; \
    # Pre-create dirs that get volume mounts so they're owned by the user.
    mkdir -p "/home/${USERNAME}/.claude" "/home/${USERNAME}/.cache" \
             "/home/${USERNAME}/.platformio" "/home/${USERNAME}/.local/bin"; \
    chown -R "${USER_UID}:${USER_GID}" "/home/${USERNAME}"

COPY --chmod=0755 install-platformio.sh /usr/local/bin/install-platformio

USER ${USERNAME}
WORKDIR /home/${USERNAME}
