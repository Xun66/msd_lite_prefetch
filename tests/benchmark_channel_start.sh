#!/usr/bin/env bash
set -u

# Paired cold-vs-prefetched IPTV first-frame benchmark.
# Measures wall time from HTTP open until FFmpeg decodes one video frame.

host="${MSD_HOST:-192.168.10.245}"
cold_port="${MSD_COLD_PORT:-7088}"
warm_port="${MSD_WARM_PORT:-17088}"
rounds="${MSD_ROUNDS:-10}"
settle_seconds="${MSD_SETTLE_SECONDS:-2}"
timeout_seconds="${MSD_TIMEOUT_SECONDS:-12}"
prepare_channel="${MSD_PREPARE_CHANNEL:-239.3.1.129:8008}"
target_channel="${MSD_TARGET_CHANNEL:-239.3.1.60:8084}"
output="${MSD_OUTPUT:-/tmp/msd-channel-start-$(date +%Y%m%d-%H%M%S).csv}"

need_command() {
	if ! command -v "$1" >/dev/null 2>&1; then
		echo "missing required command: $1" >&2
		exit 2
	fi
}

need_command curl
need_command date
need_command ffmpeg
need_command sort
need_command ssh
need_command timeout

case "$rounds" in
	''|*[!0-9]*) echo "MSD_ROUNDS must be a positive integer" >&2; exit 2 ;;
esac
if ((rounds < 1)); then
	echo "MSD_ROUNDS must be a positive integer" >&2
	exit 2
fi

cold_url="http://${host}:${cold_port}/udp/${target_channel}"
warm_url="http://${host}:${warm_port}/udp/${target_channel}"
prepare_url="http://${host}:${warm_port}/udp/${prepare_channel}"

measure_frame() {
	local url="$1" start end rc
	start="$(date +%s%N)"
	timeout "$timeout_seconds" ffmpeg \
		-nostdin -v error \
		-analyzeduration 2000000 -probesize 6000000 \
		-i "$url" -map 0:v:0 -frames:v 1 -f null - \
		>/dev/null 2>&1
	rc=$?
	end="$(date +%s%N)"
	echo "$(((end - start) / 1000000)),$rc"
}

prepare_warm_target() {
	# Tuning the learned predecessor makes target_channel a warm prediction.
	curl --max-time 1 -fsS -o /dev/null "$prepare_url" \
		>/dev/null 2>&1 || true
	sleep "$settle_seconds"
}

measure_cold() {
	# Give the stock daemon's one-second timer time to destroy a zero-client hub.
	sleep "$settle_seconds"
	measure_frame "$cold_url"
}

measure_warm() {
	prepare_warm_target
	measure_frame "$warm_url"
}

summarize_one() {
	local label="$1" column="$2" values count failed min max mean std median p90
	values="$(mktemp)"
	awk -F, -v col="$column" 'NR > 1 && $(col + 1) == 0 {print $col}' \
		"$output" | sort -n >"$values"
	count="$(wc -l <"$values" | tr -d ' ')"
	failed="$(awk -F, -v col="$column" 'NR > 1 && $(col + 1) != 0 {n++} END {print n + 0}' "$output")"
	if ((count == 0)); then
		echo "$label: no successful samples, failed=$failed"
		rm -f "$values"
		return
	fi
	read -r min max mean std < <(
		awk '
			NR == 1 {min = $1}
			{a[NR] = $1; sum += $1; sumsq += $1 * $1; max = $1}
			END {
				mean = sum / NR
				variance = (sumsq / NR) - (mean * mean)
				if (variance < 0) variance = 0
				printf "%.0f %.0f %.1f %.1f\n",
				    min, max, mean, sqrt(variance)
			}' "$values"
	)
	median="$(awk -v n="$count" '
		NR == int((n + 1) / 2) {lo = $1}
		NR == int((n + 2) / 2) {hi = $1}
		END {printf "%.1f", (lo + hi) / 2}' "$values")"
	p90_rank="$(((90 * count + 99) / 100))"
	p90="$(awk -v rank="$p90_rank" 'NR == rank {print; exit}' "$values")"
	echo "$label: n=$count failed=$failed mean=${mean}ms median=${median}ms" \
	    "p90=${p90}ms stddev=${std}ms min=${min}ms max=${max}ms"
	rm -f "$values"
}

summarize_pairs() {
	local values count wins mean median p90 percent
	values="$(mktemp)"
	awk -F, '
		NR > 1 && $3 == 0 && $5 == 0 {
			print $2 - $4
		}' "$output" | sort -n >"$values"
	count="$(wc -l <"$values" | tr -d ' ')"
	if ((count == 0)); then
		echo "paired improvement: no successful pairs"
		rm -f "$values"
		return
	fi
	wins="$(awk '$1 > 0 {n++} END {print n + 0}' "$values")"
	mean="$(awk '{sum += $1} END {printf "%.1f", sum / NR}' "$values")"
	median="$(awk -v n="$count" '
		NR == int((n + 1) / 2) {lo = $1}
		NR == int((n + 2) / 2) {hi = $1}
		END {printf "%.1f", (lo + hi) / 2}' "$values")"
	p90_rank="$(((90 * count + 99) / 100))"
	p90="$(awk -v rank="$p90_rank" 'NR == rank {print; exit}' "$values")"
	percent="$(awk -F, '
		NR > 1 && $3 == 0 && $5 == 0 {
			cold += $2; warm += $4
		}
		END {printf "%.1f", 100 * (cold - warm) / cold}' "$output")"
	echo "paired improvement: wins=${wins}/${count}" \
	    "mean=${mean}ms median=${median}ms p90=${p90}ms reduction=${percent}%"
	rm -f "$values"
}

if [[ "${MSD_SUMMARY_ONLY:-0}" == "1" ]]; then
	if [[ ! -f "$output" ]]; then
		echo "results file not found: $output" >&2
		exit 2
	fi
	echo "Results: $output"
	summarize_one "7088 cold" 2
	summarize_one "17088 prefetched" 4
	summarize_pairs
	exit 0
fi

echo "Checking remote baseline (no credential files are inspected)..."
ssh -o BatchMode=yes -o ConnectTimeout=8 "root@${host}" \
	"echo cold; wget -qO- http://127.0.0.1:${cold_port}/hubstat | grep '${target_channel}' || true; \
	 echo warm; wget -qO- http://127.0.0.1:${warm_port}/hubstat | grep '${target_channel}' || true" \
	|| echo "warning: remote baseline check failed; continuing" >&2

# Ensure the warm instance has at least two A->B observations. These samples
# are training only and are excluded from measured results.
echo "Training/confirming ${prepare_channel} -> ${target_channel}..."
for channel in "$prepare_channel" "$target_channel" \
    "$prepare_channel" "$target_channel" "$prepare_channel"; do
	curl --max-time 1 -fsS -o /dev/null \
	    "http://${host}:${warm_port}/udp/${channel}" >/dev/null 2>&1 || true
done
sleep "$settle_seconds"

echo "round,cold_ms,cold_rc,warm_ms,warm_rc,order" >"$output"
echo "Running $rounds paired samples..."
for ((round = 1; round <= rounds; round++)); do
	if ((round % 2)); then
		cold="$(measure_cold)"
		warm="$(measure_warm)"
		order="cold-warm"
	else
		warm="$(measure_warm)"
		cold="$(measure_cold)"
		order="warm-cold"
	fi
	echo "$round,$cold,$warm,$order" >>"$output"
	echo "round=$round cold=${cold%,*}ms(rc=${cold#*,})" \
	    "warm=${warm%,*}ms(rc=${warm#*,}) order=$order"
done

echo
echo "Results: $output"
summarize_one "7088 cold" 2
summarize_one "17088 prefetched" 4
summarize_pairs
