# Build custom Valhalla with prefer_curvature motorcycle costing
# Based on docker/Dockerfile + docker/Dockerfile-scripted patterns

FROM ubuntu:24.04 AS builder

ENV PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
ENV LD_LIBRARY_PATH=/usr/local/lib:/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu
RUN export DEBIAN_FRONTEND=noninteractive && apt-get update && apt-get install -y sudo git

WORKDIR /src
ARG VALHALLA_BRANCH=curvature-costing
RUN git clone --recurse-submodules --shallow-submodules --depth=1 \
    -b ${VALHALLA_BRANCH} \
    https://github.com/Lazark0x/valhalla.git valhalla

WORKDIR /src/valhalla
RUN bash ./scripts/install-linux-deps.sh

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc \
      -DENABLE_TESTS=Off -DENABLE_SINGLE_FILES_WERROR=Off && \
    make -C build all -j$(nproc) && \
    make -C build install

WORKDIR /usr/local/src
RUN for f in /src/valhalla/locales/*.json; do \
      cat ${f} | python3 -c "import sys; import json; print(json.load(sys.stdin)[\"posix_locale\"])"; \
    done > valhalla_locales

# Runner image with scripts for tile building + service startup
FROM ubuntu:24.04 AS runner

ENV PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
ENV LD_LIBRARY_PATH=/usr/local/lib:/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu

RUN apt-get update > /dev/null && \
    export DEBIAN_FRONTEND=noninteractive && \
    apt-get install -y libluajit-5.1-2 libgeotiff5 \
    libzmq5 libczmq4 spatialite-bin libprotobuf-lite32 sudo locales \
    libsqlite3-0 libsqlite3-mod-spatialite libcurl4 \
    python3-minimal python3-requests python3-shapely python-is-python3 \
    curl unzip moreutils jq > /dev/null && \
    rm -rf /var/lib/apt/lists/*

COPY --from=builder /usr/local /usr/local
COPY docker/scripts/. /valhalla/scripts

ENV use_tiles_ignore_pbf=True
ENV build_tar=True
ENV serve_tiles=True
ENV update_existing_config=True
ENV force_rebuild=False
ENV build_admins=True
ENV build_time_zones=True
ENV build_elevation=False
ENV build_transit=False

WORKDIR /custom_files
EXPOSE 8002
ENTRYPOINT ["/valhalla/scripts/docker-entrypoint.sh"]
CMD ["build_tiles"]
