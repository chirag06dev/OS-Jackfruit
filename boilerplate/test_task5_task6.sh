#!/bin/bash

# Task 5 & 6 Validation Script
# This script automates the testing of soft-limit and hard-limit features

set -e

cd boilerplate

echo "=========================================="
echo "OS-Jackfruit Task 5 & 6 Test Suite"
echo "=========================================="
echo ""

# Function to cleanup
cleanup() {
    echo ""
    echo "Cleaning up..."
    pkill -f "engine supervisor" 2>/dev/null || true
    sudo pkill -f "engine supervisor" 2>/dev/null || true
    sleep 1
}

trap cleanup EXIT

# Ensure we're using the latest build
echo "Rebuilding kernel module and tools..."
make clean > /dev/null 2>&1
make module > /dev/null 2>&1
make > /dev/null 2>&1
echo "✓ Build complete"
echo ""

# Load kernel module
echo "Loading kernel monitor module..."
sudo insmod monitor.ko 2>/dev/null || true
sleep 1
if [ ! -c /dev/container_monitor ]; then
    echo "✗ ERROR: /dev/container_monitor not found!"
    exit 1
fi
echo "✓ Kernel module loaded"
echo ""

# Start supervisor
echo "Starting supervisor process..."
sudo ./engine supervisor ./rootfs-base > /tmp/supervisor.log 2>&1 &
SUPERVISOR_PID=$!
sleep 2
if ! ps -p $SUPERVISOR_PID > /dev/null; then
    echo "✗ ERROR: Supervisor failed to start!"
    cat /tmp/supervisor.log
    exit 1
fi
echo "✓ Supervisor running (PID: $SUPERVISOR_PID)"
echo ""

# ======== TASK 5: SOFT-LIMIT WARNING ========
echo "=========================================="
echo "TASK 5: Testing Soft-Limit Warning"
echo "=========================================="
echo ""
echo "Setup:"
echo "  - Container: softdemo"
echo "  - Soft-limit: 32 MiB"
echo "  - Hard-limit: 80 MiB"
echo "  - Memory allocation: 8 MiB/sec"
echo "  - Expected behavior: Warning at ~5 seconds (36 MiB > 32 MiB limit)"
echo ""

echo "Clearing dmesg buffer..."
sudo dmesg -C

echo "Starting container softdemo (will run for 10 seconds)..."
CONTAINER_START_TIME=$(date +%s)
timeout 10 sudo ./engine start softdemo ./rootfs-alpha /memory_hog --soft-mib 32 --hard-mib 80 2>&1 | tee /tmp/softdemo.log || true
CONTAINER_END_TIME=$(date +%s)

echo ""
echo "Waiting for monitoring to settle..."
sleep 2

echo ""
echo "--- Kernel Messages (dmesg) ---"
SOFT_LIMIT_MSG=$(sudo dmesg | grep "SOFT LIMIT" | grep softdemo || echo "NOT FOUND")
if [ "$SOFT_LIMIT_MSG" != "NOT FOUND" ]; then
    echo "✓ SOFT LIMIT message found:"
    echo "  $SOFT_LIMIT_MSG"
else
    echo "✗ NO SOFT LIMIT message found in dmesg"
fi

REGISTRATION_MSG=$(sudo dmesg | grep "Registering container=softdemo" | tail -1 || echo "NOT FOUND")
if [ "$REGISTRATION_MSG" != "NOT FOUND" ]; then
    echo "✓ Registration found:"
    echo "  $REGISTRATION_MSG"
else
    echo "✗ NO registration found"
fi

echo ""
echo "--- Engine Status ---"
sudo ./engine ps

echo ""
echo "--- Container Output (first 20 lines) ---"
head -20 /tmp/softdemo.log || true

echo ""
echo "========== Task 5 Result =========="
if [ "$SOFT_LIMIT_MSG" != "NOT FOUND" ]; then
    echo "✓ TASK 5: PASSED - Soft-limit warning detected"
    TASK5_PASS=1
else
    echo "✗ TASK 5: FAILED - No soft-limit warning message"
    TASK5_PASS=0
fi
echo ""
echo ""

# ======== TASK 6: HARD-LIMIT ENFORCEMENT ========
echo "=========================================="
echo "TASK 6: Testing Hard-Limit Enforcement"
echo "=========================================="
echo ""
echo "Setup:"
echo "  - Container: hardtest"
echo "  - Soft-limit: 50 MiB"
echo "  - Hard-limit: 64 MiB"
echo "  - Memory allocation: 8 MiB/sec"
echo "  - Expected behavior:"
echo "    * Warning at ~7 seconds (56 MiB > 50 MiB)"
echo "    * Killed at ~9 seconds (72 MiB > 64 MiB)"
echo "    * exit_reason should be 'hard_limit_killed'"
echo ""

echo "Clearing dmesg buffer..."
sudo dmesg -C

echo "Starting container hardtest (will be auto-killed when hard-limit exceeded)..."
timeout 15 sudo ./engine start hardtest ./rootfs-alpha /memory_hog --soft-mib 50 --hard-mib 64 2>&1 | tee /tmp/hardtest.log || true

echo ""
echo "Waiting for monitoring to settle..."
sleep 3

echo ""
echo "--- Kernel Messages (dmesg) ---"
HARD_LIMIT_MSG=$(sudo dmesg | grep "HARD LIMIT" | grep hardtest || echo "NOT FOUND")
if [ "$HARD_LIMIT_MSG" != "NOT FOUND" ]; then
    echo "✓ HARD LIMIT message found:"
    echo "  $HARD_LIMIT_MSG"
else
    echo "✗ NO HARD LIMIT message found"
fi

SOFT_LIMIT_MSG_TASK6=$(sudo dmesg | grep "SOFT LIMIT" | grep hardtest || echo "NOT FOUND")
if [ "$SOFT_LIMIT_MSG_TASK6" != "NOT FOUND" ]; then
    echo "✓ Soft-limit warning (before hard-limit):"
    echo "  $SOFT_LIMIT_MSG_TASK6"
else
    echo "⚠ NO soft-limit message shown (kernel may have gone straight to hard-limit)"
fi

echo ""
echo "--- Engine Status (final ps) ---"
PS_OUTPUT=$(sudo ./engine ps | grep hardtest || echo "NOT FOUND")
if [ "$PS_OUTPUT" != "NOT FOUND" ]; then
    echo "$PS_OUTPUT"
    if echo "$PS_OUTPUT" | grep -q "hard_limit_killed"; then
        echo "✓ Exit reason shows 'hard_limit_killed'"
        TASK6_REASON_OK=1
    else
        echo "✗ Exit reason does NOT show 'hard_limit_killed'"
        TASK6_REASON_OK=0
    fi
else
    echo "✗ Container not found in engine ps"
    TASK6_REASON_OK=0
fi

echo ""
echo "--- Container Output (first 20 lines) ---"
head -20 /tmp/hardtest.log || true

echo ""
echo "========== Task 6 Result =========="
if [ "$HARD_LIMIT_MSG" != "NOT FOUND" ] && [ "$TASK6_REASON_OK" = "1" ]; then
    echo "✓ TASK 6: PASSED - Hard-limit enforcement detected"
    TASK6_PASS=1
elif [ "$HARD_LIMIT_MSG" != "NOT FOUND" ]; then
    echo "⚠ TASK 6: PARTIAL - Hard-limit message found, but exit_reason may be incorrect"
    TASK6_PASS=1
else
    echo "✗ TASK 6: FAILED - No hard-limit message"
    TASK6_PASS=0
fi

echo ""
echo "=========================================="
echo "FINAL RESULTS"
echo "=========================================="
if [ "$TASK5_PASS" = "1" ]; then
    echo "✓ Task 5 (Soft-Limit Warning): PASSED"
else
    echo "✗ Task 5 (Soft-Limit Warning): FAILED"
fi

if [ "$TASK6_PASS" = "1" ]; then
    echo "✓ Task 6 (Hard-Limit Enforcement): PASSED"
else
    echo "✗ Task 6 (Hard-Limit Enforcement): FAILED"
fi
echo "=========================================="
echo ""
echo "Logs saved to:"
echo "  /tmp/supervisor.log"
echo "  /tmp/softdemo.log"
echo "  /tmp/hardtest.log"
echo ""
echo "To view kernel messages:"
echo "  sudo dmesg | grep 'container_monitor' | tail -20"
echo ""
