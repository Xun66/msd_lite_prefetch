# OpenWrt first-frame benchmark

Measured on `192.168.10.245` using the stock daemon on port 7088 and the
prefetch build on port 17088. Each value is wall-clock milliseconds from the
HTTP request until FFmpeg decodes one video frame. Test order alternates per
round to reduce ordering bias.

Run:

```sh
MSD_ROUNDS=10 ./tests/benchmark_channel_start.sh
```

H.264 (`239.3.1.60:8084`) results:

- stock cold mean: 2393.5 ms
- prefetched mean: 2065.5 ms
- successful paired wins: 10/10
- mean reduction: 328.0 ms (13.7%)

HEVC Main10/HDR (`239.3.1.118:8001`) results:

- stock cold successful-sample mean: 2642.2 ms (2/6 failed)
- prefetched successful-sample mean: 1485.2 ms (1/6 failed)
- valid paired wins: 3/3
- valid-pair mean reduction: 1251.0 ms (47.6%)

Exit code 69 samples are retained in the CSV but excluded from latency
statistics. The HEVC sample is small and has failures, so it is supporting
evidence rather than a precise population estimate.

## CPU and RAM sampling

Run the latency benchmark with one-second `/proc` sampling:

```sh
MSD_ROUNDS=10 ./tests/benchmark_with_resources.sh
```

In a 101.4-second H.264 run (101 resource samples):

| Process | Mean CPU | Peak 1s CPU | Mean RSS | Peak RSS |
|---|---:|---:|---:|---:|
| 7088 stock | 0.35% | 3.96% | 1.81 MiB | 5.16 MiB |
| 17088 prefetch | 4.16% | 5.94% | 12.67 MiB | 17.75 MiB |

CPU percentages are relative to one logical CPU. The OpenWrt test VM has two
logical CPUs. The prefetch process intentionally keeps receiving and parsing
the predicted multicast while the stock process is mostly idle between cold
requests. Its peak RSS corresponds roughly to two active 8 MiB channel ring
buffers plus daemon overhead.
