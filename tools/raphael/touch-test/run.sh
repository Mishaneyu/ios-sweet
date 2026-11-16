#!/bin/sh
# Host test of the raphael touch board in Bergamota's xnu-11215 sweet_touch.c.
# usage: run.sh [path to sweet_touch.c]  (from a tree patched with xnu/patches-11215)
set -e
cd "$(dirname "$0")"
SRC=${1:?usage: run.sh PATH/TO/patched/bsd/dev/sweet_touch.c}
mkdir -p build
python mk_testcopy.py "$SRC" build/sweet_touch_test.c
gcc -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
    -Wno-sign-compare -DARM64_BOARD_CONFIG_SWEET -Ibuild -Istubs \
    -o build/test_touch test_touch.c
./build/test_touch
