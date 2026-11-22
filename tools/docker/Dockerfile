# Local Linux toolchains for Ulog: GCC 12 / libstdc++ and Clang 16 / libc++.
# Complements CI (Ubuntu 24.04 GCC and Clang with libstdc++) with an older
# GCC and with libc++, and gives Windows hosts a Linux build.
#
# Everything goes through stdin, so a remote daemon works too:
#   docker build -t ulog-linux - < tools/docker/Dockerfile
#   docker volume create ulog-conan
#   git ls-files -co --exclude-standard | tar c -T - | docker run -i --rm \
#       -v ulog-conan:/root/.conan2/p ulog-linux \
#       sh -c 'tar x && tools/docker/conan-create.sh gcc12'
# The volume keeps built Conan dependencies across runs.
FROM debian:bookworm-slim

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        g++-12 clang-16 libc++-16-dev libc++abi-16-dev libclang-rt-16-dev llvm-16 \
        cmake ninja-build make python3-pip git ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && pip3 install --break-system-packages "conan>=2.8,<3"

WORKDIR /src
