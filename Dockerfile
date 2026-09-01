# syntax=docker/dockerfile:1
# pinned: codegen drift between gcc:15 refreshes makes the binary ladder unreadable (R70-prep).
# The round protocol compares the candidate binary against the image running in production,
# symbol size by symbol size. Docker Hub republishes the `gcc:15` tag every few weeks, and a
# rebuilt toolchain can shift code generation across the whole 3.9 MB .text — the ladder would
# then light up everywhere and the cause would not be our code. Bump this digest deliberately,
# in its own commit, and re-run the ladder against the previous round when you do.
FROM gcc:15@sha256:486d5379b2d679f18bca957901d628952aa0ebc2d82817047bbef1dbfbc13d70 AS build

# Setup the build environment
RUN apt-get update -y
RUN apt-get install git cmake libboost-dev libicu-dev libpq-dev -y --no-install-recommends

ARG SERVER_BUILD_TYPE=RelWithDebInfo

# Build the server
WORKDIR /build/alicia-server

# Prepare the source
COPY . .

# Use
ENV PATH="/usr/local/bin:${PATH}"

RUN git init
RUN git submodule update --init --recursive

RUN cmake -DCMAKE_BUILD_TYPE=${SERVER_BUILD_TYPE} -DBUILD_TESTS=False . -B ./build
RUN cmake --build ./build --parallel 8

# Install the binary
RUN cmake --install ./build --prefix /usr/local

# Copy the resources
RUN mkdir /var/lib/alicia-server/
RUN cp -r ./resources/* /var/lib/alicia-server/

# pinned: codegen drift between gcc:15 refreshes makes the binary ladder unreadable (R70-prep).
# The round protocol compares the candidate binary against the image running in production,
# symbol size by symbol size. Docker Hub republishes the `gcc:15` tag every few weeks, and a
# rebuilt toolchain can shift code generation across the whole 3.9 MB .text — the ladder would
# then light up everywhere and the cause would not be our code. Bump this digest deliberately,
# in its own commit, and re-run the ladder against the previous round when you do.
FROM gcc:15@sha256:486d5379b2d679f18bca957901d628952aa0ebc2d82817047bbef1dbfbc13d70

LABEL author="Legacy of Alicia contributors" maintainer="legacy-of-alicia"
LABEL org.opencontainers.image.source="https://github.com/legacy-of-alicia/legacy-of-alicia"
LABEL org.opencontainers.image.description="Dedicated server implementation for the Alicia game series"

# Setup the runtime environent
RUN apt-get update -y
RUN apt-get install libicu76 libpq5 libstdc++6 -y --no-install-recommends

WORKDIR /opt/alicia-server

COPY --from=build /usr/local/bin/alicia-server /usr/local/bin/alicia-server
COPY --from=build /var/lib/alicia-server/ /var/lib/alicia-server/

ENTRYPOINT ["/usr/local/bin/alicia-server", "/var/lib/alicia-server"]