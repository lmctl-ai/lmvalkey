#!/usr/bin/env bash
set -euo pipefail

echo "=== Building Valkey ==="
make -j$(nproc)

echo "=== Running REST integration tests ==="
./runtest --single unit/rest

echo "=== Running basic integration tests ==="
./runtest --single unit/quit

echo "=== Running protocol & client integration tests ==="
./tests/integration-test.sh

echo "=== All tests passed! ==="
