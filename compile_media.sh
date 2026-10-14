#!/bin/sh
cd /mnt/data/koralo_r45_parallel
clang++ -std=c++20 -Wall -Wextra -Wpedantic -Wshadow -O0 -g0 -Isrc -c src/media/media_engine.cpp -o build-parallel/CMakeFiles/koralo_core.dir/src/media/media_engine.cpp.o
result=$?
echo "$result" > compile_media.exit
exit "$result"
