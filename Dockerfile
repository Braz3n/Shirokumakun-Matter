FROM alpine:3.21

# gcompat + libstdc++ provide glibc compatibility for the arm-zephyr-eabi toolchain binaries,
# which are dynamically linked against glibc.
RUN apk add --no-cache \
        gcompat \
        libstdc++ \
        python3 \
        py3-pip \
        git \
        bash \
        make \
        wget \
        tar \
        xz \
        dtc \
        gperf \
        file \
        linux-headers \
        musl-dev \
        gcc \
        unzip \
        ccache

# cmake 4.x and west 1.4 are required by NCS v3.3.0; Alpine 3.21 ships cmake 3.x.
# Python build requirements are pulled directly from the pinned NCS/Zephyr v3.3.0 sources.
# Download pre-built static GN binary from CIPD (pinned to the commit NCS v3.3.0 uses).
# Building GN from source fails on Alpine/musl due to stat64/lstat64 ABI differences vs glibc.
ARG GN_COMMIT=71305b07d708830ed7b96006dfa773a79ff313fe
RUN wget -qO /tmp/gn.zip \
        "https://chrome-infra-packages.appspot.com/dl/gn/gn/linux-amd64/+/git_revision:${GN_COMMIT}" && \
    unzip -q /tmp/gn.zip gn -d /usr/local/bin && \
    chmod +x /usr/local/bin/gn && \
    rm /tmp/gn.zip

# python_path.py shim: Matter's codegen_paths.py imports PythonPath from scripts/setup/,
# which is absent from the NCS sdk-matter-fork. Install into site-packages so it is
# importable without any PYTHONPATH manipulation.
COPY scripts/python_path_shim.py /tmp/python_path.py
RUN cp /tmp/python_path.py \
        "$(python3 -c 'import site; print(site.getsitepackages()[0])')/python_path.py" && \
    rm /tmp/python_path.py

ARG NCS_TAG=v3.3.0
ARG ZEPHYR_TAG=ncs-v3.3.0
ARG REQS_BASE=https://raw.githubusercontent.com
RUN pip3 install --break-system-packages \
        cmake==4.2.1 \
        ninja==1.11.1.1 \
        west==1.4.0 \
        lark && \
    wget -qO /tmp/zephyr-reqs-base.txt \
        "${REQS_BASE}/nrfconnect/sdk-zephyr/${ZEPHYR_TAG}/scripts/requirements-base.txt" && \
    wget -qO /tmp/nrf-reqs-base.txt \
        "${REQS_BASE}/nrfconnect/sdk-nrf/${NCS_TAG}/scripts/requirements-base.txt" && \
    wget -qO /tmp/nrf-reqs-build.txt \
        "${REQS_BASE}/nrfconnect/sdk-nrf/${NCS_TAG}/scripts/requirements-build.txt" && \
    pip3 install --break-system-packages \
        -r /tmp/zephyr-reqs-base.txt \
        -r /tmp/nrf-reqs-base.txt \
        -r /tmp/nrf-reqs-build.txt && \
    rm /tmp/*.txt

# Zephyr SDK v0.17.0 — required by NCS v3.3.0.
# The minimal tarball + setup.sh installs only the arm-zephyr-eabi cross-compiler.
ARG ZEPHYR_SDK_VER=0.17.0
RUN wget -qO /tmp/zephyr-sdk.tar.xz \
        "https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${ZEPHYR_SDK_VER}/zephyr-sdk-${ZEPHYR_SDK_VER}_linux-x86_64_minimal.tar.xz" && \
    tar xf /tmp/zephyr-sdk.tar.xz -C /opt && \
    rm /tmp/zephyr-sdk.tar.xz && \
    /opt/zephyr-sdk-${ZEPHYR_SDK_VER}/setup.sh -t arm-zephyr-eabi

ENV ZEPHYR_TOOLCHAIN_VARIANT=zephyr
ENV ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk-0.17.0
ENV PATH="/opt/zephyr-sdk-0.17.0/arm-zephyr-eabi/bin:${PATH}"

# NCS workspace and project source are mounted at runtime:
#   docker run -v ncs-v3.3.0:/ncs -v $(pwd):/workspace ...
WORKDIR /workspace
