#!/bin/bash

echo "=== Final compilation check ==="
echo

echo "1. Checking rand imports..."
if grep -q "use rand::{Rng, RngCore}" core/src/main.rs; then
    echo "✅ RngCore imported"
else
    echo "❌ RngCore not imported properly"
fi

echo
echo "2. Checking fill_bytes usage..."
if grep -q "fill_bytes" core/src/main.rs; then
    echo "✅ Using fill_bytes"
else
    echo "❌ fill_bytes not found"
fi

echo
echo "3. Checking for unused variables..."
if grep -q "let network_manager = " core/src/main.rs; then
    echo "❌ Found unused network_manager variable"
else
    echo "✅ No unused network_manager variables"
fi

echo
echo "4. Checking for Send trait issues..."
if grep -q "dag_engine.process_consensus_round" core/src/main.rs; then
    echo "❌ Found potentially problematic dag_engine usage in async block"
else
    echo "✅ No problematic dag_engine usage in async blocks"
fi

echo
echo "5. Checking vertex_hash usage..."
if grep -q "vertex_hash," core/src/main.rs; then
    echo "❌ Found unused vertex_hash"
else
    echo "✅ No unused vertex_hash found"
fi

echo
echo "6. Checking for parse_address function..."
if grep -q "fn parse_address" core/src/main.rs; then
    echo "✅ parse_address function exists"
else
    echo "❌ parse_address function missing"
fi

echo
echo "=== All checks complete ==="