#!/usr/bin/env bash
# tests/run.sh -- compile and link the tests in every memory model with the
# installed toolchain (propeller-elf-gcc on PATH) at -O2 and -Os.
#
# With P1RUN set to a Propeller 1 emulator that runs a Spin .binary and
# prints its serial output (emu/'s p1run: "make -C emu" builds
# emu/build/p1run), the hub-memory builds (LMM, CMM) are also run and must
# print their "passed" line.
#
#   tests/run.sh
#   make -C emu && P1RUN=emu/build/p1run tests/run.sh
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
CC=${CC:-propeller-elf-gcc}
OUT=$(mktemp -d)
MODELS="lmm cmm xmmc xmm-single xmm-split xmm"
fails=0

for t in bench regress; do
    for opt in -O2 -Os; do
        for m in $MODELS; do
            elf=$OUT/$t$opt-$m.elf
            if $CC $opt -m$m -o "$elf" "$HERE/$t.c" 2> "$OUT/err"; then
                printf '  %-8s %-3s %-11s ok\n' "$t" "$opt" "$m"
            else
                printf '  %-8s %-3s %-11s FAILED TO BUILD\n' "$t" "$opt" "$m"
                sed 's/^/      /' "$OUT/err"
                fails=$((fails + 1))
            fi
        done
    done
done

if [ -n "${P1RUN:-}" ]; then
    for t in bench regress; do
        for opt in -O2 -Os; do
            for m in lmm cmm; do
                elf=$OUT/$t$opt-$m.elf
                [ -f "$elf" ] || continue
                (cd "$OUT" && propeller-load -s "$elf" > /dev/null 2>&1)
                bin=${elf%.elf}.binary
                out=$("$P1RUN" "$bin" --baud 115200 --seconds 20 --until passed 2> /dev/null)
                if grep -q "passed" <<< "$out"; then
                    printf '  run %-8s %-3s %-4s %s\n' "$t" "$opt" "$m" "$(head -n1 <<< "$out" | tr -d '\r')"
                else
                    printf '  run %-8s %-3s %-4s FAILED\n' "$t" "$opt" "$m"
                    sed 's/^/      /' <<< "$out"
                    fails=$((fails + 1))
                fi
            done
        done
    done
fi

rm -rf "$OUT"
if [ $fails -eq 0 ]; then
    echo "all tests passed"
else
    echo "$fails tests failed"
    exit 1
fi
