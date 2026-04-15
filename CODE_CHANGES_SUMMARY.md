# Code Changes Summary - Task 5 & 6 Fixes

## Changes Made

### 1. monitor.c - timer_callback() Enhancement
**File:** `boilerplate/monitor.c`

**Changes:**
- Added entry count tracking to detect when monitoring is active with no containers
- Added informational logging when processes exit (helps debug)
- Added "Memory OK" logging when memory drops below soft-limit
- Added periodic heartbeat messages every 10 cycles to verify timer is running with data
- These changes ensure comprehensive visibility into what the kernel monitor is doing

**Key improvement:** If containers aren't being registered properly, the heartbeat messages will show there are no containers monitored, making debugging easier.

---

### 2. monitor.c - monitor_ioctl() Enhancement  
**File:** `boilerplate/monitor.c`

**Changes:**
- Added enhanced error logging for registration failures
- Added SUCCESS logging messages to confirm containers are registered
- Added explicit validation error messages if soft-limit > hard-limit
- Added memory allocation failure logging

**Key improvement:** You'll now see clear messages like:
```
[container_monitor] Registering container=softdemo pid=12345 soft=33554432 hard=83886080
[container_monitor] Registration SUCCESS container=softdemo pid=12345
```

Or if registration fails:
```
[container_monitor] Registration FAILED: soft-limit > hard-limit container=softdemo
```

---

### 3. memory_hog.c - Process Lifecycle Improvement
**File:** `boilerplate/memory_hog.c`

**Changes:**
- When malloc fails, instead of exiting, the process now sleeps indefinitely
- This ensures the container keeps running even after it can't allocate more memory
- The kernel monitor can then kill it when hard-limit is exceeded
- Added startup message showing the allocation parameters being used

**Key improvement:** Prevents premature exit. The container will now be killed by the kernel when hard-limit is exceeded, not by malloc failure.

**Before:**
```c
if (!mem) {
    printf("malloc failed after %d allocations\n", count);
    break;  // ← EXITS HERE - never reaches hard-limit kill
}
```

**After:**
```c
if (!mem) {
    printf("allocation=%d malloc failed after %d allocations, sleeping indefinitely\n", 
           count, count);
    fflush(stdout);
    while (1) sleep(1);  // ← SLEEPS - gives kernel time to kill it
}
```

---

## How to Apply and Test

### Step 1: Rebuild
```bash
cd boilerplate
make clean
make
```

### Step 2: Run Tests
```bash
# Option A: Run manual tests
sudo dmesg -C
sudo insmod monitor.ko
sudo ./engine supervisor ./rootfs-base &
sleep 2

# Test soft-limit
sudo ./engine start softdemo ./rootfs-alpha /memory_hog --soft-mib 32 --hard-mib 80
sleep 8
sudo dmesg | grep "container_monitor"  # Look for SOFT LIMIT message
sudo ./engine stop softdemo

# Test hard-limit
sudo ./engine start hardtest ./rootfs-alpha /memory_hog --soft-mib 50 --hard-mib 64
sleep 12  # Wait for it to be killed
sudo dmesg | grep "container_monitor"  # Look for HARD LIMIT message
sudo ./engine ps  # Should show exit_reason = hard_limit_killed
```

### Option B: Automated Test Script
```bash
cd boilerplate
chmod +x test_task5_task6.sh
sudo ./test_task5_task6.sh  # Runs complete test suite
```

---

## Expected Output

### Task 5 (Soft-Limit Warning)
You should see in dmesg:
```
[container_monitor] Registering container=softdemo pid=XXXXX soft=33554432 hard=83886080
[container_monitor] Registration SUCCESS container=softdemo pid=XXXXX
[container_monitor] SOFT LIMIT container=softdemo pid=XXXXX rss=35000000 limit=33554432
```

Container status in `engine ps`:
```
ID        PID    STATE    REASON
softdemo  XXXXX  running  -
```

### Task 6 (Hard-Limit Enforcement)
You should see in dmesg:
```
[container_monitor] Registering container=hardtest pid=YYYY soft=52428800 hard=67108864
[container_monitor] Registration SUCCESS container=hardtest pid=YYYY
[container_monitor] SOFT LIMIT container=hardtest pid=YYYY rss=60000000 limit=52428800
[container_monitor] HARD LIMIT container=hardtest pid=YYY rss=75000000 limit=67108864
```

Container auto-kill message from supervisor:
```
Container hardtest reaped (PID: YYYY, reason: hard_limit_killed)
```

Container status in `engine ps`:
```
ID        PID    STATE    REASON
hardtest  YYYY   stopped  hard_limit_killed
```

---

## Debugging: If Tests Still Fail

### Issue: No SOFT LIMIT message
**Check:**
1. `lsmod | grep monitor` - Is kernel module loaded?
2. `ls -l /dev/container_monitor` - Does the device exist?
3. `sudo dmesg | grep "Registration SUCCESS"` - Did containers register?

**If no SUCCESS messages:**
- Containers might be failing to register
- Check `/tmp/runtime.sock` is writable
- Check `/dev/container_monitor` device permissions
- Try: `sudo chmod 666 /dev/container_monitor`

### Issue: Container exits before limits trigger
**Increase the wait time:**
```bash
# Give more time for allocations to accumulate
sleep 15  # Instead of sleep 8-12
```

**Or reduce allocation rate:**
```bash
# Allocate more slowly to give timer callbacks time to detect
sudo ./engine start softdemo ./rootfs-alpha /memory_hog 4 500 --soft-mib 32 --hard-mib 80
#                                                        ↑ 4MB  ↑ every 500ms
```

### Issue: Zombie processes visible
**The hard-limit kill should clean up properly now.** If you still see zombies:
1. Don't manually stop containers while monitoring
2. Let the kernel kill them
3. The supervisor should reap them automatically

---

## Files Modified

1. **boilerplate/monitor.c**
   - Enhanced timer_callback() with better diagnostics
   - Enhanced monitor_ioctl() with registration logging

2. **boilerplate/memory_hog.c**
   - Changed exit behavior to indefinite sleep
   - Added startup message

## Files Created

1. **boilerplate/build_and_test.sh** - Helper script to rebuild
2. **boilerplate/test_task5_task6.sh** - Automated test suite

---

## Key Points

- All code changes focus on making the existing functionality work correctly
- No changes to the core logic (soft-limit detection, hard-limit killing)
- Changes ensure better visibility and prevent premature process exits
- The improvements make debugging much easier if issues arise

Run the test script to validate everything works before submitting evidence!

