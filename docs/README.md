# Advanced operation and development

[中文](README_zh.md) · [Back to the main README](../readme.md)

This guide describes prediction behavior, configuration details, persistence,
all runtime files touched by a typical OpenWrt deployment, source changes,
testing, and releases.

## How warm channel startup works

For each viewer IP, tuning from A to B increments the directed A-to-B edge and
refreshes its last-seen timestamp:

```text
current channel -> likely next channel 1, likely next channel 2
current channel <- most likely incoming edge (previous-channel prediction)
```

Reverse prediction is computed from incoming forward edges; it does not train
or persist a second graph. Per-viewer current positions remain memory-only, so
the first request after a restart is not incorrectly learned as a transition
from the last channel watched before shutdown.

The warm hub joins the multicast address following `/udp/` in the request URL.
The stream stays compressed. The built-in parser reads MPEG-TS PAT/PMT and
recognizes H.264 IDR plus H.265 BLA/IDR/CRA NAL units. It neither decodes YUV
nor launches FFmpeg. A new client starts at the latest PAT/PMT cycle before a
random-access GOP, then catches up through the existing zero-copy ring buffer.

## Prediction retention

- `observationCap` saturates edge strength. The recommended value 25 prevents
  an old schedule from accumulating an effectively unbeatable count.
- Equal counts prefer the most recently observed transition.
- Expiry grows in `entryTTL` steps according to `floor(log2(count))`, capped
  by `maxEntryTTL`.
- Every source channel has its own `protectedPerChannel` Top-N. These edges do
  not expire while they remain in that channel's Top-N.
- A new path can enter the protected set through repeated recent use.
- `maxChannels` and `maxEntries` bound memory use. Full-table eviction skips
  protected edges and selects a weak/stale non-protected edge.

Counts and last-seen timestamps update in memory immediately. State is written
only when dirty, at most once every ten minutes, and during clean shutdown.

If no real HTTP stream client remains for `idleTimeout` seconds, every
prediction-only warm hub is released. Viewer history stays available for the
next active session. Set `idleTimeout` to zero to disable this behavior.

## Configuration reference

| Setting | Meaning |
| --- | --- |
| `fEnable` | Enable prediction and warm hubs. |
| `fPrevious` | Enable reverse/previous prediction. |
| `nextCount` | Global maximum forward prediction hubs, 0–2. |
| `previousCount` | Global maximum reverse prediction hubs, 0–1. |
| `minObservations` | Minimum edge count before it can predict. |
| `maxChannels` | Maximum indexed channel URLs. |
| `maxEntries` | Maximum directed transition edges. |
| `entryTTL` | Base non-protected expiry in seconds. |
| `maxEntryTTL` | Upper bound for count-extended expiry. |
| `protectedPerChannel` | Permanent Top-N outgoing edges per source channel. |
| `observationCap` | Saturating edge count; zero means unlimited. |
| `idleTimeout` | Seconds without real clients before warm hubs stop. |
| `stateFile` | Persistent learned graph location. |

`nextCount` and `previousCount` are global limits, not per client. Channels
with real clients are not counted as prediction-only hubs.

For a one-second GOP at 40 Mbit/s, an 8 MiB `ringBufSize` leaves practical room
for PAT/PMT and one complete GOP. Increase it if the source GOP or peak bitrate
is larger.

## Persistence format

`stateFile` is a compact internal binary format, not XML or JSON. No additional
parser library is required. The header contains:

- magic bytes `MSDP`;
- format version 1;
- bounded channel and edge counts.

Malformed, incompatible, or out-of-range state is rejected rather than
partially loaded. The file contains the channel index and forward edges with
counts and timestamps. It does not contain a reverse graph or per-viewer
current positions.

To clear it safely:

```sh
/etc/init.d/msd_lite stop
rm /etc/msd_lite/prefetch.state
/etc/init.d/msd_lite start
```

## OpenWrt file inventory

Exact paths depend on the package and init script. The following list covers a
typical ImmortalWrt/Kwrt-style deployment and every file touched by the
documented manual procedure.

### Persistent overlay files

| Path | Change and ownership |
| --- | --- |
| `/usr/bin/msd_lite` | Executable replaced by the selected Release binary. |
| `/usr/bin/msd_lite.before-prefetch` | User-created rollback copy of the original executable. |
| `/etc/config/msd_lite` | Existing UCI package configuration; `ring_buffer_size` may be changed to 8192. |
| `/etc/config/msd_lite.before-prefetch` | Optional rollback copy of the UCI file. |
| `/etc/msd_lite/msd_lite.conf.sample` | Persistent XML template used by common OpenWrt init scripts. |
| `/etc/msd_lite/msd_lite.conf.sample.before-prefetch` | Optional rollback copy of the template. |
| `/etc/msd_lite/prefetch.state` | Learned graph, atomically rewritten when dirty. |

All paths above reside in overlay on a normal OpenWrt installation and survive
an ordinary reboot. A sysupgrade, package reinstall, factory reset, custom
backup policy, or read-only image layout can change that guarantee.

### Generated or temporary files

| Path | Lifecycle |
| --- | --- |
| `/var/run/msd_lite/*.conf` | Generated from UCI plus the persistent template at service start; never edit directly. |
| `/var/run/msd_lite/` | Runtime directory, normally removed/recreated by the init script. |
| `/tmp/msd_lite-openwrt-*` | Optional uploaded Release binary used only for staging. |
| `/tmp/msd_lite-prefetch*` | Optional test binary/config/PID from manual benchmarking; not required in production. |

`/var/run` and `/tmp` are tmpfs and disappear on reboot. The daemon's configured
PID file, if any, is also runtime-only.

### Service actions

The deployment stops and starts `/etc/init.d/msd_lite`. It does not need to
modify that init script. procd regenerates the runtime XML and starts the
binary using its existing UCI instance.

### Rollback

Adapt backup suffixes to the names you used:

```sh
/etc/init.d/msd_lite stop
cp /usr/bin/msd_lite.before-prefetch /usr/bin/msd_lite
chmod 755 /usr/bin/msd_lite
cp /etc/msd_lite/msd_lite.conf.sample.before-prefetch \
   /etc/msd_lite/msd_lite.conf.sample
cp /etc/config/msd_lite.before-prefetch /etc/config/msd_lite
/etc/init.d/msd_lite start
```

The learned state can be retained during binary rollback or removed while the
service is stopped.

## Verification

```sh
ps w | grep '[m]sd_lite'
netstat -lntp | grep ':7088'
grep -n 'prefetch\\|ringBufSize\\|stateFile' /var/run/msd_lite/*.conf
ls -l /etc/msd_lite/prefetch.state
```

A five-second stream smoke test from another machine:

```sh
curl --max-time 5 -o /dev/null \
  'http://ROUTER:7088/udp/239.0.0.1:1234'
```

A timeout after receiving bytes is expected because an IPTV stream has no
natural end.

## Source-tree change map

The fork's relevant tracked files are:

| Path | Purpose |
| --- | --- |
| `src/channel_predict.c`, `.h` | Bounded transition graph, retention, ranking, binary persistence. |
| `src/mpegts_gop.c`, `.h` | Lightweight PAT/PMT and H.264/H.265 random-access parser. |
| `src/msd_lite.c` | Configuration, lifecycle, idle cleanup, periodic persistence. |
| `src/stream_sys.c`, `.h` | Warm hubs and GOP-aware client startup integration. |
| `src/msd_lite_stat_text.c` | Existing server output integration/build compatibility. |
| `conf/msd_lite.conf` | Recommended configuration. |
| `tests/test_prefetch.c` | Parser, prediction, reverse derivation, and persistence tests. |
| `tests/generate_prefetch_state.c` | Offline seeded-state generator. |
| `tests/benchmark_channel_start.sh` | Repeatable cold/warm first-byte benchmark. |
| `tests/benchmark_with_resources.sh` | Latency plus server CPU/RSS collection. |
| `tests/results/` | Recorded benchmark data and notes. |
| `CMakeLists.txt`, `src/CMakeLists.txt` | Build, tests, Threads, and portable hardening checks. |
| `Makefile` | Small CMake wrapper. |
| `.github/workflows/build-release.yml` | Linux/OpenWrt/macOS validation and clean tagged releases. |
| `.gitignore` | Local build output exclusions. |
| `readme.md`, `readme_zh.md`, `docs/` | User and advanced documentation. |

The `src/liblcb` submodule remains an upstream submodule; this fork does not
carry local modifications inside it.

## Tests and benchmarks

```sh
make -j4
make test
```

Direct CMake:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

The test suite covers base library tests plus PAT/PMT, H.264/H.265 GOP
detection, forward prediction, reverse derivation, and state persistence.

Benchmark scripts accept the server/channel parameters documented by their
`--help` output. Resource sampling reports server CPU and RSS while repeated
requests measure first-byte latency.

## Release workflow

Pushes and pull requests build and test:

- Linux x86_64;
- static musl OpenWrt x86_64, aarch64, armv7, and mipsel;
- macOS arm64.

A `v*` tag creates a GitHub Release containing only six named binaries. It
does not publish blockmaps, updater YAML, installers, or build directories.
Native Windows is not currently supported because the daemon relies on POSIX
process, socket, pthread, syslog, and queue APIs.
