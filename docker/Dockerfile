# PS Vita SDK build environment for J2ME port
# Based on Ubuntu 24.04 (GLIBC 2.39 >= 2.38 required by vitasdk toolchain)
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential cmake ninja-build git curl wget python3 \
    libarchive-tools unzip zip \
    pkg-config autoconf automake libtool \
    && rm -rf /var/lib/apt/lists/*

# Install vitasdk toolchain (matches host GLIBC inside container)
ENV VITASDK=/usr/local/vitasdk
ENV PATH=$VITASDK/bin:$PATH

RUN mkdir -p $VITASDK && cd /tmp && \
    wget -qO- https://github.com/vitasdk/vita-headers/raw/master/.travis.d/last_built_toolchain.py | python3 - master linux > toolchain_url.txt && \
    echo "Toolchain URL: $(cat toolchain_url.txt)" && \
    wget -O- "$(cat toolchain_url.txt)" | tar xj -C $VITASDK --strip-components=1 && \
    rm -f toolchain_url.txt

# Verify toolchain works
RUN arm-vita-eabi-gcc --version

WORKDIR /workspace

CMD ["/bin/bash"]
