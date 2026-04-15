# Code Structure Reference

## engine.c - Organization

### Global State (Lines 90-120)
```c
container_t containers[MAX_CONTAINERS];      // Array of container metadata
int container_count;                         // Current count
pthread_mutex_t container_lock;              // Protects access

log_buffer_t log_buffer;                     // Bounded-buffer for logging
```

### Bounded-Buffer Operations (Lines 130-210)
- `log_buffer_init()` - Initialize mutex, cond_vars
- `log_buffer_enqueue()` - Producer: add entry, wait if full
- `log_buffer_dequeue()` - Consumer: remove entry, wait if empty

### Logging Threads (Lines 220-350)
- `producer_thread()` - Reads container pipes, uses epoll, inserts into buffer
- `consumer_thread()` - Reads from buffer, writes to log files

### Container Lifecycle (Lines 360-500)
- `container_func()` - Entry point for cloned child
  - Redirects stdout/stderr to pipes
  - Mounts /proc
  - Executes command in chroot
- `start_container_impl()` - Creates pipes, clones child, registers with kernel
- `stop_container_impl()` - Sends SIGTERM to container
- `reap_container()` - Updates metadata after child exits

### CLI Commands (Lines 510-650)
- `parse_start_command()` - Parse command-line options (--soft-mib, --hard-mib, --nice)
- `cmd_start()`, `cmd_run()`, `cmd_ps()`, `cmd_logs()`, `cmd_stop()`

### Signal Handlers (Lines 660-680)
- `sigchld_handler()` - Reaps exited children via waitpid()
- `sigterm_handler()` - Sets graceful_shutdown flag

### CLI Client (Lines 690-710)
- `cli_send_command()` - Connects to socket, sends command, receives response

### Supervisor Loop (Lines 720-850)
- `run_supervisor()` - Main event loop
  - Creates Unix socket at /tmp/runtime.sock
  - Accepts CLI connections
  - Parses and executes commands
  - Handles SIGTERM: terminates containers, joins threads, cleanup

---

## monitor.c - Organization

### Data Structures (Lines 25-40)
```c
struct monitor_entry {
    pid_t pid;
    char container_id[64];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    bool soft_limit_exceeded;
    struct list_head list;
};

static LIST_HEAD(monitor_list);              // Linked-list of monitored PIDs
static DEFINE_MUTEX(monitor_lock);           // Protects list access
```

### Core Functions (Lines 50-200)
- `get_rss_bytes(pid_t pid)` - Reads RSS from task_struct
- `log_soft_limit_event()` - Prints warning to dmesg
- `kill_process()` - Sends SIGKILL to process, logs to dmesg
- `timer_callback()` - Called every 1 second
  - Iterates list, checks RSS against soft/hard limits
  - Logs warning on soft limit (once)
  - Kills on hard limit
  - Removes exited processes

### IOCTL Handler (Lines 210-280)
- `monitor_ioctl()`
  - MONITOR_REGISTER: allocate entry, add to list
  - MONITOR_UNREGISTER: find and remove from list

### Module Lifecycle (Lines 290-350)
- `monitor_init()` - Alloc chrdev region, create device, setup timer
- `monitor_exit()` - Shutdown timer, free all list entries, unregister device

---

## Synchronization Explained

### Container Lock (engine.c)
```c
pthread_mutex_t container_lock;

// In start_container_impl():
pthread_mutex_lock(&container_lock);
container_count++;
containers[idx].pid = pid;
pthread_mutex_unlock(&container_lock);
```
**Why:** Prevents concurrent modification of container array while iterating/updating

### Log Buffer Lock + Condition Vars (engine.c)
```c
typedef struct {
    log_entry_t entries[MAX_LOG_ENTRIES];
    int head, tail, count;
    pthread_mutex_t lock;
    pthread_cond_t not_full;      // Signaled when space available
    pthread_cond_t not_empty;     // Signaled when data available
} log_buffer_t;

// Producer:
while (buf->count >= MAX_LOG_ENTRIES && !buf->shutdown) {
    pthread_cond_wait(&buf->not_full, &buf->lock);  // Release lock, wait
}                                                    // Reacquire upon wake
buffer[head] = data;
buf->head = (buf->head + 1) % MAX_LOG_ENTRIES;
buf->count++;
pthread_cond_signal(&buf->not_empty);
```
**Why:** Ensures no data loss, no deadlock, efficient wait/wake

### Memory Monitoring Lock (monitor.c)
```c
static DEFINE_MUTEX(monitor_lock);

// In timer_callback():
mutex_lock(&monitor_lock);
list_for_each_entry_safe(entry, tmp, &monitor_list, list) {
    // Check RSS, kill if needed
}
mutex_unlock(&monitor_lock);
```
**Why:** Protects linked-list from concurrent register/unregister IOCTL calls

---

##IPC Paths

### Path A: Logging (Container → Logs)
```
Container (child process)
    ↓
   [stdout_pipe[1], stderr_pipe[1]] (write ends)
    ↓
Parent keeps [stdout_pipe[0], stderr_pipe[0]] (read ends)
    ↓
producer_thread() (epoll)
    ↓
log_buffer (mutex + cond_vars)
    ↓
consumer_thread()
    ↓
logs/container_id.log
```

### Path B: Control (CLI → Commands)
```
CLI client (separate process)
    ↓
connect() to /tmp/runtime.sock (Unix domain socket)
    ↓
write() command string
    ↓
supervisor (main thread, blocking accept() in socket loop)
    ↓
parse + execute command (cmd_start, cmd_ps, etc.)
    ↓
write() response
    ↓
CLI client reads response
```

---

## Key Compile-Time Constants

| Constant | Value | Purpose |
|----------|-------|---------|
| MAX_CONTAINERS | 10 | Max running containers |
| BUFFER_SIZE | 4096 | Bytes per log buffer entry |
| MAX_LOG_ENTRIES | 1000 | Circular buffer capacity |
| CHECK_INTERVAL_SEC | 1 | Kernel timer period (seconds) |
| STACK_SIZE | 1MB | Clone stack per container |
| SOCKET_PATH | /tmp/runtime.sock | CLI IPC socket |
| LOG_DIR | logs | Directory for container logs |
| DEFAULT_SOFT_MIB | 40 | Default soft limit |
| DEFAULT_HARD_MIB | 64 | Default hard limit |

---

## Testing Workflow

```bash
# Terminal 1: Supervisor
sudo ./engine supervisor ./rootfs-base

# Terminal 2: Start containers
sudo ./engine start c1 ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start c2 ./rootfs-beta /bin/sh --soft-mib 64 --hard-mib 96

# Terminal 3: Monitor
sudo ./engine ps                    # See metadata
sudo ./engine logs c1               # View captured output
dmesg | grep container_monitor      # See kernel warnings/kills

# Terminal 4: Trigger limits
# Inside container c1:
# /memory_hog 2 500  → allocates 2MB chunks
# Watch dmesg for soft/hard limit messages

# Terminal 2: Manage lifecycle
sudo ./engine stop c1
sudo ./engine ps
```

---

## Common Debugging

### Supervisor Hang
- Check if another supervisor on /tmp/runtime.sock: `lsof /tmp/runtime.sock`
- Kill stale: `pkill -f "engine supervisor"`
- Remove socket: `rm /tmp/runtime.sock`

### No Logs Captured
- Check logs directory created: `ls -la logs/`
- Check permissions: `ls -la logs/*.log`
- Check producer thread started: Look for "[producer] Thread started" in supervisor output

### Module Load Fails
- Check prerequisites: `uname -r` (should show Linux kernel)
- Verify Secure Boot OFF: `mokutil --sb-state`
- Check module exists: `ls -la *.ko`

### Memory Limit Not Enforced
- Check module loaded: `lsmod | grep monitor`
- Check device created: `ls -la /dev/container_monitor`
- Check kernel messages: `dmesg | tail`
- Verify registration: Look for "[container_monitor] Registering" in dmesg

---

## Reference Code Snippets

### Clone with Namespaces
```c
pid_t pid = clone(container_func,
                  stack[idx] + STACK_SIZE,
                  CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                  &child_args);
```

### SIGCHLD Handler
```c
void sigchld_handler(int sig) {
    pid_t pid;
    int status;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        reap_container(pid, status);
    }
}
```

### Epoll Setup
```c
int epfd = epoll_create1(0);
struct epoll_event ev;
ev.events = EPOLLIN;
ev.data.u64 = container_idx;
epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
```

### IOCTL Registration (from supervisor)
```c
struct monitor_request req;
req.pid = pid;
req.soft_limit_bytes = cont->soft_limit_bytes;
req.hard_limit_bytes = cont->hard_limit_bytes;
strncpy(req.container_id, id, sizeof(req.container_id) - 1);
ioctl(cont->monitor_fd, MONITOR_REGISTER, &req);
```

---

