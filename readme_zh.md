# msd_lite 频道预读版

[English](readme.md) · [中文进阶运维与开发文档](docs/README_zh.md) · [下载 Release](https://github.com/Xun66/msd_lite_prefetch/releases)

这是 [rozhuk-im/msd_lite](https://github.com/rozhuk-im/msd_lite) 的频道预测
分支。它接收 UDP/RTP 组播 MPEG-TS，并通过 HTTP 转发；新增功能会学习用户的
换台路径，提前加入最可能的下一频道，以及可选的上一频道。

程序只解析 MPEG-TS、PAT/PMT 和 H.264/H.265 随机访问帧，不解码 YUV，
运行时不依赖 FFmpeg。客户端换台时会从最近一个可用 GOP 开始接收，从而减少
冷请求等待关键帧的时间。

## 下载

每个带 tag 的 Release 只包含必要的可执行文件：

- Linux x86_64
- OpenWrt x86_64、aarch64、armv7、mipsel
- macOS arm64

先运行 `uname -m`，再选择对应的 OpenWrt 文件。

## OpenWrt 快速安装

原版 XML 配置兼容。如果配置中没有 `<prefetch>`，只替换 binary 后仍保持
原版冷请求行为。

先确认实际路径：

```sh
uname -m
command -v msd_lite
ps w | grep '[m]sd_lite'
```

典型安装位置是 `/usr/bin/msd_lite`：

```sh
/etc/init.d/msd_lite stop
cp -p /usr/bin/msd_lite /usr/bin/msd_lite.before-prefetch
cp /tmp/msd_lite-openwrt-x86_64 /usr/bin/msd_lite
chmod 755 /usr/bin/msd_lite
/etc/init.d/msd_lite start
```

其他架构请替换成 `aarch64`、`armv7` 或 `mipsel` 产物。目前没有本 fork 的
opkg 软件包；固件升级或重装原版软件包可能覆盖手工替换的 binary。

OpenWrt 软件包通常会在 `/var/run/msd_lite/` 生成临时 XML。不要修改这个
文件，因为重启后会被覆盖。应修改持久模板（常见路径为
`/etc/msd_lite/msd_lite.conf.sample`），或者使用 `-c` 指定独立配置。

## 推荐预读配置

把下面内容放在根 `<msd>` 元素内部：

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

对于最高约 40 Mbit/s、GOP 约 1 秒的流，建议：

```xml
<ringBufSize>8192</ringBufSize>
```

仓库中的 [`conf/msd_lite.conf`](conf/msd_lite.conf) 已包含推荐值。请把其中的
组播接口名改成 IPTV 源实际使用的接口。

## 清除训练结果

必须先停止服务，否则内存中的模型可能把文件重新写回来：

```sh
/etc/init.d/msd_lite stop
rm /etc/msd_lite/prefetch.state
/etc/init.d/msd_lite start
```

## 编译和测试

```sh
git clone --recursive https://github.com/Xun66/msd_lite_prefetch.git
cd msd_lite_prefetch
make -j4
make test
```

训练算法、参数调优、状态格式、OpenWrt 上所有涉及的文件、持久化边界、回滚、
性能测试、源码改动地图和 CI 目标，参见[中文进阶文档](docs/README_zh.md)。

## 命令行

```text
msd_lite [-d] [-v] [-c file]
         [-p PID文件] [-u uid|用户 -g gid|用户组]
```

## 许可与原作者

BSD License。原版 msd_lite 作者：Rozhuk Ivan
<rozhuk.im@gmail.com>，2011–2026。

上游网站：<http://www.netlab.linkpc.net/wiki/en:software:msd:lite>
