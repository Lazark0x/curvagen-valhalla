# Build custom Valhalla with prefer_curvature motorcycle costing
# Multi-stage: clone fork from GitHub, build, overlay onto official image

FROM ghcr.io/valhalla/valhalla:latest AS base

FROM ubuntu:24.04 AS builder

ENV PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
ENV LD_LIBRARY_PATH=/usr/local/lib:/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu
RUN export DEBIAN_FRONTEND=noninteractive && apt-get update && apt-get install -y sudo git

WORKDIR /src
ARG VALHALLA_BRANCH=curvature-costing
RUN git clone --recurse-submodules --shallow-submodules --depth=1 \
    -b ${VALHALLA_BRANCH} \
    https://github.com/AndreyLazarko/curvagen-valhalla.git valhalla

WORKDIR /src/valhalla
RUN bash ./scripts/install-linux-deps.sh

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc \
      -DENABLE_TESTS=Off -DENABLE_SINGLE_FILES_WERROR=Off && \
    make -C build all -j$(nproc) && \
    make -C build install

# Final image: official scripted runner with our custom binaries + library
FROM base AS runner
COPY --from=builder /usr/local/bin/valhalla_* /usr/local/bin/
COPY --from=builder /usr/local/lib/libvalhalla.* /usr/local/lib/
