#!/usr/bin/env bash
set -euo pipefail
test "$#" = 1 || exit 2
for core in big little; do
    found=0
    for file in "$1/$core"/CMakeFiles/*/CMakeCXXCompiler.cmake; do
        test -f "$file" || continue
        compiler=$(sed -n 's/^set(CMAKE_CXX_COMPILER "\(.*\)")$/\1/p' "$file")
        test -n "$compiler" && test -x "$compiler"
        printf '%s compiler=%s\n' "$core" "$compiler"
        "$compiler" --version
        found=1
    done
    test "$found" = 1 || { echo "Missing configured compiler for $core" >&2; exit 1; }
done
