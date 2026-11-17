# RtTC for a Linux radio host: RadioConnector and RadioConnectorCli, with direwolf and
# Hamlib built from source (the same versions and checksums as the CI build).
# Build from the repository root:
#   docker build -f docker/Dockerfile -t rttc .
# Run it with docker/compose.radio.yaml, which passes the sound card and serial ports in.

ARG DEBIAN=trixie

FROM debian:${DEBIAN}-slim AS build

ARG HAMLIB_VERSION=4.7.2
ARG HAMLIB_SHA256=ae1fcf2dbc80ea0786ea8f047b09399c3f7737d1930442f61a031708ed33e88f
ARG DIREWOLF_VERSION=1.8.1
ARG DIREWOLF_SHA256=89d5f7992ae1e74d8cf26ec6479dde74d1f480bde950043756e875a689d065d7

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ninja-build pkg-config ca-certificates curl python3-venv \
        libasound2-dev libusb-1.0-0-dev libudev-dev \
    && rm -rf /var/lib/apt/lists/*

# CMakeLists.txt needs CMake 4.1 or newer, newer than Debian's
RUN python3 -m venv /opt/cmake && /opt/cmake/bin/pip install --no-cache-dir "cmake>=4.1"
ENV PATH=/opt/cmake/bin:$PATH

WORKDIR /src

# Hamlib (shared, into /usr/local): RtTC's CAT control, and direwolf's PTT over CAT
RUN curl -fsSL --retry 5 --retry-all-errors --retry-delay 10 -o hamlib.tar.gz \
        "https://github.com/Hamlib/Hamlib/releases/download/${HAMLIB_VERSION}/hamlib-${HAMLIB_VERSION}.tar.gz" \
    && echo "${HAMLIB_SHA256}  hamlib.tar.gz" | sha256sum -c \
    && mkdir hamlib && tar xzf hamlib.tar.gz -C hamlib --strip-components=1 \
    && cd hamlib \
    && ./configure --prefix=/usr/local --without-cxx-binding --without-readline \
    && make -j"$(nproc)" && make install && ldconfig

# direwolf, built against that Hamlib
RUN curl -fsSL --retry 5 --retry-all-errors --retry-delay 10 -o direwolf.tar.gz \
        "https://github.com/wb2osz/direwolf/archive/refs/tags/${DIREWOLF_VERSION}.tar.gz" \
    && echo "${DIREWOLF_SHA256}  direwolf.tar.gz" | sha256sum -c \
    && mkdir direwolf && tar xzf direwolf.tar.gz -C direwolf --strip-components=1 \
    && cmake -S direwolf -B direwolf-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local \
    && cmake --build direwolf-build \
    && cmake --install direwolf-build

# RtTC. Bluetooth audio is left out (not used on radio hosts); RadioTests runs in CI
COPY . /src/rttc
RUN cmake -S rttc -B rttc-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DRTC_TESTS=OFF -DRTC_BLUETOOTH=OFF \
    && cmake --build rttc-build --target RadioConnector RadioConnectorCli \
    && install -m 755 rttc-build/RadioConnector rttc-build/RadioConnectorCli /usr/local/bin/


FROM debian:${DEBIAN}-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        libasound2t64 libusb-1.0-0 libudev1 alsa-utils tini \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /usr/local/bin/ /usr/local/bin/
COPY --from=build /usr/local/lib/ /usr/local/lib/
RUN ldconfig

# Settings live in the /data volume: /data/rttc/connector.ini
RUN useradd --uid 1000 --home-dir /data --create-home rttc
ENV HOME=/data XDG_CONFIG_HOME=/data
COPY docker/entrypoint.sh /entrypoint.sh
RUN chmod 755 /entrypoint.sh
USER rttc
WORKDIR /data
VOLUME /data

ENTRYPOINT ["/usr/bin/tini", "--", "/entrypoint.sh"]
CMD []
