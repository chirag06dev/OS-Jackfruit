# Quick Reference: Task 5 vs Task 6

## Side-by-Side Comparison

### TASK 5: Soft-Limit Warning

**What Happens:**
- Container memory gradually increases
- When RSS > soft-limit for FIRST TIME: kernel logs "[container_monitor] SOFT LIMIT ..." message
- Container **CONTINUES RUNNING** (no kill)
- If memory drops back below soft-limit: warning flag resets

**In Code (monitor.c line 209-216):**
```c
if (rss > entry->soft_limit_bytes) {
    if (!entry->soft_limit_exceeded) {  // Only log once per crossing
        log_soft_limit_event(...);      // Enqueues message
        entry->soft_limit_exceeded = true;
    }
}
```

**Expected dmesg Output:**
```
[container_monitor] SOFT LIMIT container=softdemo pid=12345 rss=35000000 limit=33554432
      ↑                ↑                                  ↑              ↑
   Exact prefix    Container ID                       Actual RSS    Configured limit
```

**Expected engine ps Output:**
```
ID        PID   STATE      REASON
softdemo  12345 running    -
          ↑             ↑
      Still alive      No reason = no error
```

**Proof Checklist:**
- [ ] dmesg has "[container_monitor] SOFT LIMIT"
- [ ] Container ID visible in message
- [ ] RSS value shown is > configured soft-limit
- [ ] engine ps shows STATE = "running" (not stopped/exited)
- [ ] No manual ./engine stop command executed
- [ ] Container continues allocating memory

---

### TASK 6: Hard-Limit Enforcement

**What Happens:**
- Container memory gradually increases
- When RSS > hard-limit: kernel logs "[container_monitor] HARD LIMIT ..." message
- IMMEDIATELY sends SIGKILL to container process
- Container **DIES AUTOMATICALLY** (no ./engine stop needed!)
- Supervisor reaps the dead process, sets exit_reason = "hard_limit_killed"

**In Code (monitor.c line 221-223):**
```c
if (rss > entry->hard_limit_bytes) {
    kill_process(...);              // Sends SIGKILL
    list_del(&entry->list);         // Removes from monitoring
}

// In kill_process():
send_sig(SIGKILL, task, 1);         // Process dies
enqueue_monitor_log("[HARD LIMIT]..."); // Logs event
```

**In Code (engine.c line 1194-1195):**
```c
else if (cont->term_signal == SIGKILL)
    safe_copy(cont->exit_reason, sizeof(cont->exit_reason), "hard_limit_killed");
     ↑
     This only happens when killed by kernel's SIGKILL
```

**Expected dmesg Output:**
```
[container_monitor] HARD LIMIT container=hardtest pid=23456 rss=75000000 limit=67108864
      ↑               ↑                                  ↑              ↑
   Exact prefix   HARD (not SOFT)                 RSS > hard limit  Configured limit
```

**Expected engine.c stdout Output:**
```
Container hardtest reaped (PID: 23456, reason: hard_limit_killed)
                                                    ↑
                                     This proves SIGKILL from kernel
```

**Expected engine ps Output:**
```
ID        PID   STATE    REASON
hardtest  23456 stopped  hard_limit_killed
          ↑          ↑    ↑
      Is here    No longer running  Automatic kill reason
```

**Proof Checklist:**
- [ ] dmesg has "[container_monitor] HARD LIMIT" (not manual stop!)
- [ ] Container ID visible in message
- [ ] RSS value shown is > configured hard-limit
- [ ] Container stdout shows memory allocations were HAPPENING (not manual stop)
- [ ] NO "./engine stop hardtest" command visible in sequence
- [ ] Supervisor outputs "Container X reaped (... reason: hard_limit_killed)"
- [ ] engine ps shows REASON = "hard_limit_killed"

---

## Critical Differences

| Aspect | Soft-Limit | Hard-Limit |
|--------|-----------|-----------|
| **dmesg Text** | "[container_monitor] SOFT LIMIT" | "[container_monitor] HARD LIMIT" |
| **Container Action** | Logs warning & continues | Receives SIGKILL & dies |
| **How It Dies** | Manual stop or by exiting normally | Automatically killed by kernel |
| **exit_reason** | Default "-" or "stopped" | **ALWAYS "hard_limit_killed"** |
| **What Proves It** | Message + running state | Message + SIGKILL signal + exit_reason |
| **You Must NOT Do** | (can use ./engine stop later) | **DO NOT use ./engine stop!** |

---

## One-Line Test Summary

### Task 5 One-Liner
```bash
# Watch: Should see SOFT LIMIT message, then 'engine ps' shows RUNNING
( sudo dmesg -C & sudo dmesg -w ) & sleep 0.5 && \
sudo ./engine start softdemo ./rootfs-alpha /memory_hog --soft-mib 32 --hard-mib 80 && \
sleep 8 && sudo ./engine ps
```

### Task 6 One-Liner  
```bash
# Watch: Should see HARD LIMIT message, then 'engine ps' shows hard_limit_killed
( sudo dmesg -C & sudo dmesg -w ) & sleep 0.5 && \
sudo ./engine start hardtest ./rootfs-alpha /memory_hog --soft-mib 50 --hard-mib 64 && \
sleep 12 && sudo ./engine ps
```

---

## Screenshot Filter (Remove Noise)

**Before (Too Much Noise):**
```
[102.345] AppArmor: some_event
[102.456] Firefox: something
[102.567] [container_monitor] SOFT LIMIT container=softdemo ...
[102.678] random kernel noise
[102.789] More AppArmor stuff
```

**After (Clean):**
```
[102.567] [container_monitor] SOFT LIMIT container=softdemo pid=12345 rss=35000000 limit=33554432
```

**How to Filter:**
```bash
# Only show container_monitor messages
sudo dmesg | grep "container_monitor"

# Or redirect to file, then edit
sudo dmesg > /tmp/dmesg.txt
# Then copy only relevant lines to screenshot
```

---

## Final Validation Before Submission

### For BOTH tasks:
- [ ] NO unrelated system messages visible (AppArmor, Firefox, etc.)
- [ ] Exact container ID shown in dmesg matches engine ps
- [ ] Container PID shown in dmesg matches engine ps
- [ ] Memory values in dmesg are in bytes (not MiB)
- [ ] Limits shown in dmesg match what was configured

### ONLY for Task 5:
- [ ] Container clearly in "running" state
- [ ] SOFT LIMIT text visible (not HARD LIMIT)

### ONLY for Task 6:
- [ ] Container clearly in "stopped" state with "hard_limit_killed" reason
- [ ] HARD LIMIT text visible (not SOFT LIMIT)
- [ ] No "./engine stop" command in the sequence

---

## If Evidence Still Doesn't Show...

### For Task 5 (Soft-limit warning not appearing):
1. Check: `lsmod | grep monitor` - module loaded?
2. Check: `sudo dmesg | grep "Registering"` - containers registering?
3. Issue: Soft-limit too close to hard-limit
   - Solution: Use `--soft-mib 32 --hard-mib 80` (larger gap)
4. Issue: memory_hog finishes before warning triggers
   - Solution: Use slow allocation: `/memory_hog 4 500` (4MB per 500ms)

### For Task 6 (Hard-limit kill not showing):
1. Check: Same as Task 5 above
2. Check: Wait long enough for container to reach limit
   - 64 MiB at 8 MiB/sec = ~8 seconds, so wait 10+ seconds
3. Issue: Manual stop command interfering
   - Solution: DO NOT run ./engine stop; let it auto-kill
4. Issue: Container output not showing allocations
   - Solution: Redirect stdout: `sudo ./engine start hardtest ... 2>&1 | tee /tmp/output.txt`

