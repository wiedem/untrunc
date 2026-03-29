# pull base image
FROM ubuntu:24.04 AS build
LABEL stage=intermediate
ARG FF_VER=8.1

# install packaged dependencies
RUN apt-get update && [ "$FF_VER" = 'shared' ] && \
	apt-get -y install --no-install-recommends libavformat-dev libavcodec-dev libavutil-dev g++ make git || \
	apt-get -y install --no-install-recommends nasm wget g++ make git ca-certificates xz-utils zlib1g-dev && \
	rm -rf /var/lib/apt/lists/*

# copy code
ADD . /untrunc-src
WORKDIR /untrunc-src

# build untrunc: IS_STATIC=1 is only active for non-shared builds
RUN /usr/bin/make FF_VER=$FF_VER IS_STATIC=1 && strip untrunc


# deliver clean image
FROM ubuntu:24.04
ARG FF_VER=8.1

# For shared builds: install runtime FFmpeg libs (Ubuntu 24.04 ships FFmpeg 6.x).
# For source builds (default): the binary is statically linked, no libs needed.
RUN apt-get update && [ "$FF_VER" = 'shared' ] && \
	apt-get -y install --no-install-recommends libavformat60 libavcodec60 libavutil58 && \
	rm -rf /var/lib/apt/lists/* || true
COPY --from=build /untrunc-src/untrunc /bin/untrunc

# non-root user
RUN useradd untrunc
USER untrunc

# execution
ENTRYPOINT ["/bin/untrunc"]
