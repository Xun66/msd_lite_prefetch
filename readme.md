# msd_lite

[![Build-macOS-latest Actions Status](https://github.com/rozhuk-im/msd_lite/workflows/build-macos-latest/badge.svg)](https://github.com/rozhuk-im/msd_lite/actions)
[![Build-Ubuntu-latest Actions Status](https://github.com/rozhuk-im/msd_lite/workflows/build-ubuntu-latest/badge.svg)](https://github.com/rozhuk-im/msd_lite/actions)


Rozhuk Ivan <rozhuk.im@gmail.com> 2011-2026

msd_lite - Multi stream daemon lite.
This lightweight version of Multi Stream daemon (msd)
Program for organizing IP TV streaming on the network via HTTP.


## Licence
BSD licence.
Website: http://www.netlab.linkpc.net/wiki/en:software:msd:lite


## Donate
Support the author
* **GitHub Sponsors:** [!["GitHub Sponsors"](https://camo.githubusercontent.com/220b7d46014daa72a2ab6b0fcf4b8bf5c4be7289ad4b02f355d5aa8407eb952c/68747470733a2f2f696d672e736869656c64732e696f2f62616467652f2d53706f6e736f722d6661666266633f6c6f676f3d47697448756225323053706f6e736f7273)](https://github.com/sponsors/rozhuk-im) <br/>
* **Buy Me A Coffee:** [!["Buy Me A Coffee"](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://www.buymeacoffee.com/rojuc) <br/>
* **PayPal:** [![PayPal](https://srv-cdn.himpfen.io/badges/paypal/paypal-flat.svg)](https://paypal.me/rojuc) <br/>
* **Bitcoin (BTC):** `1AxYyMWek5vhoWWRTWKQpWUqKxyfLarCuz` <br/>


## Features
* Open source
* BSD License
* No deadlocks threads during operation
* Receiving only udp-multicast, including rtp streams
* Not available options URL: precache and blocksize
* Zero Copy on Send (ZCoS) is always on
* No polling to send out to clients fUsePollingForSend
* Lightweight MPEG2-TS/GOP analyzer for smart startup of new clients


## Channel prediction and GOP warmup

This fork can keep likely channel-change targets warm.  It learns transitions
per viewer IP address and maintains a bounded channel index plus directed
transition entries:

```
current channel -> most likely next channel 1, next channel 2
current channel -> most likely previous channel
```

Counts and last-seen times are held in memory. Stale transition and viewer
entries are removed after `entryTTL`; table sizes are bounded by
`maxChannels` and `maxEntries`. Channels and transition edges are atomically
saved to `stateFile` every ten minutes when changed and once during a clean
shutdown, then restored after a daemon restart. Per-viewer
"currently tuned channel" data is deliberately not persisted, so a new TV
session is not treated as a transition from the channel watched before
shutdown.

`stateFile` is an internal compact binary format. Its header contains the
`MSDP` magic, a format version, and bounded channel/edge counts; incompatible
or malformed files are rejected rather than partially loaded.

Warm hubs join the multicast source using the address after `/udp/` in the
request URL.  The stream remains compressed: a small built-in parser reads
MPEG-TS PAT/PMT tables and recognizes H.264 IDR and H.265 BLA/IDR/CRA NAL
units.  It does not decode YUV frames and it does not require FFmpeg.  A new
HTTP client starts at the most recent PAT/PMT cycle preceding a random-access
GOP, then catches up to the live write position through the existing
zero-copy ring buffer.

Example:

```xml
<prefetch>
	<fEnable>yes</fEnable>
	<fPrevious>yes</fPrevious>
	<nextCount>1</nextCount>
	<previousCount>1</previousCount>
	<minObservations>2</minObservations>
	<maxChannels>512</maxChannels>
	<maxEntries>4096</maxEntries>
	<entryTTL>604800</entryTTL>
	<maxEntryTTL>2592000</maxEntryTTL>
	<protectedPerChannel>4</protectedPerChannel>
	<observationCap>25</observationCap>
	<idleTimeout>60</idleTimeout>
	<stateFile>/etc/msd_lite/prefetch.state</stateFile>
</prefetch>
```

`nextCount` accepts 0..2. `previousCount` accepts 0..1. With both set to one,
at most one predicted forward hub and one reverse-predicted hub are retained
in addition to channels that have real clients. Set `fEnable` to `no` for
the original cold-request lifecycle, or `fPrevious` to `no` to disable the
reverse model. Transition expiry grows in `entryTTL` steps based on
`floor(log2(count))`, up to `maxEntryTTL`. Each source channel independently
keeps its strongest `protectedPerChannel` outgoing transitions forever; ties
prefer the most recently observed transition. Set `protectedPerChannel` to
zero to disable permanent retention. `observationCap` is a saturating counter;
after competing paths reach the cap, the most recently observed path wins
ties. Set it to zero for unlimited historical counts. For 40 Mbit/s streams with a one-second
GOP, use at least an
8 MiB `ringBufSize`; the supplied configuration does so. If no real HTTP
stream client remains for `idleTimeout` seconds, all predicted warm hubs are
released. A value of zero disables this idle timeout.

### Training behavior

For each viewer IP, tuning from A to B increments the directed A-to-B edge
and refreshes its last-seen time. Reverse prediction does not train a second
graph: the previous channel for B is selected from incoming edges ending at
B. Edge counts saturate at `observationCap`, avoiding old schedules becoming
impossible to replace. For equal counts, the most recently observed edge
wins.

Every source channel has its own permanent Top-N set, controlled by
`protectedPerChannel`. These edges do not expire while they remain in that
channel's Top-N. Other edges receive a count-dependent TTL, capped by
`maxEntryTTL`, and can challenge the protected set through repeated use.

To clear learned state, stop the daemon first, remove the configured
`stateFile`, and restart it. Removing the file while the daemon is running is
not sufficient because the in-memory graph can write it again:

```sh
/etc/init.d/msd_lite stop
rm /etc/msd_lite/prefetch.state
/etc/init.d/msd_lite start
```

Adjust the path if `stateFile` points elsewhere. Per-viewer current-channel
positions are intentionally memory-only and are cleared by every restart.




## Compilation and Installation

The top-level Makefile is a small CMake wrapper:

```sh
make -j4
make test
```

Direct CMake builds remain supported:

```
sudo apt-get install build-essential git cmake fakeroot
git clone --recursive https://github.com/rozhuk-im/msd_lite.git
cd msd_lite
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_VERBOSE_MAKEFILE=true ..
make -j 8
```


## Run tests
```
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=1 ..
cmake --build . --config Release -j 16
ctest -C Release --output-on-failure -j 16
```

## Release binaries

GitHub Actions builds:

* Linux x86_64
* OpenWrt-compatible static musl binaries for x86_64, aarch64, armv7, and
  mipsel
* macOS arm64

Pushes and pull requests only validate builds. Pushing a tag such as `v1.11.0`
creates a GitHub Release containing only the named executables. The workflow
does not publish blockmaps, updater YAML, installers, or build directories.

Native Windows x64 is not currently published. The daemon depends on POSIX
process, socket, pthread, syslog, and queue APIs; a real Windows build requires
a maintained Win32 compatibility layer rather than merely cross-compiling the
current source.


## Usage
```
msd_lite [-d] [-v] [-c file]
       [-p PID file] [-u uid|usr -g gid|grp]
 -h           usage (this screen)
 -d           become daemon
 -c file      config file
 -p PID file  file name to store PID
 -u uid|user  change uid
 -g gid|group change gid
 -v           verboce
```


## Setup

### msd_lite
Copy %%ETCDIR%%/msd_lite.conf.sample to %%ETCDIR%%/msd_lite.conf
then replace lan0 with your network interface name.
Add more sections if needed.
Remove IPv4/IPv6 lines if not needed.

Add to /etc/rc.conf:
```
msd_lite_enable="YES"
```

Run:
```
service msd_lite restart
```
