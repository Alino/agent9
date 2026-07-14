# llvm9 host-generate builder — linux/amd64 so cmake/tablegen emit x86_64 defines
# and generated headers that match cc9's x86_64-unknown-none target. This image
# ONLY configures LLVM + builds LLVMSupport/LLVMMC natively (g++) to (a) generate
# the tablegen/config headers and (b) emit compile_commands.json for the harvest.
# It does NOT target 9front — cc9 does that on the host from the harvested set.
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
      cmake ninja-build python3 g++ \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
