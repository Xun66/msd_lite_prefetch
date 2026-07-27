#!/usr/bin/env bash
set -euo pipefail

# Runs benchmark_channel_start.sh while sampling both OpenWrt daemon processes.

host="${MSD_HOST:-192.168.10.245}"
rounds="${MSD_ROUNDS:-10}"
result_dir="${MSD_RESULT_DIR:-tests/results}"
label="${MSD_RESULT_LABEL:-resource-h264-10}"
latency_csv="${result_dir}/${label}-latency.csv"
resource_csv="${result_dir}/${label}-resources.csv"
summary_txt="${result_dir}/${label}-summary.txt"

mkdir -p "$result_dir"

read -r cold_pid warm_pid clk_tck < <(
	ssh -o BatchMode=yes -o ConnectTimeout=8 "root@${host}" '
		cold="$(ps w | awk "/\\/usr\\/bin\\/msd_lite / && !/awk/ {print \$1; exit}")"
		warm="$(cat /tmp/msd_lite-prefetch.pid)"
		clk="$(getconf CLK_TCK 2>/dev/null || echo 100)"
		echo "$cold $warm $clk"
	'
)

if [[ -z "$cold_pid" || -z "$warm_pid" ]]; then
	echo "unable to find both daemon PIDs" >&2
	exit 2
fi
echo "Sampling stock PID $cold_pid and prefetch PID $warm_pid..."

ssh -o BatchMode=yes "root@${host}" "
	echo uptime_s,cold_ticks,cold_rss_kb,cold_vmsize_kb,warm_ticks,warm_rss_kb,warm_vmsize_kb
	while kill -0 $cold_pid 2>/dev/null && kill -0 $warm_pid 2>/dev/null; do
		up=\$(awk '{print \$1}' /proc/uptime)
		ct=\$(awk '{print \$14+\$15}' /proc/$cold_pid/stat)
		cr=\$(awk '/^VmRSS:/ {print \$2}' /proc/$cold_pid/status)
		cv=\$(awk '/^VmSize:/ {print \$2}' /proc/$cold_pid/status)
		wt=\$(awk '{print \$14+\$15}' /proc/$warm_pid/stat)
		wr=\$(awk '/^VmRSS:/ {print \$2}' /proc/$warm_pid/status)
		wv=\$(awk '/^VmSize:/ {print \$2}' /proc/$warm_pid/status)
		echo \$up,\$ct,\$cr,\$cv,\$wt,\$wr,\$wv
		sleep 1
	done
" >"$resource_csv" &
sampler_pid=$!

cleanup() {
	kill "$sampler_pid" 2>/dev/null || true
	wait "$sampler_pid" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

MSD_ROUNDS="$rounds" MSD_OUTPUT="$latency_csv" \
	./tests/benchmark_channel_start.sh

cleanup
trap - EXIT INT TERM

summarize_process() {
	local name="$1" tick_col="$2" rss_col="$3" vm_col="$4"
	awk -F, -v name="$name" -v tc="$tick_col" -v rc="$rss_col" \
	    -v vc="$vm_col" -v hz="$clk_tck" '
		NR == 2 {
			first_up = prev_up = $1
			first_ticks = prev_ticks = $tc
		}
		NR > 1 {
			samples++
			rss_sum += $rc
			vm_sum += $vc
			if ($rc > rss_peak) rss_peak = $rc
			if ($vc > vm_peak) vm_peak = $vc
			if (NR > 2 && $1 > prev_up) {
				cpu = 100 * ($tc - prev_ticks) / (hz * ($1 - prev_up))
				if (cpu > cpu_peak) cpu_peak = cpu
			}
			last_up = $1
			last_ticks = $tc
			prev_up = $1
			prev_ticks = $tc
		}
		END {
			elapsed = last_up - first_up
			if (elapsed > 0)
				cpu_mean = 100 * (last_ticks - first_ticks) /
				    (hz * elapsed)
			else
				cpu_mean = 0
			printf "%s: samples=%d mean_cpu=%.2f%% peak_1s_cpu=%.2f%% ",
			    name, samples, cpu_mean, cpu_peak
			printf "mean_rss=%.1fMiB peak_rss=%.1fMiB ",
			    rss_sum / samples / 1024, rss_peak / 1024
			printf "mean_vmsize=%.1fMiB peak_vmsize=%.1fMiB\n",
			    vm_sum / samples / 1024, vm_peak / 1024
		}' "$resource_csv"
}

{
	echo "Resource sampling interval: 1 second; CLK_TCK=$clk_tck"
	summarize_process "7088 stock" 2 3 4
	summarize_process "17088 prefetch" 5 6 7
	echo
	MSD_SUMMARY_ONLY=1 MSD_OUTPUT="$latency_csv" \
	    ./tests/benchmark_channel_start.sh
} | tee "$summary_txt"

echo "Resource samples: $resource_csv"
echo "Summary: $summary_txt"
