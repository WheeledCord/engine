#!/bin/sh
# Runs the go/no-go benches on a modest machine and writes bench_LABEL.txt beside the engine.
#   tools/bench_x61.sh x61
# Builds the engine and the two benches, pins them to the last CPU core, and records the machine.
# Nothing here opens a window: store_bench is pure CPU and draw_bench renders through EGL with no
# surface. Needs libEGL and its headers (Mesa) for draw_bench; without them only store_bench runs.
set -e
LABEL=${1:?usage: tools/bench_x61.sh LABEL}
cd "$(dirname "$0")/.."
OUT="bench_$LABEL.txt"
LAST=$(( $(nproc) - 1 ))
{
    echo "engine $(git rev-parse --short HEAD) label $LABEL date $(date -u +%Y-%m-%dT%H:%MZ)"
    echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
    echo "cores: $(nproc), pinned to core $LAST"
    if [ -r /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
        echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor) (performance is quieter)"
    fi
    command -v glxinfo >/dev/null 2>&1 && echo "gl: $(glxinfo -B 2>/dev/null | grep -m1 'OpenGL renderer' | cut -d: -f2-)"
} > "$OUT"
make -f Makefile.core build/core/store_bench >/dev/null
echo "== store_bench (condition 3)" >> "$OUT"
taskset -c "$LAST" ./build/core/store_bench >> "$OUT" 2>&1
if [ -f /usr/include/EGL/egl.h ] || [ -f /usr/local/include/EGL/egl.h ]; then
    make -f Makefile.core build/core/draw_bench >/dev/null
    echo "== draw_bench (condition 6)" >> "$OUT"
    LP_NUM_THREADS=1 taskset -c "$LAST" ./build/core/draw_bench >> "$OUT" 2>&1 || echo "draw_bench failed" >> "$OUT"
else
    echo "== draw_bench skipped: no EGL headers" >> "$OUT"
fi
make -f Makefile.core build/core/trench >/dev/null
echo "== swat-tower headless bot session, 3600 ticks (condition 3: gameplay tick cost)" >> "$OUT"
taskset -c "$LAST" ./build/core/trench run examples/swat-tower --headless --bot --ticks 3600 --seed 7 --bench >> "$OUT" 2>&1 || echo "trench failed" >> "$OUT"
echo "wrote $OUT"
