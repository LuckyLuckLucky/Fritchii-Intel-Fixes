#!/bin/sh
# Translates one compute environment with Intel's profile and the 8x16x16 matrix path.
h=$1
SWEEP_INTEL=1 SWEEP_COOP8=1 /c/work/Fritchii-intel/_build/bin/shader-sweep.exe --environment env_$h.bin --cache-version 52 --output coop/$h.spv > coop/$h.json 2>&1
