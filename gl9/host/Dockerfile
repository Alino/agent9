# gl9 Mesa host-generate/oracle builder.
# This image only runs meson+ninja to (a) materialize Mesa's generated C sources,
# (b) emit compile_commands.json (the bridge harvest.py reads), and (c) build a
# native softpipe+OSMesa we reuse as the parity golden oracle. It does NOT target
# 9front — cc9 does that on the host from the harvested file set.
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
      meson ninja-build pkg-config \
      python3 python3-mako python3-setuptools \
      flex bison \
      zlib1g-dev libexpat1-dev \
      gcc g++ \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
