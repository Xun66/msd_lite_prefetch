# msd_lite prefetch

[中文说明](readme_zh.md) · [Advanced operation and development](docs/README.md) · [Releases](https://github.com/Xun66/msd_lite_prefetch/releases)

This fork adds learned channel prediction and GOP-aware warmup to
[rozhuk-im/msd_lite](https://github.com/rozhuk-im/msd_lite). It receives
UDP/RTP multicast MPEG-TS and serves it over HTTP. H.264 and H.265 streams are
parsed without decoding video and without an FFmpeg runtime dependency.

When a viewer changes from channel A to B, the daemon learns the directed
transition. It can keep the most likely next channel and, optionally, the most
likely previous channel warm. A new client starts from the latest usable
PAT/PMT plus random-access GOP, reducing cold channel-change latency.

## Download

Each tagged release contains only the executables:

- Linux x86_64
- OpenWrt x86_64, aarch64, armv7, and mipsel
- macOS arm64

Choose the OpenWrt file that matches `uname -m`.

## OpenWrt quick install

Existing XML remains compatible. Without a `<prefetch>` section, the new
binary keeps the original cold-request behavior.

Find the actual program and configuration first:

```sh
uname -m
command -v msd_lite
ps w | grep '[m]sd_lite'
```

A typical installation uses `/usr/bin/msd_lite`. Back it up and replace it:

```sh
/etc/init.d/msd_lite stop
cp -p /usr/bin/msd_lite /usr/bin/msd_lite.before-prefetch
cp /tmp/msd_lite-openwrt-x86_64 /usr/bin/msd_lite
chmod 755 /usr/bin/msd_lite
/etc/init.d/msd_lite start
```

Use the matching `aarch64`, `armv7`, or `mipsel` file where appropriate.
There is no opkg package for this fork yet. A firmware upgrade or reinstall of
the upstream package may overwrite the manually replaced executable.

OpenWrt packages commonly generate XML under `/var/run/msd_lite/`. Do not edit
that generated file. Edit the persistent template (commonly
`/etc/msd_lite/msd_lite.conf.sample`) or use a dedicated configuration passed
with `-c`.

## Recommended prefetch configuration

Add this block directly under the root `<msd>` element:

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

For streams up to 40 Mbit/s with a one-second GOP, set:

```xml
<ringBufSize>8192</ringBufSize>
```

The supplied [`conf/msd_lite.conf`](conf/msd_lite.conf) already contains these
settings. Replace its multicast interface name with the interface used by your
IPTV source.

## Clear learned transitions

Stop the daemon before deleting the state. Otherwise the in-memory model may
write it back:

```sh
/etc/init.d/msd_lite stop
rm /etc/msd_lite/prefetch.state
/etc/init.d/msd_lite start
```

## Build and test

```sh
git clone --recursive https://github.com/Xun66/msd_lite_prefetch.git
cd msd_lite_prefetch
make -j4
make test
```

For training details, tuning, persistence format, every file touched on
OpenWrt, rollback instructions, benchmarks, source layout, and CI targets, see
the [advanced guide](docs/README.md).

## Usage

```text
msd_lite [-d] [-v] [-c file]
         [-p PID-file] [-u uid|user -g gid|group]
```

## License and upstream author

BSD License. Original msd_lite by Rozhuk Ivan
<rozhuk.im@gmail.com>, 2011–2026.

Upstream website:
<http://www.netlab.linkpc.net/wiki/en:software:msd:lite>
