FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        python3 \
        python3-pip \
        python3-venv \
        git \
        make \
        wget \
        curl \
        tar \
        xz-utils \
        device-tree-compiler \
        gperf \
        file \
        linux-libc-dev \
        gcc \
        g++ \
        unzip \
        ccache \
        ninja-build \
        libffi-dev \
        libssl-dev \
        && rm -rf /var/lib/apt/lists/*

# cmake 4.x is required by NCS v3.3.0; Debian bookworm ships 3.x.
# Python build requirements are pulled directly from the pinned NCS/Zephyr v3.3.0 sources.
# Download pre-built static GN binary from CIPD (pinned to the commit NCS v3.3.0 uses).
ARG GN_COMMIT=71305b07d708830ed7b96006dfa773a79ff313fe
RUN wget -qO /tmp/gn.zip \
        "https://chrome-infra-packages.appspot.com/dl/gn/gn/linux-amd64/+/git_revision:${GN_COMMIT}" && \
    unzip -q /tmp/gn.zip gn -d /usr/local/bin && \
    chmod +x /usr/local/bin/gn && \
    rm /tmp/gn.zip

# ZAP CLI for cluster/endpoint generation. Version pinned to match
# the SDK's scripts/setup/zap.json expectation (MIN_ZAP_VERSION 2025.10.23).
ARG ZAP_VERSION=v2025.10.23-nightly.2
RUN mkdir -p /opt/zap && \
    wget -qO /tmp/zap.zip \
        "https://chrome-infra-packages.appspot.com/dl/experimental/matter/zap/linux-amd64/+/version:${ZAP_VERSION}" && \
    unzip -q /tmp/zap.zip zap-cli -d /opt/zap && \
    chmod +x /opt/zap/zap-cli && \
    rm /tmp/zap.zip

ENV ZAP_INSTALL_PATH=/opt/zap

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
