#!/bin/bash

echo "=== Checking compilation fixes ==="
echo

echo "1. Checking for rng.fill() usage (should be rng.fill_bytes())..."
if grep -n "rng\.fill(" core/src/main.rs; then
    echo "❌ Found rng.fill() - should be rng.fill_bytes()"
else
    echo "✅ No rng.fill() found - using rng.fill_bytes()"
fi

echo
echo "2. Checking for vertices_to_process usage (should be vertex_hashes)..."
if grep -n "vertices_to_process" core/src/main.rs; then
    echo "❌ Found vertices_to_process - should be vertex_hashes"
else
    echo "✅ No vertices_to_process found"
fi

echo
echo "3. Checking for vertex_hash unused variable..."
if grep -n "vertex_hash," core/src/main.rs; then
    echo "❌ Found unused vertex_hash variable"
else
    echo "✅ No unused vertex_hash variable found"
fi

echo
echo "4. Checking for serde_big_array usage..."
if grep -n "serde_big_array" core/src/main.rs; then
    echo "✅ Found serde_big_array usage"
else
    echo "❌ No serde_big_array found"
fi

echo
echo "5. Checking for parse_address function..."
if grep -n "fn parse_address" core/src/main.rs; then
    echo "✅ Found parse_address function"
else
    echo "❌ No parse_address function found"
fi

echo
echo "=== Fix verification complete ==="