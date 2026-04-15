# Multi-Container Runtime with Kernel Memory Monitor

## Team Information
- Name 1: Harshit Chandak
- SRN 1: PES2UG24CS185
- Name 2: Hamza Shabbir Sahapurwala
- SRN 2: PES2UG24CS177

## Environment
- Ubuntu 22.04 or 24.04 VM
- Secure Boot OFF
- Linux kernel headers installed

## Build and Run

### 1. Build
```bash
cd boilerplate
make clean
make
```

### 2. Prepare root filesystems
```bash
cd boilerplate
mkdir -p rootfs-base
wget -nc https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
cp -a rootfs-base rootfs-gamma

cp memory_hog rootfs-alpha/
cp memory_hog rootfs-beta/
cp cpu_hog rootfs-alpha/
cp cpu_hog rootfs-beta/
cp io_pulse rootfs-gamma/
```

### 3. Load monitor module
```bash
cd boilerplate
sudo insmod monitor.ko
ls -l /dev/container_monitor
```

`engine start` and `engine run` now fail fast if registration with `/dev/container_monitor` does not succeed, so load the module before launching containers.

### 4. Start supervisor
```bash
cd boilerplate
sudo ./engine supervisor ./rootfs-base
```

### 5. CLI usage (second terminal)
```bash
cd boilerplate
sudo ./engine start alpha ./rootfs-alpha /memory_hog --soft-mib 32 --hard-mib 80
sudo ./engine start beta  ./rootfs-beta  /cpu_hog --soft-mib 64 --hard-mib 128 --nice 10
sudo ./engine ps
sudo ./engine logs alpha
sudo ./engine stop beta
sudo ./engine klist
sudo ./engine schedtest hi ./rootfs-alpha /cpu_hog -5 lo ./rootfs-beta /cpu_hog 10
```

If the container command itself needs `--flags`, separate runtime flags from command flags with `--`, for example:
```bash
sudo ./engine run shelltest ./rootfs-alpha -- /bin/sh -c "echo ready"
```

## Deliverable Proof Matrix

### 1. Multi-container supervision (required two or more at once)
Run both starts before either exits:
```bash
sudo ./engine start alpha ./rootfs-alpha /memory_hog --soft-mib 48 --hard-mib 128
sudo ./engine start beta  ./rootfs-beta  /cpu_hog --soft-mib 64 --hard-mib 128
sudo ./engine ps
```

Expected `ps` shows two running containers under one supervisor.

Screenshot required:
- both alpha and beta present together in one `ps` output

### 2. Metadata tracking in single correlated view
`engine ps` prints ID, PID, STATE, ROOTFS, SOFT/HARD limits, and REASON in one table.

Screenshot required:
- one `ps` output where ID, PID, rootfs path, limits, and reason are visible together

### 3. Bounded-buffer logging pipeline
Implemented in user space (not kernel printk path):
- producer thread reads container stdout/stderr pipes via epoll
- bounded queue with mutex + condition variables
- consumer thread writes per-container logs in logs/<id>.log

Kernel monitor also uses a bounded ring queue for events:
- timer / ioctl paths enqueue monitor events
- dedicated kernel consumer thread dequeues and emits dmesg lines
- avoids direct burst printk from timer path

Proof commands:
```bash
sudo ./engine supervisor ./rootfs-base
# in another terminal
sudo ./engine start alpha ./rootfs-alpha /cpu_hog
sleep 2
sudo ./engine logs alpha
ls -l logs/alpha.log
```

Screenshot required:
- supervisor output showing producer/consumer threads active
- log file content generated via `engine logs alpha`

### 4. CLI and IPC with running supervisor
Control path is Unix domain socket at /tmp/runtime.sock.
Each CLI command is a client request to the running supervisor.

Extra IPC-backed commands:
- `engine klist` retrieves kernel metadata snapshot through supervisor socket + MONITOR_LIST ioctl
- `engine schedtest ...` runs an in-supervisor scheduling comparison and returns elapsed-time summary

Proof commands:
```bash
sudo ./engine supervisor ./rootfs-base
# separate terminal
sudo ./engine ps
sudo ./engine start gamma ./rootfs-gamma /io_pulse
sudo ./engine ps
```

Screenshot required:
- supervisor terminal receiving command handling
- CLI terminal showing command result

### 5. Soft-limit warning evidence
Use workload that gradually allocates memory so soft limit triggers before hard limit:
```bash
sudo ./engine start softdemo ./rootfs-alpha /memory_hog --soft-mib 24 --hard-mib 128
sleep 3
dmesg | tail -n 50 | grep "SOFT LIMIT"
```

Expected kernel line contains:
- SOFT LIMIT container=softdemo ...

Screenshot required:
- dmesg output with soft-limit warning

### 6. Hard-limit enforcement evidence
```bash
sudo ./engine start harddemo ./rootfs-beta /memory_hog --soft-mib 16 --hard-mib 32
sleep 5
dmesg | tail -n 80 | grep "HARD LIMIT"
sudo ./engine ps
```

Expected:
- HARD LIMIT dmesg event
- `ps` reason for harddemo shown as hard_limit_killed

Screenshot required:
- both dmesg hard-limit line and `engine ps` reason column

### 7. Scheduling experiment (required)

Experiment A: CPU vs CPU with different nice values
```bash
time sudo ./engine run hi ./rootfs-alpha /cpu_hog --nice -5 --soft-mib 64 --hard-mib 256
time sudo ./engine run lo ./rootfs-beta  /cpu_hog --nice 10 --soft-mib 64 --hard-mib 256
```

Experiment B: CPU vs IO concurrently
```bash
sudo ./engine start cpubox ./rootfs-alpha /cpu_hog --nice 0
sudo ./engine start iobox  ./rootfs-gamma /io_pulse --nice 0
sleep 4
sudo ./engine logs cpubox
sudo ./engine logs iobox
```

Programmatic comparison command:
```bash
sudo ./engine schedtest hi ./rootfs-alpha /cpu_hog -5 lo ./rootfs-beta /cpu_hog 10
```

Report table template:

| Experiment | Workloads | Config | Metric | Observation |
|---|---|---|---|---|
| A | cpu_hog vs cpu_hog | nice -5 vs +10 | completion time | TODO |
| B | cpu_hog vs io_pulse | same nice | responsiveness/log cadence | TODO |

Screenshot required:
- terminal output with timings and/or side-by-side logs showing behavior difference

### 8. Clean teardown and zombie verification
```bash
# stop active containers first
sudo ./engine stop alpha
sudo ./engine stop beta

# stop supervisor
sudo pkill -SIGTERM -f "./engine supervisor"

# verify no zombies / stale runtime
ps aux | grep "[d]efunct"
ps aux | grep "[e]ngine supervisor"
ls -l /tmp/runtime.sock

# unload module
sudo rmmod monitor
dmesg | tail -n 20
```

Expected:
- no defunct container children
- supervisor exits
- socket removed
- module unload log appears

Kernel metadata snapshot check before teardown:
```bash
sudo ./engine klist
```

Screenshot required:
- output proving no defunct processes after teardown

## Engineering Analysis

### Isolation mechanisms
Containers use CLONE_NEWPID, CLONE_NEWUTS, CLONE_NEWNS and chroot. PID namespace isolates process IDs; mount namespace isolates mounts; UTS isolates hostname. Kernel remains shared across containers.

### Supervisor and lifecycle
A long-running supervisor centralizes lifecycle and metadata. It starts containers, receives CLI control requests, reaps children via SIGCHLD + waitpid(WNOHANG), and records terminal reason.

### IPC, threads, synchronization
Two IPC paths are used:
- control: Unix socket between CLI and supervisor
- logging: pipes from child stdout/stderr into producer, then bounded buffer to consumer

Mutex and condition variables protect queue state and avoid races on full/empty transitions.

### Memory enforcement
RSS is checked in kernel timer callback. Soft limit logs warning once on crossing, hard limit sends SIGKILL. Kernel-space enforcement is reliable and not bypassable from container process context.

### Scheduling behavior
Different nice values alter scheduler share for CPU-bound tasks. Mixed CPU/IO workloads demonstrate responsiveness differences due to blocking and CFS fairness.

## Design Decisions and Tradeoffs

1. Chroot instead of pivot_root
- Simpler integration and easier debugging
- Tradeoff: weaker isolation semantics than full pivot_root flow

2. Epoll-based producer for log pipes
- Scales better than per-pipe blocking reads
- Tradeoff: more event-loop complexity

3. Kernel linked-list monitor with timer polling
- Straightforward periodic policy enforcement
- Tradeoff: coarse 1-second detection granularity

4. Socket-based control plane
- Decouples short-lived CLI from long-lived supervisor
- Tradeoff: command parsing and protocol framing overhead

## Code Map
- User-space runtime: boilerplate/engine.c
- Kernel monitor module: boilerplate/monitor.c
- Shared ioctl header: boilerplate/monitor_ioctl.h
- Workloads: boilerplate/cpu_hog.c, boilerplate/io_pulse.c, boilerplate/memory_hog.c
- Build: boilerplate/Makefile

## Cleanup Commands
```bash
cd boilerplate
sudo rmmod monitor || true
make clean
```
