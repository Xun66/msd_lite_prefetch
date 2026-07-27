# 进阶运维与开发文档

[English](README.md) · [返回中文首页](../readme_zh.md)

本文介绍预测算法、参数、持久化格式、OpenWrt 部署会涉及的全部文件、回滚、
源码改动、测试和发布流程。

## 预读如何工作

程序按客户端 IP 记录换台。用户从 A 切到 B 时，A→B 有向边的观测次数加一，
并刷新最后观测时间：

```text
当前频道 -> 最可能的下一频道 1、下一频道 2
当前频道 <- 指向它的最强入边（上一频道预测）
```

反向预测直接从正向边的入边计算，不训练、不保存第二份反向图。每个客户端
“当前正在看的频道”只保存在内存，因此程序重启后的第一次请求不会被错误地
当成从关机前频道发生的换台。

预读 hub 根据请求 URL 中 `/udp/` 后面的地址加入组播。码流始终保持压缩状态。
内置解析器读取 MPEG-TS PAT/PMT，识别 H.264 IDR 和 H.265 BLA/IDR/CRA，
不解码 YUV，也不启动 FFmpeg。新客户端从随机访问 GOP 前最近的 PAT/PMT
开始，再通过原有零拷贝 ring buffer 追上直播位置。

## 路径保留与淘汰

- `observationCap` 限制观测强度，推荐 25，避免旧节目单积累无法挑战的高分。
- 分数相同时，最近一次观测的路径优先。
- 普通路径有效期按 `floor(log2(count))` 延长，每级增加一个 `entryTTL`，
  但不超过 `maxEntryTTL`。
- 每个源频道各自拥有 `protectedPerChannel` 个永久保护位，不是所有频道共享。
- 新路径可以通过近期反复使用进入保护区，并替换原有路径。
- `maxChannels` 和 `maxEntries` 限制内存。表满时跳过保护路径，淘汰较弱且较旧
  的普通路径。

每次换台会立即更新内存；只有状态发生变化时才写盘，最多每 10 分钟一次，
正常退出时也会保存。

如果连续 `idleTimeout` 秒没有真实 HTTP 流客户端，全部纯预测 hub 都会停止，
但已学路径仍保留，等待下次活跃会话。设为 0 可关闭空闲停止机制。

## 参数说明

| 参数 | 含义 |
| --- | --- |
| `fEnable` | 启用预测与预读。 |
| `fPrevious` | 启用上一频道预测。 |
| `nextCount` | 全局下向预测上限，0–2。 |
| `previousCount` | 全局上向预测上限，0–1。 |
| `minObservations` | 路径参与预测所需的最低观测次数。 |
| `maxChannels` | 频道 URL 索引上限。 |
| `maxEntries` | 有向路径总数上限。 |
| `entryTTL` | 非保护路径的基础有效期，单位秒。 |
| `maxEntryTTL` | 按强度延长后的最大有效期。 |
| `protectedPerChannel` | 每个源频道永久保护的 Top-N 出边。 |
| `observationCap` | 观测次数饱和值；0 表示不限制。 |
| `idleTimeout` | 无真实客户端多少秒后停止预测流。 |
| `stateFile` | 学习状态持久化路径。 |

`nextCount` 和 `previousCount` 是全局限制，不会按每个客户端分别放大。已有真实
客户端的频道不算纯预测 hub。

对于 GOP 约 1 秒、峰值 40 Mbit/s 的流，推荐至少 8 MiB `ringBufSize`，
以容纳 PAT/PMT 和完整 GOP。GOP 更长或峰值更高时应继续增大。

## 状态文件格式

`stateFile` 使用紧凑的内部二进制格式，不是 XML 或 JSON，因此无需增加解析库。
文件头包含：

- magic bytes：`MSDP`；
- 格式版本：1；
- 有上限校验的频道数和路径数。

格式错误、版本不兼容或数量越界时会拒绝整个文件，不会加载一半。文件只保存
频道索引、正向路径、次数和时间戳；不保存反向图，也不保存每个客户端当前频道。

安全清除方法：

```sh
/etc/init.d/msd_lite stop
rm /etc/msd_lite/prefetch.state
/etc/init.d/msd_lite start
```

## OpenWrt 文件完整清单

准确路径取决于固件的软件包和 init 脚本。下面覆盖典型
ImmortalWrt/Kwrt 安装，以及本文手工部署步骤会触碰的全部文件。

### 可持久化 overlay 文件

| 路径 | 变化与用途 |
| --- | --- |
| `/usr/bin/msd_lite` | 替换为对应架构的 Release binary。 |
| `/usr/bin/msd_lite.before-prefetch` | 用户创建的原 binary 回滚备份。 |
| `/etc/config/msd_lite` | 原 UCI 配置；可把 `ring_buffer_size` 改为 8192。 |
| `/etc/config/msd_lite.before-prefetch` | 可选的 UCI 回滚备份。 |
| `/etc/msd_lite/msd_lite.conf.sample` | 常见 OpenWrt init 脚本使用的持久 XML 模板。 |
| `/etc/msd_lite/msd_lite.conf.sample.before-prefetch` | 可选的模板回滚备份。 |
| `/etc/msd_lite/prefetch.state` | 学习图；有变化时原子替换写入。 |

正常 OpenWrt 上这些路径位于 overlay，普通重启不会丢失。sysupgrade、重装软件包、
恢复出厂、定制备份策略或只读固件布局可能改变这一保证。

### 自动生成或临时文件

| 路径 | 生命周期 |
| --- | --- |
| `/var/run/msd_lite/*.conf` | 启动时由 UCI 和持久模板生成；不要直接修改。 |
| `/var/run/msd_lite/` | 运行目录，通常由 init 脚本删除并重建。 |
| `/tmp/msd_lite-openwrt-*` | 可选的 Release 上传暂存文件，生产运行不依赖。 |
| `/tmp/msd_lite-prefetch*` | 手工测试可能留下的 binary、配置和 PID，生产不需要。 |

`/var/run` 和 `/tmp` 是 tmpfs，重启即消失。通过 `-p` 指定的 PID 文件通常也只
属于运行时。

### 服务层变化

部署过程会停止、启动 `/etc/init.d/msd_lite`，但不需要修改这个 init 脚本。
procd 仍使用原 UCI instance，重新生成运行时 XML 后启动新 binary。

### 回滚

请按实际备份后缀调整命令：

```sh
/etc/init.d/msd_lite stop
cp /usr/bin/msd_lite.before-prefetch /usr/bin/msd_lite
chmod 755 /usr/bin/msd_lite
cp /etc/msd_lite/msd_lite.conf.sample.before-prefetch \
   /etc/msd_lite/msd_lite.conf.sample
cp /etc/config/msd_lite.before-prefetch /etc/config/msd_lite
/etc/init.d/msd_lite start
```

回滚 binary 时可以继续保留训练状态；如果想清除，必须在服务停止期间删除。

## 部署验证

```sh
ps w | grep '[m]sd_lite'
netstat -lntp | grep ':7088'
grep -n 'prefetch\\|ringBufSize\\|stateFile' /var/run/msd_lite/*.conf
ls -l /etc/msd_lite/prefetch.state
```

从其他机器进行 5 秒真实流测试：

```sh
curl --max-time 5 -o /dev/null \
  'http://路由器地址:7088/udp/239.0.0.1:1234'
```

收到数据后超时是正常的，因为直播流本身不会结束。

## 源码改动地图

本 fork 主要涉及以下受 Git 跟踪的文件：

| 路径 | 用途 |
| --- | --- |
| `src/channel_predict.c`、`.h` | 有界预测图、保留与排名、二进制持久化。 |
| `src/mpegts_gop.c`、`.h` | PAT/PMT 和 H.264/H.265 随机访问解析器。 |
| `src/msd_lite.c` | 配置、生命周期、空闲清理和定期持久化。 |
| `src/stream_sys.c`、`.h` | 预读 hub 与 GOP 起播集成。 |
| `src/msd_lite_stat_text.c` | 原有服务输出及构建兼容处理。 |
| `conf/msd_lite.conf` | 推荐配置。 |
| `tests/test_prefetch.c` | 解析、预测、反向推导和持久化测试。 |
| `tests/generate_prefetch_state.c` | 离线生成预置状态。 |
| `tests/benchmark_channel_start.sh` | 冷/热首字节重复测试。 |
| `tests/benchmark_with_resources.sh` | 延迟以及服务端 CPU/RSS 统计。 |
| `tests/results/` | 已记录的测试数据和说明。 |
| `CMakeLists.txt`、`src/CMakeLists.txt` | 构建、测试、Threads 和跨平台加固探测。 |
| `Makefile` | CMake 的简易入口。 |
| `.github/workflows/build-release.yml` | Linux/OpenWrt/macOS CI 与纯 binary Release。 |
| `.gitignore` | 忽略本地构建输出。 |
| `readme.md`、`readme_zh.md`、`docs/` | 普通用户和进阶文档。 |

`src/liblcb` 仍是上游 submodule，本 fork 没有在 submodule 内保留本地修改。

## 测试和性能脚本

```sh
make -j4
make test
```

也可以直接使用 CMake：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

测试覆盖基础库、PAT/PMT、H.264/H.265 GOP、正向预测、反向推导和状态持久化。
性能脚本的参数可查看各自 `--help`；资源测试会在重复测量首字节延迟时同步记录
服务端 CPU 和 RSS。

## CI 与 Release

push 和 PR 会构建、测试：

- Linux x86_64；
- 静态 musl OpenWrt x86_64、aarch64、armv7、mipsel；
- macOS arm64。

推送 `v*` tag 后，GitHub Release 只包含 6 个命名 binary，不包含 blockmap、
更新器 YAML、安装器或构建目录。当前没有原生 Windows 版本，因为项目依赖
POSIX 进程、socket、pthread、syslog 和 queue API。
