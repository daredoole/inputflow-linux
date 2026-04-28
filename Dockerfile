FROM ubuntu:24.04

# Install all necessary tooling and dependencies for building the custom C++17 client
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libssl-dev \
    libevdev-dev \
    zlib1g-dev \
    pkg-config \
    git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . /app

# Compile project
RUN rm -rf build && cmake -S . -B build && cmake --build build -j"$(nproc)"

# Example Docker run:
#   docker run --rm -it --network host --device /dev/uinput:/dev/uinput \
#     -e MWB_SCREEN_WIDTH=2560 -e MWB_SCREEN_HEIGHT=1600 \
#     mwb-linux <WINDOWS_IP> <SECURITY_KEY>
# Example Podman run on Fedora:
#   podman run --rm -it --network host --device /dev/uinput:/dev/uinput \
#     --security-opt label=disable --group-add keep-groups \
#     -e MWB_SCREEN_WIDTH=2560 -e MWB_SCREEN_HEIGHT=1600 \
#     localhost/mwb-linux <WINDOWS_IP> <SECURITY_KEY>
ENTRYPOINT ["/app/build/mwb_client"]
