#!/bin/sh
# Translates every compute environment with the B580's real profile into hot/compute/intel/.
cd /c/work/Fritchii-intel/_tools/hot/compute
for f in env_*.bin; do
  h=${f#env_}; h=${h%.bin}
  [ -f intel/$h.spv ] && continue
  SWEEP_INTEL=1 SWEEP_COOP8=1 /c/work/Fritchii-intel/_build/bin/shader-sweep.exe --environment $f --cache-version 52 --output intel/$h.spv > /dev/null 2>&1
done
echo done
