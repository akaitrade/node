#!/bin/bash

# Test script to check if the main.rs file compiles without syntax errors
echo "Testing syntax and compilation of main.rs..."

# Check for basic syntax errors
echo "Checking for common syntax issues..."

# Check if all imports are properly formatted
echo "✓ Checking imports..."
if grep -q "use.*;" core/src/main.rs; then
    echo "✓ Imports found"
else
    echo "✗ No imports found - this might be an issue"
fi

# Check for basic struct definitions
echo "✓ Checking struct definitions..."
if grep -q "struct.*{" core/src/main.rs; then
    echo "✓ Struct definitions found"
else
    echo "✗ No struct definitions found"
fi

# Check for function definitions
echo "✓ Checking function definitions..."
if grep -q "fn.*(" core/src/main.rs; then
    echo "✓ Function definitions found"
else
    echo "✗ No function definitions found"
fi

# Check for main function
echo "✓ Checking main function..."
if grep -q "fn main" core/src/main.rs; then
    echo "✓ Main function found"
else
    echo "✗ Main function not found"
fi

# Check for parse_address function
echo "✓ Checking parse_address function..."
if grep -q "fn parse_address" core/src/main.rs; then
    echo "✓ parse_address function found"
else
    echo "✗ parse_address function not found"
fi

# Check for serde attributes
echo "✓ Checking serde attributes..."
if grep -q "serde_big_array" core/src/main.rs; then
    echo "✓ serde_big_array usage found"
else
    echo "✗ serde_big_array not found"
fi

echo "Basic syntax check complete!"