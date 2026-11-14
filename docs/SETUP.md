# Setup

## Packages (Ubuntu / Debian)

```bash
# compilers, debugger, memory checker
sudo apt install build-essential gdb valgrind strace ltrace graphviz

# GStreamer: dev headers + runtime plugins + tools
sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
     gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
     gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav

# optional: RTSP server library (gstreamer/11-network-streaming)
sudo apt install libgstrtspserver-1.0-dev
```

Check:

```bash
gcc --version && g++ --version
pkg-config --modversion gstreamer-1.0
gst-inspect-1.0 videotestsrc
```

## Building

| Command | What it does |
|---|---|
| `make` | build all DSA programs (debug, `-O0 -g3`, ASan + UBSan) |
| `make gst` | build all GStreamer programs |
| `make test` / `make test-gst` | build and run everything; prints pass/fail |
| `make run P=<file.c>` | build and run one program |
| `make SAN=0` | no sanitizers, which you need for **valgrind** and sometimes **gdb** |
| `make OPT=1 SAN=0` | optimised build for benchmarks |
| `make list` | list every program |
| `make clean` | delete `build/` |

Binaries go to `build/<same path as the source, without extension>`.

## Debugging any program

```bash
make SAN=0 build/dsa/06-linked-lists/c/01_singly_linked_list
gdb -q build/dsa/06-linked-lists/c/01_singly_linked_list
valgrind --leak-check=full build/dsa/06-linked-lists/c/01_singly_linked_list
```

See [dsa/03-debugging](../dsa/03-debugging/README.md) for the full guide.

## Editor

VS Code with the C/C++ extension works well. Point IntelliSense at
`/usr/include/gstreamer-1.0` and `/usr/include/glib-2.0`, or generate
`compile_commands.json` with `bear -- make gst`.

## Cross-compiling for the BeagleBone AI-64 (aarch64)

```bash
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
make CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++ SAN=0
scp build/dsa/... debian@<board-ip>:~
```

The GStreamer programs need the board's GStreamer libraries to link against
(a sysroot). See [gstreamer/14-embedded-tda4vm](../gstreamer/14-embedded-tda4vm/README.md).
