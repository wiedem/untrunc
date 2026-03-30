Untrunc
=======

Fork of [anthwlock/untrunc](https://github.com/anthwlock/untrunc), which is itself a fork of the
original [untrunc](https://github.com/ponchio/untrunc) by Federico Ponchio.

Restore a damaged (truncated) mp4, m4v, mov, 3gp video. Provided you have a similar not broken video. And some luck.

You need:

* Another video file which isn't broken

## Features

* Low memory usage
* Support for files larger than 2 GB
* Ability to skip over unknown bytes
* Generic support for all tracks with fixed-width chunks (e.g. twos/sowt)
* Configurable log levels
* Can stretch video to match audio duration (beta, `-sv`)
* Handles invalid atom lengths
* GoPro support and Sony RSV (recording-in-progress) file recovery

## Building

Untrunc requires FFmpeg 8.1 or later.

#### With system libraries

```shell
sudo apt-get install libavformat-dev libavcodec-dev libavutil-dev
# get the source code
git clone https://github.com/wiedem/untrunc && cd untrunc
make
sudo cp untrunc /usr/local/bin
```

#### With local libraries

Just use following commands, make will do the rest for you.

```shell
sudo apt-get install nasm wget
git clone https://github.com/wiedem/untrunc && cd untrunc
make FF_VER=8.1
sudo cp untrunc /usr/local/bin
```

#### macOS with Homebrew

```shell
brew install ffmpeg nasm
export PKG_CONFIG_PATH="/opt/homebrew/lib/pkgconfig"
CPPFLAGS="-I/opt/homebrew/include" LDFLAGS="-L/opt/homebrew/lib" make
```

## Docker container

You can use the included Dockerfile to build and execute the package as a container.\
The optional argument 'FF_VER' will be passed to `make`.

```shell
# docker build --build-arg FF_VER=8.1 -t untrunc .
docker build -t untrunc .
docker image prune --filter label=stage=intermediate -f

docker run --rm -v ~/Videos/:/mnt untrunc /mnt/ok.mp4 /mnt/broken.mp4
```

## Using

You need both the broken video and an example working video (ideally from the same camera, if not the chances to fix it are slim).

Run this command in the folder where you have compiled Untrunc but replace the `/path/to/...` bits with your 2 video files:

```shell
./untrunc /path/to/working-video.m4v /path/to/broken-video.m4v
```

Then it should churn away and hopefully produce a playable file called `broken-video_fixed.m4v`.

That's it you're done!

### Help/Support

#### Reporting issues
Use the `-v` parameter for a more detailed output. Both the healthy and corrupt file might be needed to help you.
