#!/usr/bin/env bash
# Read-only. Does not install, unload, reset, or change compute mode/clocks.
set -u
date -u
uname -a
nvcc --version
nvidia-smi -q
cat /proc/driver/nvidia/version
ls -l /dev/nvidia*
