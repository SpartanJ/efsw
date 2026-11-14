#!/usr/bin/env bash
# Runs a range of efsw-fuzz seeds in parallel, keeps a log per seed, and lists the failing seeds.
#
# Usage: campaign.sh <efsw-fuzz binary> <log directory> <first seed> <last seed> [-j jobs]
#                    [-r rounds] [-- efsw-fuzz options]
#
#   -j jobs     seeds run at once (default 8). The fuzzer mostly waits for efsw, so more jobs
#               than cores is fine; but load changes timing, and so which seeds fail.
#   -r rounds   rounds per seed (default 25)
#
# Example, the busiest layout, 400 seeds:
#   src/fuzz/campaign.sh bin/efsw-fuzz-release /tmp/fuzz-optall 1 400 -- --generic --optall --maxops 12
#
# Each seed's output is in <log directory>/seed_<seed>.log. Exits with 1 if a seed failed.
set -u

usage() {
	sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
}

[ $# -ge 4 ] || usage
fuzzer=$1
logs=$2
first=$3
last=$4
shift 4
jobs=8
rounds=25
while [ $# -gt 0 ]; do
	case $1 in
		-j) jobs=$2; shift 2 ;;
		-r) rounds=$2; shift 2 ;;
		--) shift; break ;;
		*) usage ;;
	esac
done

[ -x "$fuzzer" ] || { echo "not an executable: $fuzzer" >&2; exit 2; }
mkdir -p "$logs" || exit 2

export fuzzer logs rounds
seq "$first" "$last" | xargs -P "$jobs" -I{} sh -c \
	'"$fuzzer" {} "$rounds" "$@" > "$logs/seed_{}.log" 2>&1' sh "$@"

seedsOf() { sed 's/.*seed_\([0-9]*\)\.log/\1/' | sort -n | tr '\n' ' '; }
failing=$(grep -l "FAILED" "$logs"/seed_*.log 2>/dev/null | seedsOf)
# A seed that did not finish, on an error or a timeout, has no summary line.
broken=$(grep -L "^seed [0-9]*: [0-9]* failing rounds of" "$logs"/seed_*.log 2>/dev/null | seedsOf)
total=$((last - first + 1))
echo "$(echo $failing | wc -w | tr -d ' ') of $total seeds failed${failing:+: $failing}"
[ -z "$broken" ] || echo "$(echo $broken | wc -w | tr -d ' ') of $total seeds did not finish: $broken"
[ -z "$failing" ] && [ -z "$broken" ]
