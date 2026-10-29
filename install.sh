#!/bin/sh
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
legacy_dir=$(dirname "$project_dir")/iiSocialPublish
# Preserve a pre-rename CMake cache until it has been regenerated at the new path.
if [ -L "$legacy_dir" ] && [ -f "$project_dir/build/CMakeCache.txt" ] &&
   grep -Fq "CMAKE_HOME_DIRECTORY:INTERNAL=$legacy_dir" "$project_dir/build/CMakeCache.txt"; then
    project_dir=$legacy_dir
fi
prefix=${1:-"$HOME/.local/SDK/iiSocialIntegration"}
cmake -S "$project_dir" -B "$project_dir/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix"
cmake --build "$project_dir/build" --parallel
ctest --test-dir "$project_dir/build" --output-on-failure
cmake --install "$project_dir/build"
