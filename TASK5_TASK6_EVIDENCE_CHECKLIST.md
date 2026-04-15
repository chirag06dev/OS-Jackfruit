# Task 5 & Task 6 - Critical Evidence Requirements

## Summary: What Went Wrong & How to Fix It

### Task 5: Soft-Limit Warning
**Current Problem:** Showing timer debug messages instead of actual soft-limit event
**Root Cause:** The dmesg log shows generic monitoring activity, not the specific "[container_monitor] SOFT LIMIT" message

**Fix Required:**
```bash
# CORRECT: This is what container status should show during soft-limit warning
$ sudo dmesg | grep "container_monitor"
[12.345] [container_monitor] Registering container=softdemo pid=12345 soft=33554432 hard=83886080
[15.123] [container_monitor] SOFT LIMIT container=softdemo pid=12345 rss=35000000 limit=33554432
```

**Evidence Proof:**
- ✅ Message starts with "[container_monitor]"
- ✅ Contains "SOFT LIMIT" in text
- ✅ Shows container ID, PID, actual RSS
- ✅ Shows configured soft-limit
- ✅ Container still shows "running" state in `engine ps`

---

### Task 6: Hard-Limit Enforcement  
**Current Problem:** Showing manual stops instead of automatic kernel-enforced killing

**Root Cause:** 
1. Using `./engine stop` command instead of letting kernel kill the container
2. Missing the "[container_monitor] HARD LIMIT" message that proves kernel enforcement
3. Not showing the exit_reason transition to "hard_limit_killed"

**Fix Required:**

**In dmesg:**
```bash
$ sudo dmesg | grep "container_monitor"
[12.345] [container_monitor] Registering container=hardtest pid=23456 soft=52428800 hard=67108864
[18.234] [container_monitor] SOFT LIMIT container=hardtest pid=23456 rss=60000000 limit=52428800
[20.567] [container_monitor] HARD LIMIT container=hardtest pid=23456 rss=75000000 limit=67108864
```

**In engine.c output (automatic):**
```
Container hardtest reaped (PID: 23456, reason: hard_limit_killed)
```

**In engine ps output:**
```bash
$ sudo ./engine ps
ID             PID    STATE    ROOTFS          SOFT(MiB)  HARD(MiB)  REASON
hardtest       23456  stopped  ./rootfs-alpha  50         64         hard_limit_killed
```

**Evidence Proof - ALL of these must be visible:**
- ✅ "[container_monitor] HARD LIMIT" message in dmesg (NOT manual stop!)
- ✅ Shows container ID, PID, actual RSS exceeding hard-limit
- ✅ Container stdout shows memory allocations continuing until killed
- ✅ "Container X reaped (PID: Y, reason: hard_limit_killed)" printed
- ✅ `engine ps` shows REASON field = "hard_limit_killed"
- ✅ NO `./engine stop` command executed

---

## Source Code Evidence

### Line Reference: How reap_container Sets exit_reason

**File:** [boilerplate/engine.c](boilerplate/engine.c#L1193-L1195)
```c
else if (cont->term_signal == SIGKILL)
    safe_copy(cont->exit_reason, sizeof(cont->exit_reason), "hard_limit_killed");
```

**How This Works:**
1. Kernel monitor sends SIGKILL when hard-limit exceeded
2. Container process dies and sends SIGCHLD signal to supervisor
3. Supervisor catches signal and calls reap_container()
4. reap_container() checks: `WIFSIGNALED(status)` → yes (process was killed)
5. Checks: `WTERMSIG(status) == SIGKILL` → yes (killed by SIGKILL)
6. Sets: `exit_reason = "hard_limit_killed"`

**This is the ONLY way exit_reason becomes "hard_limit_killed"** - it proves automatic kernel enforcement!

---

### Line Reference: How kernel Enqueues Hard-Limit Message

**File:** [boilerplate/monitor.c](boilerplate/monitor.c#L185-L195)
```c
static void kill_process(const char *container_id,
                         pid_t pid,
                         unsigned long limit_bytes,
                         long rss_bytes)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        send_sig(SIGKILL, task, 1);
    rcu_read_unlock();

    enqueue_monitor_log("[container_monitor] HARD LIMIT container=%s pid=%d rss=%ld limit=%lu",
                        container_id,
                        pid,
                        rss_bytes,
                        limit_bytes);
}
```

**In timer_callback (line 221-223):**
```c
if (rss > entry->hard_limit_bytes) {
    kill_process(entry->container_id, entry->pid, entry->hard_limit_bytes, rss);
    // Process is now dead, gets reaped → exit_reason = "hard_limit_killed"
}
```

---

## What Evaluators Will Check

### Task 5 Checklist
- [ ] dmesg contains exact string: "[container_monitor] SOFT LIMIT"
- [ ] Message shows: container_id, pid, rss_bytes, limit_bytes
- [ ] Container remains "running" during warning (not killed)
- [ ] No manual commands like `./engine stop` visible
- [ ] rss_bytes value is GREATER than limit_bytes in the message
- [ ] Container continues allocating memory after warning

### Task 6 Checklist  
- [ ] dmesg contains exact string: "[container_monitor] HARD LIMIT"
- [ ] Message shows: container_id, pid, rss_bytes > hard-limit_bytes
- [ ] Container stdout shows memory allocations CONTINUING (not manual stop)
- [ ] Supervisor prints: "Container X reaped (PID: Y, reason: hard_limit_killed)"
- [ ] engine ps shows exit_reason = "hard_limit_killed"
- [ ] Container state = "stopped" or "exited" (NOT just from manual stop)
- [ ] NO manual `./engine stop` command in the sequence

---

## Quick Test Commands

### Verify Monitor Loading
```bash
cd boilerplate
sudo insmod monitor.ko
sudo dmesg | tail -5  # Should show registration messages
```

### Run Soft-Limit Test
```bash
cd boilerplate

# Terminal 1: Monitor logs
sudo dmesg -C && sudo dmesg -w

# Terminal 2: Run test
sudo ./engine supervisor ./rootfs-base &
sleep 2
sudo ./engine start softdemo ./rootfs-alpha /memory_hog --soft-mib 32 --hard-mib 80
sleep 8
sudo ./engine ps
# Should show: SOFT LIMIT message in Terminal 1, container still running
```

### Run Hard-Limit Test
```bash
cd boilerplate

# Terminal 1: Monitor logs
sudo dmesg -C && sudo dmesg -w

# Terminal 2: Run test
sudo ./engine supervisor ./rootfs-base &
sleep 2
sudo ./engine start hardtest ./rootfs-alpha /memory_hog --soft-mib 50 --hard-mib 64
# DO NOT RUN: sudo ./engine stop hardtest
# JUST WAIT FOR KERNEL TO KILL IT
sleep 12
sudo ./engine ps
# Should show: HARD LIMIT message in Terminal 1, exit_reason=hard_limit_killed
```

---

## Why Your Previous Evidence Was Wrong

### Task 5 Issues:
| Issue | Why It Failed | Evidence Required |
|-------|---|---|
| Only TIMER RUNNING messages | That's internal kernel debug output | "[container_monitor] SOFT LIMIT" exact string |
| No soft-limit message visible | Logs were mixed with unrelated noise | Clean dmesg showing warning moment |
| No container ID link | Could be any container | Container ID + matching `engine ps` output |
| No proof threshold crossed | Just showed monitoring active | rss_bytes value in the log message |

### Task 6 Issues:
| Issue | Why It Failed | Evidence Required |
|-------|---|---|
| Manual stops shown | Using `./engine stop beta` instead of auto-kill | Let kernel kill container, no manual stop |
| No HARD LIMIT message | Didn't show kernel's enforcement action | "[container_monitor] HARD LIMIT" exact string |
| Zombie processes | Improper cleanup after manual kill | Automatic cleanup after hard-limit kill |
| No exit_reason metadata | Didn't show the status after auto-kill | engine ps showing "hard_limit_killed" reason |
| Showed shutdown behavior | That's task about stopping containers | This is about automatic enforcement |

---

## Screenshot Organization Tips

### For Task 5: Create a 2-Panel Screenshot
```
┌─────────────────────────────────┬─────────────────────────────────┐
│  dmesg output (Terminal)        │  engine ps output              │
│                                 │                                │
│ [container_monitor] Registering │  ID         STATE   REASON    │
│ container=softdemo ...          │  softdemo   running  -         │
│ [container_monitor] SOFT LIMIT  │                                │
│ container=softdemo ...          │  (shows running during warning) │
└─────────────────────────────────┴─────────────────────────────────┘
+ Container stdout showing memory allocations continuing
```

### For Task 6: Create a 3-Part Screenshot
```
┌─────────────────────────────────────────────────────────────────────┐
│  Part 1: Container allocating memory (STDOUT)                      │
│                                                                     │
│  allocation=1 chunk=8MB total=8MB                                  │
│  allocation=2 chunk=8MB total=16MB                                 │
│  ...continues until killed...                                      │
└─────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────┐
│  Part 2: Kernel enforcement (dmesg)                                │
│                                                                     │
│  [container_monitor] Registering container=hardtest pid=23456      │
│  [container_monitor] HARD LIMIT container=hardtest pid=23456       │
│  rss=75000000 limit=67108864                                       │
└─────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────┐
│  Part 3: Metadata proof (engine ps)                                │
│                                                                     │
│  ID        STATE    REASON                                         │
│  hardtest  stopped  hard_limit_killed                              │
└─────────────────────────────────────────────────────────────────────┘
```

---

## Validation Command Sequence

```bash
#!/bin/bash
cd boilerplate

# Setup
sudo dmesg -C
sudo insmod monitor.ko
sudo ./engine supervisor ./rootfs-base &
SUPERVISOR_PID=$!
sleep 2

# TASK 5: Soft-limit
echo "=== TASK 5: SOFT-LIMIT TEST ==="
sudo dmesg -C
sudo ./engine start softdemo ./rootfs-alpha /memory_hog --soft-mib 32 --hard-mib 80 2>&1
sleep 1
echo "--- dmesg output (should show SOFT LIMIT) ---"
sudo dmesg | grep "container_monitor"
echo "--- engine ps output (should show running) ---"
sudo ./engine ps
echo ""

# Let it run a bit longer
sleep 6

echo "--- Final engine ps ---"
sudo ./engine ps
echo ""
sudo ./engine stop softdemo

# TASK 6: Hard-limit
echo "=== TASK 6: HARD-LIMIT TEST ==="
sudo dmesg -C
sudo ./engine start hardtest ./rootfs-alpha /memory_hog --soft-mib 50 --hard-mib 64 2>&1
# NO STOP COMMAND - LET KERNEL KILL IT
sleep 12
echo "--- dmesg output (should show HARD LIMIT and SOFT LIMIT) ---"
sudo dmesg | grep "container_monitor"
echo "--- engine ps output (should show hard_limit_killed) ---"
sudo ./engine ps

# Cleanup
kill $SUPERVISOR_PID 2>/dev/null || true
```

Run this, capture the output, and you'll have complete evidence for both tasks!

