#!/usr/bin/env bash
set -euo pipefail

echo "=== Building Valkey ==="
make -j$(nproc) lmvalkey valkey

echo "=== Running REST integration tests ==="
./runtest --single unit/rest

echo "=== Running basic integration tests ==="
./runtest --single unit/quit

echo "=== All tests passed! ==="
