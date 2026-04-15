# ACTION CHECKLIST - Next Steps

## Immediate Actions

### Step 1: Rebuild the Code ✓
```bash
cd boilerplate
make clean
make
```
**What changed:** Fixed memory_hog to not exit on malloc failure, enhanced kernel monitor logging

### Step 2: Test the Changes
Choose ONE of these methods:

**Option A - Quick Manual Test:**
```bash
cd boilerplate
sudo insmod monitor.ko
sudo ./engine supervisor ./rootfs-base &
sleep 2

# Test Task 5
sudo dmesg -C
sudo ./engine start softdemo ./rootfs-alpha /memory_hog --soft-mib 32 --hard-mib 80
sleep 8
sudo dmesg | grep "SOFT LIMIT"  # Look for the soft-limit message
sudo ./engine ps

# Test Task 6
sudo dmesg -C
sudo ./engine start hardtest ./rootfs-alpha /memory_hog --soft-mib 50 --hard-mib 64
sleep 12  # Let it auto-kill
sudo dmesg | grep "HARD LIMIT"  # Look for the hard-limit message
sudo ./engine ps  # Should show hard_limit_killed
```

**Option B - Automated Test Script:**
```bash
cd boilerplate
chmod +x test_task5_task6.sh
sudo ./test_task5_task6.sh
```

### Step 3: Collect Evidence
Once tests pass, capture screenshots showing:

**For Task 5:**
- Split-screen with:
  - **Left:** dmesg output showing `[container_monitor] SOFT LIMIT container=softdemo ...`
  - **Right:** `engine ps` showing container in "running" state

**For Task 6:**
- Three-part screenshot:
  1. Container stdout showing memory allocations continuing
  2. dmesg showing `[container_monitor] HARD LIMIT container=hardtest ...`
  3. Supervisor output and `engine ps` showing "hard_limit_killed"

### Step 4: Document Your Results
Update your FEATURE_CHECKLIST.md with:
- Evidence file names/locations
- Brief description of what was changed and why
- Test procedure used
- Pass/Fail status

---

## Code Changes Made

| File | Change | Why |
|------|--------|-----|
| monitor.c | Enhanced timer_callback() | Better diagnostics when monitoring active|
| monitor.c | Enhanced monitor_ioctl() | Clear registration success/failure messages |
| memory_hog.c | Sleep instead of exit on malloc fail | Prevents premature exit before hard-limit kill |

See CODE_CHANGES_SUMMARY.md for detailed explanation

---

## Expected Test Results

### Task 5 Should Show:
```
✓ Kernel registers container with soft/hard limits
✓ Memory allocations proceed (8 MiB/sec)
✓ At ~T=5s: SOFT LIMIT message appears (RSS > 32 MiB)
✓ Container remains RUNNING (not killed)
✓ Allocations continue
```

### Task 6 Should Show:
```
✓ Kernel registers container with soft/hard limits  
✓ Memory allocations proceed (8 MiB/sec)
✓ At ~T=7s: SOFT LIMIT message appears first (50 MiB threshold)
✓ At ~T=9s: HARD LIMIT message appears (64 MiB exceeded)
✓ Container process killed by SIGKILL
✓ exit_reason shows "hard_limit_killed"
✓ No manual "./engine stop" command needed
```

---

## Troubleshooting Quick Links

**If soft-limit message doesn't appear:**
- → See CODE_CHANGES_SUMMARY.md → "Debugging: If Tests Still Fail" → "No SOFT LIMIT message"

**If hard-limit kill doesn't happen:**
- → Same document → "Container exits before limits trigger"

**If zombie processes appear:**
- → Same document → "Zombie processes visible"

---

## Files Reference

| File | Purpose |
|------|---------|
| CODE_CHANGES_SUMMARY.md | Explains what code changed and why |
| TASK5_TASK6_EVIDENCE_CHECKLIST.md | What evaluators will check |
| TASK5_TASK6_QUICK_REFERENCE.md | Side-by-side comparison of soft vs hard limits |
| boilerplate/test_task5_task6.sh | Automated test suite |
| boilerplate/build_and_test.sh | Build helper |

---

## Success Criteria

✓ All tests pass without manual intervention  
✓ dmesg shows clean SOFT LIMIT and HARD LIMIT messages  
✓ Container auto-kill happens for hard-limit  
✓ exit_reason metadata is correct  
✓ No manual stop commands needed for hard-limit test  

Once you have this working, you're ready to capture and submit evidence!

---

## Timeline: How Long This Takes

- Building: ~2-3 minutes
- Single manual test: ~2-3 minutes per test
- Automated test script: ~5 minutes for both
- Evidence collection: ~5-10 minutes
- **Total: ~15-20 minutes**

Go ahead and run the tests now!

