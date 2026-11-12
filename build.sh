#!/bin/sh
set -eu

cd "$(dirname "$0")"

cmake -S . -B build -DMATCONV_FETCH_DEPENDENCIES=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
