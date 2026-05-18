#!/bin/bash
# Run from within WSL: bash /mnt/c/Users/basju/Documents/Projects/node/sync_to_wsl.sh
set -eu
SRC=/mnt/c/Users/basju/Documents/Projects/node
DST=$HOME/csbuild/node
FILES=(
  client/config/config.hpp
  client/config/config.cpp
  csnode/include/csnode/blockchain.hpp
  csnode/src/blockchain.cpp
  csnode/include/csnode/caches_serialization_manager.hpp
  csnode/src/caches_serialization_manager.cpp
  csnode/src/node.cpp
  csnode/src/itervalidator.cpp
  csnode/src/roundstat_serializer.cpp
  csdb/include/csdb/storage.hpp
  csdb/src/storage.cpp
  tools/csdb_migrate/src/main.cpp
)
for f in "${FILES[@]}"; do
  cp -v "$SRC/$f" "$DST/$f"
done
