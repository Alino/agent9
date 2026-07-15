# iris build-gen container: Mesa's meson + the iris deps. linux/amd64 so arch
# defines match cc9's x86_64 target. libdrm-dev is here ONLY to satisfy meson's
# configure check — the 9front build never calls libdrm (iris's kernel contract
# is a plain POSIX ioctl(), implemented in cc9's runtime in-process).
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
      meson ninja-build pkg-config \
      python3 python3-mako python3-setuptools \
      flex bison gcc g++ \
      zlib1g-dev libexpat1-dev libdrm-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
