#!/bin/bash

set -e

echo "=========================================="
echo "Cleaning previous builds..."
echo "=========================================="
make clean

echo ""
echo "=========================================="
echo "Building monitor kernel module..."
echo "=========================================="
make module

echo ""
echo "=========================================="
echo "Building user-space tools..."
echo "=========================================="
make

echo ""
echo "=========================================="
echo "Build complete! Ready to test."
echo "=========================================="
echo ""
echo "To test, run:"
echo ""
echo "  Terminal 1: sudo dmesg -C && sudo dmesg -w"
echo ""
echo "  Terminal 2:"
echo "    cd boilerplate"
echo "    sudo insmod monitor.ko"
echo "    sudo ./engine supervisor ./rootfs-base &"
echo "    sleep 2"
echo "    sudo ./engine start softdemo ./rootfs-alpha /memory_hog --soft-mib 32 --hard-mib 80"
echo "    sleep 8"
echo "    sudo dmesg | grep 'container_monitor'"
echo "    sudo ./engine ps"
echo ""
