# Task 5 & Task 6 Evidence Collection Guide

## Critical Points

### Task 5 Issues with Current Evidence
- ❌ dmesg only shows repeated "TIMER RUNNING" debug messages
- ❌ No explicit "[container_monitor] SOFT LIMIT" message visible
- ❌ No proof that memory threshold was actually crossed
- ❌ No link between the warning and specific container (softdemo)
- ❌ Unrelated system noise (AppArmor, Firefox) obscures the actual event

### Task 6 Issues with Current Evidence
- ❌ Shows manual stops (`./engine stop alpha`) instead of automatic killing
- ❌ No "[container_monitor] HARD LIMIT" message in dmesg
- ❌ Zombie processes visible (<defunct>) indicates improper cleanup
- ❌ No exit_reason metadata showing "hard_limit_killed"
- ❌ Evidence shows shutdown behavior, not hard-limit enforcement

---

## Task 5: Soft-Limit Warning (CORRECT PROCEDURE)

### What Should Happen
When a container's RSS exceeds soft-limit but stays below hard-limit:
1. Kernel timer fires every 1 second (CHECK_INTERVAL_SEC)
2. Monitor checks container RSS against soft_limit_bytes
3. First time exceeding: logs `[container_monitor] SOFT LIMIT container=<id> pid=<pid> rss=<bytes> limit=<bytes>` to dmesg
4. Container **continues running** (not killed)
5. If memory drops back below soft-limit, warning resets

### Expected dmesg Output
```
[container_monitor] Registering container=softdemo pid=12345 soft=33554432 hard=83886080
[container_monitor] SOFT LIMIT container=softdemo pid=12345 rss=35000000 limit=33554432
```

### Testing Procedure

#### Terminal 1: Clear and monitor kernel logs
```bash
# Clear dmesg buffer first
sudo dmesg -C

# Watch for new messages
sudo dmesg -w
```

#### Terminal 2: Setup and run container
```bash
cd boilerplate

# Ensure monitor module is loaded
sudo insmod monitor.ko 2>/dev/null || true

# Start supervisor in background
sudo ./engine supervisor ./rootfs-base &
SUPERVISOR_PID=$!
sleep 2

# Start container with soft-limit that will be exceeded
# memory_hog allocates 8 MiB per second by default
# soft-limit: 32 MiB (will exceed in ~4 seconds)
# hard-limit: 80 MiB (should NOT kill container)
sudo ./engine start softdemo ./rootfs-alpha /memory_hog --soft-mib 32 --hard-mib 80

# Wait for memory to exceed soft-limit (watch Terminal 1 dmesg)
sleep 6

# Check container is still running
sudo ./engine ps

# Let it run a bit longer, then stop
sleep 2
sudo ./engine stop softdemo

# Cleanup
kill $SUPERVISOR_PID 2>/dev/null || true
```

### Screenshot Requirements for Task 5
Show a single screenshot divided into two halves or a captured sequence showing:

**Half 1: dmesg output** (from Terminal 1)
```
[container_monitor] Registering container=softdemo pid=<actual_pid> soft=33554432 hard=83886080
[container_monitor] SOFT LIMIT container=softdemo pid=<actual_pid> rss=35000000 limit=33554432
```
✅ MUST show: Container ID, PID, actual RSS value, configured limit
✅ MUST show the exact "[container_monitor]" prefix
✅ NO unrelated noise (AppArmor, Firefox, etc.)

**Half 2: engine ps output** (from Terminal 2)
```
ID             PID    STATE    ROOTFS          SOFT(MiB)  HARD(MiB)  REASON
softdemo       12345  running  ./rootfs-alpha  32         80         -
```
✅ MUST show: Container still in "running" state during soft-limit warning
✅ MUST NOT show "hard_limit_killed" reason

---

## Task 6: Hard-Limit Enforcement (CORRECT PROCEDURE)

### What Should Happen
When a container's RSS exceeds hard-limit_bytes:
1. Kernel timer fires every 1 second
2. Monitor checks container RSS against hard_limit_bytes
3. If exceeded: 
   - Logs `[container_monitor] HARD LIMIT container=<id> pid=<pid> rss=<bytes> limit=<bytes>` to dmesg
   - Sends SIGKILL to container process
   - Removes from monitoring list
4. Container is **automatically killed** (not manual stop!)
5. engine ps shows exit_reason as "hard_limit_killed" or similar

### Expected dmesg Output
```
[container_monitor] Registering container=hardtest pid=23456 soft=50000000 hard=67108864
[container_monitor] HARD LIMIT container=hardtest pid=23456 rss=75000000 limit=67108864
```

### Testing Procedure

#### Terminal 1: Monitor kernel logs
```bash
# Clear dmesg
sudo dmesg -C

# Watch for messages
sudo dmesg -w
```

#### Terminal 2: Run container that will exceed hard-limit
```bash
cd boilerplate

# Ensure monitor is loaded
sudo insmod monitor.ko 2>/dev/null || true

# Start supervisor in background
sudo ./engine supervisor ./rootfs-base &
SUPERVISOR_PID=$!
sleep 2

# Start container with hard-limit that will be exceeded
# soft-limit: 50 MiB (early warning)
# hard-limit: 64 MiB (will be exceeded around 8-9 seconds)
# memory_hog: 8 MiB per second → 64 MiB in ~8 seconds
sudo ./engine start hardtest ./rootfs-alpha /memory_hog --soft-mib 50 --hard-mib 64

# DON'T stop it manually! Let it exceed hard-limit and get killed by kernel
# Watch Terminal 1 for the HARD LIMIT message

# Wait ~10 seconds for memory to exceed hard-limit
sleep 10

# Check final state - should show exit_reason or similar
sudo ./engine ps

# Verify container was killed automatically:
sudo ./engine logs hardtest | tail -5

# Cleanup
kill $SUPERVISOR_PID 2>/dev/null || true
```

### Screenshot Requirements for Task 6
Show evidence of AUTOMATIC hard-limit enforcement:

**Part 1: dmesg output** (MUST show the HARD LIMIT line)
```
[container_monitor] Registering container=hardtest pid=23456 soft=52428800 hard=67108864
[container_monitor] SOFT LIMIT container=hardtest pid=23456 rss=60000000 limit=52428800
[container_monitor] HARD LIMIT container=hardtest pid=23456 rss=75000000 limit=67108864
```
✅ MUST show: "[container_monitor] HARD LIMIT" message with container id and pid
✅ MUST show: Actual RSS exceeding hard-limit
✅ NO manual "./engine stop" command visible!
✅ NO unrelated system noise

**Part 2: engine ps output AFTER container was auto-killed**
```
ID             PID    STATE    ROOTFS          SOFT(MiB)  HARD(MiB)  REASON
hardtest       23456  exited   ./rootfs-alpha  50         64         hard_limit_killed
```
✅ MUST show: STATE is "exited" or similar
✅ MUST show: REASON indicates "hard_limit_killed" or "SIGKILL" or similar
✅ MUST show: Container WAS running before the automatic kill

**Part 3: Container output (optional but helpful)**
```
allocation=1 chunk=8MB total=8MB
allocation=2 chunk=8MB total=16MB
allocation=3 chunk=8MB total=24MB
allocation=4 chunk=8MB total=32MB
allocation=5 chunk=8MB total=40MB
allocation=6 chunk=8MB total=48MB
allocation=7 chunk=8MB total=56MB
allocation=8 chunk=8MB total=64MB
allocation=9 chunk=8MB total=72MB
[process killed by SIGKILL - no more output]
```
✅ Shows memory allocations continuing until hard-limit hit
✅ Proves container was running and actively allocating

---

## Key Differences: Soft vs Hard

| Feature | Soft Limit | Hard Limit |
|---------|-----------|-----------|
| **dmesg message** | `[container_monitor] SOFT LIMIT ...` | `[container_monitor] HARD LIMIT ...` |
| **Container action** | Continues running | Receives SIGKILL |
| **In dmesg file** | Once per threshold cross | Only when hard-limit exceeded |
| **In engine ps** | REASON stays "-" | REASON shows "hard_limit_killed" |
| **Proof needed** | Message + running container | Message + auto-kill + exit_reason |

---

## Debugging: If Limits Don't Trigger

### Check 1: Monitor module loaded?
```bash
lsmod | grep monitor
# Should see: monitor                 ...
```

### Check 2: Monitor listening?
```bash
ls -l /dev/container_monitor
# Should show: crw-rw-rw- 1 root root ... /dev/container_monitor
```

### Check 3: Check dmesg for registration
```bash
sudo dmesg | grep -i "container_monitor"
# Should see registration messages
```

### Check 4: Manual RSS check
```bash
# Get container PID from engine ps
CONTAINER_PID=<pid>

# Check actual RSS
cat /proc/$CONTAINER_PID/status | grep VmRSS
```

### Check 5: Module debug output
```bash
# Rebuild with debug output if needed
# In monitor.c, look for timer_callback function
# Verify it's being called every second
```

---

## Common Issues and Fixes

### Issue: Still seeing TIMER RUNNING debug messages
**Fix:** Filter dmesg output or rebuild without debug prints. The assignment expects clean "[container_monitor]" prefixed messages, not internal timer debug output.

### Issue: Container gets killed before warning
**Fix:** Increase soft-limit closer to hard-limit so you see the soft-limit warning first:
```bash
--soft-mib 48 --hard-mib 64  # Will warn at 48 MiB, kill at 64 MiB
```

### Issue: memory_hog finishes before limits trigger
**Fix:** Reduce chunk size in memory_hog call:
```bash
/memory_hog 4 500  # 4 MiB per 500ms = 8 MiB per second (slower)
/memory_hog 2 200  # 2 MiB per 200ms = 10 MiB per second (more gradual)
```

### Issue: Zombie processes visible
**Fix:** Ensure proper process reaping in engine. Don't let containers sit in <defunct> state - stop cleanly or let hard-limit kill them.

---

## Validation Checklist

### Task 5 Screenshot Validation
- [ ] dmesg shows "[container_monitor] SOFT LIMIT" message
- [ ] Message includes: container_id, pid, rss value, configured soft limit
- [ ] Container ID matches the one you started
- [ ] engine ps shows container in "running" state, not exited
- [ ] No manual stop command visible in the sequence
- [ ] No unrelated system messages obscuring the event

### Task 6 Screenshot Validation
- [ ] dmesg shows "[container_monitor] HARD LIMIT" message
- [ ] Message includes: container_id, pid, rss value > hard-limit
- [ ] engine ps shows container in "exited" or "stopped" state
- [ ] exit_reason field shows "hard_limit_killed" or similar
- [ ] No "/engine stop" command visible (automatic kill only)
- [ ] Container output shows allocations until killed
- [ ] No zombie processes visible

---

## Timeline Example

```
T=0s   : Start container, soft=50 MiB, hard=64 MiB
T=1s   : memory = 8 MiB (below soft, no warning)
T=2s   : memory = 16 MiB (below soft, no warning)
T=3s   : memory = 24 MiB (below soft, no warning)
T=4s   : memory = 32 MiB (below soft, no warning)
T=5s   : memory = 40 MiB (below soft, no warning)
T=6s   : memory = 48 MiB (below soft, no warning)
T=7s   : memory = 56 MiB (above soft!) → dmesg: "[container_monitor] SOFT LIMIT ..."
T=8s   : memory = 64 MiB (at hard limit edge, still running)
T=9s   : memory = 72 MiB (above hard!) → dmesg: "[container_monitor] HARD LIMIT ..."
         → SIGKILL sent immediately
         → engine ps shows "exited" with reason "hard_limit_killed"
```

