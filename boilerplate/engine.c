#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define MAX_CONTAINERS 32
#define SOCKET_PATH "/tmp/runtime.sock"
#define LOG_DIR "logs"
#define BUFFER_SIZE 4096
#define MAX_LOG_ENTRIES 1000
#define MAX_COMMAND_LEN 512
#define CLIENT_BUFFER_SIZE 2048
#define RESPONSE_BUFFER_SIZE 16384
#define DEFAULT_SOFT_MIB 40UL
#define DEFAULT_HARD_MIB 64UL
#define MIB_TO_BYTES(mib) ((unsigned long)(mib) * 1024UL * 1024UL)

enum pipe_kind {
    PIPE_STDOUT = 1,
    PIPE_STDERR = 2,
};

typedef struct {
    char container_id[MONITOR_NAME_LEN];
    char data[BUFFER_SIZE];
    size_t len;
    time_t timestamp;
} log_entry_t;

typedef struct {
    log_entry_t entries[MAX_LOG_ENTRIES];
    int head;
    int tail;
    int count;
    int shutdown;
    pthread_mutex_t lock;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
} log_buffer_t;

typedef struct {
    int in_use;
    char id[MONITOR_NAME_LEN];
    char rootfs_path[PATH_MAX];
    char command[MAX_COMMAND_LEN];
    pid_t pid;
    char state[16];
    time_t start_time;
    time_t end_time;
    int exit_code;
    int term_signal;
    char exit_reason[64];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
    char log_path[PATH_MAX];
    int stdout_fd;
    int stderr_fd;
    int stdout_registered;
    int stderr_registered;
    int stop_requested;
    int monitor_fd;
} container_t;

typedef struct {
    char id[MONITOR_NAME_LEN];
    char rootfs[PATH_MAX];
    char command[MAX_COMMAND_LEN];
    int stdout_fd;
    int stderr_fd;
    int nice_value;
} container_args_t;

typedef struct {
    char id[MONITOR_NAME_LEN];
    char rootfs[PATH_MAX];
    char command[MAX_COMMAND_LEN];
    unsigned long soft_mib;
    unsigned long hard_mib;
    int nice_value;
} start_request_t;

typedef struct {
    int client_fd;
} client_request_t;

static container_t containers[MAX_CONTAINERS];
static pthread_mutex_t container_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t container_cond = PTHREAD_COND_INITIALIZER;
static log_buffer_t log_buffer;
static volatile sig_atomic_t graceful_shutdown = 0;
static volatile sig_atomic_t producer_shutdown_requested = 0;
static volatile sig_atomic_t run_client_stop_requested = 0;
static sigset_t supervisor_signal_set;
static char base_rootfs[PATH_MAX];
static char stack[MAX_CONTAINERS][STACK_SIZE];

static void safe_copy(char *dst, size_t dst_size, const char *src);
static void appendf(char *dst, size_t dst_size, const char *fmt, ...);
static int write_all(int fd, const void *buf, size_t len);
static void write_response(int client_fd, const char *text);
static int set_fd_nonblocking(int fd);
static int set_fd_cloexec(int fd);
static void reset_container_slot(container_t *cont);
static void initialize_container_slots(void);
static int container_is_live_locked(const container_t *cont);
static int find_container_slot_by_id_locked(const char *id);
static int find_container_slot_by_pid_locked(pid_t pid);
static int rootfs_in_use_locked(const char *rootfs);
static int find_reusable_slot_locked(int preferred_slot);
static int any_running_locked(void);
static int any_open_pipes_locked(void);
static int parse_unsigned_long_value(const char *text, unsigned long *out);
static int parse_int_value(const char *text, int *out);
static int build_command_string(int argc, char *argv[], int start_index, char *out, size_t out_size);
static int parse_start_request(const char *cmd_str, start_request_t *req, char *error, size_t error_size);
static int parse_schedtest_request(const char *cmd_str,
                                   char *id1,
                                   size_t id1_size,
                                   char *rootfs1,
                                   size_t rootfs1_size,
                                   char *cmd1,
                                   size_t cmd1_size,
                                   int *nice1,
                                   char *id2,
                                   size_t id2_size,
                                   char *rootfs2,
                                   size_t rootfs2_size,
                                   char *cmd2,
                                   size_t cmd2_size,
                                   int *nice2);
static void log_buffer_init(log_buffer_t *buf);
static void log_buffer_cleanup(log_buffer_t *buf);
static int log_buffer_enqueue(log_buffer_t *buf, const char *container_id, const char *data, size_t len);
static int log_buffer_dequeue(log_buffer_t *buf, log_entry_t *entry);
static void close_pipe_registration_locked(int epfd, int slot, enum pipe_kind kind);
static void register_pipe_locked(int epfd, int slot, enum pipe_kind kind);
static void *producer_thread(void *arg);
static void *consumer_thread(void *arg);
static int container_func(void *arg);
static int register_container_monitor_locked(container_t *cont);
static int start_container_impl(const char *id,
                                const char *rootfs,
                                const char *cmd,
                                unsigned long soft_mib,
                                unsigned long hard_mib,
                                int nice_value,
                                pid_t *out_pid,
                                char *error,
                                size_t error_size);
static int stop_container_impl(const char *id, char *message, size_t message_size);
static void reap_container(pid_t pid, int status);
static int wait_for_container_exit(const char *id, container_t *snapshot);
static void compute_deadline(struct timespec *deadline, int timeout_ms);
static void wait_for_quiesce(int timeout_ms);
static void stop_all_containers(int sig);
static void request_supervisor_shutdown(const char *signal_name);
static void *signal_thread(void *arg);
static void cmd_start(const char *cmd_str, int client_fd);
static void cmd_run(const char *cmd_str, int client_fd);
static void cmd_ps(int client_fd);
static void cmd_logs(const char *container_id, int client_fd);
static void cmd_stop(const char *container_id, int client_fd);
static void cmd_klist(int client_fd);
static void cmd_schedtest(const char *cmd_str, int client_fd);
static void handle_client_command(int client_fd, const char *command);
static void *client_thread(void *arg);
static void run_supervisor(const char *rootfs_base);
static int open_runtime_socket(void);
static int send_stop_command_from_client(const char *id);
static void run_client_signal_handler(int sig);
static int parse_run_status_code(const char *response);
static int cli_send_command(const char *command, int is_run, const char *run_id);

static void safe_copy(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0)
        return;

    if (!src) {
        dst[0] = '\0';
        return;
    }

    snprintf(dst, dst_size, "%s", src);
}

static void appendf(char *dst, size_t dst_size, const char *fmt, ...)
{
    size_t used;
    va_list args;

    if (!dst || dst_size == 0)
        return;

    used = strlen(dst);
    if (used >= dst_size - 1)
        return;

    va_start(args, fmt);
    vsnprintf(dst + used, dst_size - used, fmt, args);
    va_end(args);
}

static int write_all(int fd, const void *buf, size_t len)
{
    const char *cursor = (const char *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t written = write(fd, cursor, remaining);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        cursor += written;
        remaining -= (size_t)written;
    }

    return 0;
}

static void write_response(int client_fd, const char *text)
{
    if (client_fd >= 0 && text)
        (void)write_all(client_fd, text, strlen(text));
}

static int set_fd_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_fd_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD, 0);

    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static void reset_container_slot(container_t *cont)
{
    memset(cont, 0, sizeof(*cont));
    cont->stdout_fd = -1;
    cont->stderr_fd = -1;
    cont->monitor_fd = -1;
    safe_copy(cont->state, sizeof(cont->state), "empty");
    safe_copy(cont->exit_reason, sizeof(cont->exit_reason), "-");
}

static void initialize_container_slots(void)
{
    int i;

    for (i = 0; i < MAX_CONTAINERS; i++)
        reset_container_slot(&containers[i]);
}

static int container_is_live_locked(const container_t *cont)
{
    return cont->in_use &&
           (strcmp(cont->state, "starting") == 0 ||
            strcmp(cont->state, "running") == 0);
}

static int find_container_slot_by_id_locked(const char *id)
{
    int i;

    for (i = 0; i < MAX_CONTAINERS; i++) {
        if (containers[i].in_use && strcmp(containers[i].id, id) == 0)
            return i;
    }
    return -1;
}

static int find_container_slot_by_pid_locked(pid_t pid)
{
    int i;

    for (i = 0; i < MAX_CONTAINERS; i++) {
        if (containers[i].in_use && containers[i].pid == pid)
            return i;
    }
    return -1;
}

static int rootfs_in_use_locked(const char *rootfs)
{
    int i;

    for (i = 0; i < MAX_CONTAINERS; i++) {
        if (container_is_live_locked(&containers[i]) &&
            strcmp(containers[i].rootfs_path, rootfs) == 0)
            return 1;
    }
    return 0;
}

static int find_reusable_slot_locked(int preferred_slot)
{
    int i;
    int stopped_slot = -1;
    int empty_slot = -1;

    if (preferred_slot >= 0) {
        container_t *cont = &containers[preferred_slot];

        if (!container_is_live_locked(cont) &&
            cont->stdout_fd < 0 &&
            cont->stderr_fd < 0)
            return preferred_slot;
    }

    for (i = 0; i < MAX_CONTAINERS; i++) {
        if (!containers[i].in_use && empty_slot < 0)
            empty_slot = i;
        if (containers[i].in_use &&
            !container_is_live_locked(&containers[i]) &&
            containers[i].stdout_fd < 0 &&
            containers[i].stderr_fd < 0 &&
            stopped_slot < 0) {
            stopped_slot = i;
        }
    }

    if (empty_slot >= 0)
        return empty_slot;
    return stopped_slot;
}

static int any_running_locked(void)
{
    int i;

    for (i = 0; i < MAX_CONTAINERS; i++) {
        if (container_is_live_locked(&containers[i]))
            return 1;
    }
    return 0;
}

static int any_open_pipes_locked(void)
{
    int i;

    for (i = 0; i < MAX_CONTAINERS; i++) {
        if (containers[i].in_use &&
            (containers[i].stdout_fd >= 0 || containers[i].stderr_fd >= 0))
            return 1;
    }
    return 0;
}

static int parse_unsigned_long_value(const char *text, unsigned long *out)
{
    char *end = NULL;
    unsigned long value;

    if (!text || *text == '\0')
        return -1;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || !end || *end != '\0')
        return -1;

    *out = value;
    return 0;
}

static int parse_int_value(const char *text, int *out)
{
    char *end = NULL;
    long value;

    if (!text || *text == '\0')
        return -1;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || !end || *end != '\0')
        return -1;

    *out = (int)value;
    return 0;
}

static int build_command_string(int argc, char *argv[], int start_index, char *out, size_t out_size)
{
    size_t used = 0;
    int i;

    if (out_size == 0)
        return -1;

    out[0] = '\0';

    for (i = start_index; i < argc; i++) {
        int written = snprintf(out + used,
                               out_size - used,
                               "%s%s",
                               used > 0 ? " " : "",
                               argv[i]);
        if (written < 0 || (size_t)written >= out_size - used)
            return -1;
        used += (size_t)written;
    }

    return 0;
}

static int parse_start_request(const char *cmd_str, start_request_t *req, char *error, size_t error_size)
{
    char buffer[CLIENT_BUFFER_SIZE];
    char *saveptr = NULL;
    char *token;
    int command_started = 0;

    memset(req, 0, sizeof(*req));
    req->soft_mib = DEFAULT_SOFT_MIB;
    req->hard_mib = DEFAULT_HARD_MIB;
    req->nice_value = 0;

    safe_copy(buffer, sizeof(buffer), cmd_str);

    token = strtok_r(buffer, " ", &saveptr);
    if (!token) {
        safe_copy(error, error_size, "missing container id");
        return -1;
    }
    safe_copy(req->id, sizeof(req->id), token);

    token = strtok_r(NULL, " ", &saveptr);
    if (!token) {
        safe_copy(error, error_size, "missing rootfs path");
        return -1;
    }
    safe_copy(req->rootfs, sizeof(req->rootfs), token);

    while ((token = strtok_r(NULL, " ", &saveptr)) != NULL) {
        if (strcmp(token, "--") == 0) {
            while ((token = strtok_r(NULL, " ", &saveptr)) != NULL) {
                appendf(req->command,
                        sizeof(req->command),
                        "%s%s",
                        command_started ? " " : "",
                        token);
                command_started = 1;
            }
            break;
        }

        if (strcmp(token, "--soft-mib") == 0) {
            unsigned long value;

            token = strtok_r(NULL, " ", &saveptr);
            if (!token || parse_unsigned_long_value(token, &value) != 0) {
                safe_copy(error, error_size, "invalid --soft-mib value");
                return -1;
            }
            req->soft_mib = value;
            continue;
        }

        if (strcmp(token, "--hard-mib") == 0) {
            unsigned long value;

            token = strtok_r(NULL, " ", &saveptr);
            if (!token || parse_unsigned_long_value(token, &value) != 0) {
                safe_copy(error, error_size, "invalid --hard-mib value");
                return -1;
            }
            req->hard_mib = value;
            continue;
        }

        if (strcmp(token, "--nice") == 0) {
            int value;

            token = strtok_r(NULL, " ", &saveptr);
            if (!token || parse_int_value(token, &value) != 0) {
                safe_copy(error, error_size, "invalid --nice value");
                return -1;
            }
            req->nice_value = value;
            continue;
        }

        appendf(req->command,
                sizeof(req->command),
                "%s%s",
                command_started ? " " : "",
                token);
        command_started = 1;
    }

    if (req->command[0] == '\0') {
        safe_copy(error, error_size, "missing command");
        return -1;
    }

    if (req->soft_mib > req->hard_mib) {
        safe_copy(error, error_size, "soft limit cannot exceed hard limit");
        return -1;
    }

    return 0;
}

static int parse_schedtest_request(const char *cmd_str,
                                   char *id1,
                                   size_t id1_size,
                                   char *rootfs1,
                                   size_t rootfs1_size,
                                   char *cmd1,
                                   size_t cmd1_size,
                                   int *nice1,
                                   char *id2,
                                   size_t id2_size,
                                   char *rootfs2,
                                   size_t rootfs2_size,
                                   char *cmd2,
                                   size_t cmd2_size,
                                   int *nice2)
{
    char buffer[CLIENT_BUFFER_SIZE];
    char *saveptr = NULL;
    char *token;

    safe_copy(buffer, sizeof(buffer), cmd_str);

    token = strtok_r(buffer, " ", &saveptr);
    if (!token)
        return -1;
    safe_copy(id1, id1_size, token);

    token = strtok_r(NULL, " ", &saveptr);
    if (!token)
        return -1;
    safe_copy(rootfs1, rootfs1_size, token);

    token = strtok_r(NULL, " ", &saveptr);
    if (!token)
        return -1;
    safe_copy(cmd1, cmd1_size, token);

    token = strtok_r(NULL, " ", &saveptr);
    if (!token || parse_int_value(token, nice1) != 0)
        return -1;

    token = strtok_r(NULL, " ", &saveptr);
    if (!token)
        return -1;
    safe_copy(id2, id2_size, token);

    token = strtok_r(NULL, " ", &saveptr);
    if (!token)
        return -1;
    safe_copy(rootfs2, rootfs2_size, token);

    token = strtok_r(NULL, " ", &saveptr);
    if (!token)
        return -1;
    safe_copy(cmd2, cmd2_size, token);

    token = strtok_r(NULL, " ", &saveptr);
    if (!token || parse_int_value(token, nice2) != 0)
        return -1;

    return 0;
}

static void log_buffer_init(log_buffer_t *buf)
{
    pthread_mutex_init(&buf->lock, NULL);
    pthread_cond_init(&buf->not_full, NULL);
    pthread_cond_init(&buf->not_empty, NULL);
    buf->head = 0;
    buf->tail = 0;
    buf->count = 0;
    buf->shutdown = 0;
}

static void log_buffer_cleanup(log_buffer_t *buf)
{
    pthread_mutex_destroy(&buf->lock);
    pthread_cond_destroy(&buf->not_full);
    pthread_cond_destroy(&buf->not_empty);
}

static int log_buffer_enqueue(log_buffer_t *buf, const char *container_id, const char *data, size_t len)
{
    int idx;
    size_t copy_len;

    pthread_mutex_lock(&buf->lock);

    while (buf->count >= MAX_LOG_ENTRIES && !buf->shutdown)
        pthread_cond_wait(&buf->not_full, &buf->lock);

    if (buf->shutdown) {
        pthread_mutex_unlock(&buf->lock);
        return -1;
    }

    idx = buf->head;
    copy_len = len;
    if (copy_len > sizeof(buf->entries[idx].data))
        copy_len = sizeof(buf->entries[idx].data);

    safe_copy(buf->entries[idx].container_id,
              sizeof(buf->entries[idx].container_id),
              container_id);
    memcpy(buf->entries[idx].data, data, copy_len);
    buf->entries[idx].len = copy_len;
    buf->entries[idx].timestamp = time(NULL);

    buf->head = (buf->head + 1) % MAX_LOG_ENTRIES;
    buf->count++;

    pthread_cond_signal(&buf->not_empty);
    pthread_mutex_unlock(&buf->lock);
    return 0;
}

static int log_buffer_dequeue(log_buffer_t *buf, log_entry_t *entry)
{
    pthread_mutex_lock(&buf->lock);

    while (buf->count == 0 && !buf->shutdown)
        pthread_cond_wait(&buf->not_empty, &buf->lock);

    if (buf->count == 0) {
        pthread_mutex_unlock(&buf->lock);
        return -1;
    }

    *entry = buf->entries[buf->tail];
    buf->tail = (buf->tail + 1) % MAX_LOG_ENTRIES;
    buf->count--;

    pthread_cond_signal(&buf->not_full);
    pthread_mutex_unlock(&buf->lock);
    return 0;
}

static void close_pipe_registration_locked(int epfd, int slot, enum pipe_kind kind)
{
    container_t *cont = &containers[slot];
    int *fd_ptr = (kind == PIPE_STDOUT) ? &cont->stdout_fd : &cont->stderr_fd;
    int *registered_ptr = (kind == PIPE_STDOUT) ? &cont->stdout_registered : &cont->stderr_registered;

    if (*fd_ptr >= 0) {
        (void)epoll_ctl(epfd, EPOLL_CTL_DEL, *fd_ptr, NULL);
        close(*fd_ptr);
        *fd_ptr = -1;
    }

    *registered_ptr = 0;
    pthread_cond_broadcast(&container_cond);
}

static void register_pipe_locked(int epfd, int slot, enum pipe_kind kind)
{
    container_t *cont = &containers[slot];
    int fd = (kind == PIPE_STDOUT) ? cont->stdout_fd : cont->stderr_fd;
    int *registered_ptr = (kind == PIPE_STDOUT) ? &cont->stdout_registered : &cont->stderr_registered;
    struct epoll_event ev;

    if (fd < 0 || *registered_ptr)
        return;

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLHUP | EPOLLERR;
    ev.data.u64 = ((uint64_t)slot << 32) | (uint64_t)kind;

    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == 0)
        *registered_ptr = 1;
}

static void *producer_thread(void *arg)
{
    struct epoll_event events[MAX_CONTAINERS * 2];
    char read_buf[BUFFER_SIZE];
    int epfd = epoll_create1(0);
    (void)arg;

    if (epfd < 0) {
        perror("epoll_create1");
        return NULL;
    }

    fprintf(stdout, "[producer] Thread started\n");
    fflush(stdout);

    for (;;) {
        int nfds;
        int i;

        pthread_mutex_lock(&container_lock);
        for (i = 0; i < MAX_CONTAINERS; i++) {
            if (!containers[i].in_use)
                continue;
            register_pipe_locked(epfd, i, PIPE_STDOUT);
            register_pipe_locked(epfd, i, PIPE_STDERR);
        }

        if (producer_shutdown_requested && !any_open_pipes_locked()) {
            pthread_mutex_unlock(&container_lock);
            break;
        }
        pthread_mutex_unlock(&container_lock);

        nfds = epoll_wait(epfd, events, MAX_CONTAINERS * 2, 100);
        if (nfds < 0) {
            if (errno == EINTR)
                continue;
            perror("epoll_wait");
            break;
        }

        for (i = 0; i < nfds; i++) {
            char container_id[MONITOR_NAME_LEN];
            int slot = (int)(events[i].data.u64 >> 32);
            enum pipe_kind kind = (enum pipe_kind)(events[i].data.u64 & 0xffffffffU);
            int fd;

            if (slot < 0 || slot >= MAX_CONTAINERS)
                continue;

            pthread_mutex_lock(&container_lock);
            if (!containers[slot].in_use) {
                pthread_mutex_unlock(&container_lock);
                continue;
            }

            safe_copy(container_id, sizeof(container_id), containers[slot].id);
            fd = (kind == PIPE_STDOUT) ? containers[slot].stdout_fd : containers[slot].stderr_fd;
            pthread_mutex_unlock(&container_lock);

            if (fd < 0 || (events[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR)) == 0)
                continue;

            for (;;) {
                ssize_t nread = read(fd, read_buf, sizeof(read_buf));

                if (nread > 0) {
                    if (log_buffer_enqueue(&log_buffer,
                                           container_id,
                                           read_buf,
                                           (size_t)nread) != 0) {
                        break;
                    }
                    continue;
                }

                if (nread == 0) {
                    pthread_mutex_lock(&container_lock);
                    if (containers[slot].in_use) {
                        int current_fd = (kind == PIPE_STDOUT)
                                             ? containers[slot].stdout_fd
                                             : containers[slot].stderr_fd;
                        if (current_fd == fd)
                            close_pipe_registration_locked(epfd, slot, kind);
                    }
                    pthread_mutex_unlock(&container_lock);
                    break;
                }

                if (errno == EINTR)
                    continue;

                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;

                pthread_mutex_lock(&container_lock);
                if (containers[slot].in_use) {
                    int current_fd = (kind == PIPE_STDOUT)
                                         ? containers[slot].stdout_fd
                                         : containers[slot].stderr_fd;
                    if (current_fd == fd)
                        close_pipe_registration_locked(epfd, slot, kind);
                }
                pthread_mutex_unlock(&container_lock);
                break;
            }
        }
    }

    close(epfd);
    fprintf(stdout, "[producer] Thread exiting\n");
    fflush(stdout);
    return NULL;
}

static void *consumer_thread(void *arg)
{
    log_buffer_t *buf = (log_buffer_t *)arg;
    log_entry_t entry;

    mkdir(LOG_DIR, 0755);

    fprintf(stdout, "[consumer] Thread started\n");
    fflush(stdout);

    for (;;) {
        FILE *log_file;
        char path[PATH_MAX];
        int should_exit;

        pthread_mutex_lock(&buf->lock);
        should_exit = buf->shutdown && buf->count == 0;
        pthread_mutex_unlock(&buf->lock);

        if (should_exit)
            break;

        if (log_buffer_dequeue(buf, &entry) != 0)
            continue;

        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, entry.container_id);
        log_file = fopen(path, "ab");
        if (!log_file)
            continue;

        if (fwrite(entry.data, 1, entry.len, log_file) == entry.len)
            fflush(log_file);
        fclose(log_file);
    }

    fprintf(stdout, "[consumer] Thread exiting\n");
    fflush(stdout);
    return NULL;
}

static int container_func(void *arg)
{
    container_args_t *args = (container_args_t *)arg;
    sigset_t empty_set;

    sigemptyset(&empty_set);
    sigprocmask(SIG_SETMASK, &empty_set, NULL);
    signal(SIGPIPE, SIG_DFL);

    if (setpriority(PRIO_PROCESS, 0, args->nice_value) != 0)
        perror("setpriority");

    if (args->stdout_fd >= 0) {
        if (dup2(args->stdout_fd, STDOUT_FILENO) < 0)
            perror("dup2 stdout");
        close(args->stdout_fd);
    }

    if (args->stderr_fd >= 0) {
        if (dup2(args->stderr_fd, STDERR_FILENO) < 0)
            perror("dup2 stderr");
        close(args->stderr_fd);
    }

    (void)mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
    if (sethostname(args->id, strlen(args->id)) != 0)
        perror("sethostname");

    if (chroot(args->rootfs) != 0) {
        perror("chroot");
        _exit(1);
    }

    if (chdir("/") != 0) {
        perror("chdir");
        _exit(1);
    }

    (void)mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) != 0)
        perror("mount /proc");

    setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", 1);
    execl("/bin/sh", "/bin/sh", "-c", args->command, (char *)NULL);

    perror("exec");
    _exit(127);
}

static int register_container_monitor_locked(container_t *cont)
{
    struct monitor_request req;
    int fd = open("/dev/container_monitor", O_RDWR);

    if (fd < 0) {
        fprintf(stderr, "[engine] ERROR: Cannot open /dev/container_monitor: %s (errno=%d)\n", 
                strerror(errno), errno);
        fprintf(stderr, "[engine] FIX: sudo insmod boilerplate/monitor.ko && sudo chmod 666 /dev/container_monitor\n");
        return -1;
    }

    if (set_fd_cloexec(fd) != 0) {
        fprintf(stderr, "[engine] ERROR: Failed to set FD_CLOEXEC on monitor device\n");
        close(fd);
        return -1;
    }

    memset(&req, 0, sizeof(req));
    req.pid = cont->pid;
    req.soft_limit_bytes = cont->soft_limit_bytes;
    req.hard_limit_bytes = cont->hard_limit_bytes;
    safe_copy(req.container_id, sizeof(req.container_id), cont->id);

    fprintf(stderr, "[engine] DEBUG: Registering with monitor: container=%s pid=%d soft=%lu hard=%lu\n",
            req.container_id, req.pid, req.soft_limit_bytes, req.hard_limit_bytes);

    if (ioctl(fd, MONITOR_REGISTER, &req) != 0) {
        fprintf(stderr, "[engine] ERROR: MONITOR_REGISTER ioctl failed: %s (errno=%d)\n",
                strerror(errno), errno);
        close(fd);
        return -1;
    }

    fprintf(stderr, "[engine] SUCCESS: Container registered with kernel monitor\n");
    cont->monitor_fd = fd;
    return 0;
}

static int start_container_impl(const char *id,
                                const char *rootfs,
                                const char *cmd,
                                unsigned long soft_mib,
                                unsigned long hard_mib,
                                int nice_value,
                                pid_t *out_pid,
                                char *error,
                                size_t error_size)
{
    int preferred_slot;
    int slot;
    container_t *cont;
    container_args_t child_args;
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    pid_t pid;

    if (!id || !rootfs || !cmd || id[0] == '\0' || rootfs[0] == '\0' || cmd[0] == '\0') {
        safe_copy(error, error_size, "missing required start arguments");
        return -1;
    }

    if (soft_mib > hard_mib) {
        safe_copy(error, error_size, "soft limit cannot exceed hard limit");
        return -1;
    }

    pthread_mutex_lock(&container_lock);

    if (graceful_shutdown) {
        pthread_mutex_unlock(&container_lock);
        safe_copy(error, error_size, "supervisor is shutting down");
        return -1;
    }

    preferred_slot = find_container_slot_by_id_locked(id);
    if (preferred_slot >= 0 && container_is_live_locked(&containers[preferred_slot])) {
        pthread_mutex_unlock(&container_lock);
        safe_copy(error, error_size, "container id already running");
        return -1;
    }

    if (rootfs_in_use_locked(rootfs)) {
        pthread_mutex_unlock(&container_lock);
        safe_copy(error, error_size, "rootfs is already used by a running container");
        return -1;
    }

    slot = find_reusable_slot_locked(preferred_slot);
    if (slot < 0) {
        pthread_mutex_unlock(&container_lock);
        safe_copy(error, error_size, "no free container slots available");
        return -1;
    }

    cont = &containers[slot];
    reset_container_slot(cont);
    cont->in_use = 1;
    safe_copy(cont->id, sizeof(cont->id), id);
    safe_copy(cont->rootfs_path, sizeof(cont->rootfs_path), rootfs);
    safe_copy(cont->command, sizeof(cont->command), cmd);
    safe_copy(cont->state, sizeof(cont->state), "starting");
    safe_copy(cont->exit_reason, sizeof(cont->exit_reason), "starting");
    cont->soft_limit_bytes = MIB_TO_BYTES(soft_mib);
    cont->hard_limit_bytes = MIB_TO_BYTES(hard_mib);
    cont->nice_value = nice_value;
    cont->start_time = time(NULL);
    snprintf(cont->log_path, sizeof(cont->log_path), "%s/%s.log", LOG_DIR, id);
    mkdir(LOG_DIR, 0755);
    {
        FILE *log_file = fopen(cont->log_path, "wb");

        if (log_file)
            fclose(log_file);
    }

    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        if (stdout_pipe[0] >= 0)
            close(stdout_pipe[0]);
        if (stdout_pipe[1] >= 0)
            close(stdout_pipe[1]);
        if (stderr_pipe[0] >= 0)
            close(stderr_pipe[0]);
        if (stderr_pipe[1] >= 0)
            close(stderr_pipe[1]);
        reset_container_slot(cont);
        pthread_mutex_unlock(&container_lock);
        safe_copy(error, error_size, "failed to create log pipes");
        return -1;
    }

    if (set_fd_nonblocking(stdout_pipe[0]) != 0 || set_fd_nonblocking(stderr_pipe[0]) != 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        reset_container_slot(cont);
        pthread_mutex_unlock(&container_lock);
        safe_copy(error, error_size, "failed to configure non-blocking log pipes");
        return -1;
    }

    if (set_fd_cloexec(stdout_pipe[0]) != 0 ||
        set_fd_cloexec(stdout_pipe[1]) != 0 ||
        set_fd_cloexec(stderr_pipe[0]) != 0 ||
        set_fd_cloexec(stderr_pipe[1]) != 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        reset_container_slot(cont);
        pthread_mutex_unlock(&container_lock);
        safe_copy(error, error_size, "failed to configure close-on-exec for log pipes");
        return -1;
    }

    memset(&child_args, 0, sizeof(child_args));
    safe_copy(child_args.id, sizeof(child_args.id), id);
    safe_copy(child_args.rootfs, sizeof(child_args.rootfs), rootfs);
    safe_copy(child_args.command, sizeof(child_args.command), cmd);
    child_args.stdout_fd = stdout_pipe[1];
    child_args.stderr_fd = stderr_pipe[1];
    child_args.nice_value = nice_value;

    pid = clone(container_func,
                stack[slot] + STACK_SIZE,
                CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                &child_args);
    if (pid < 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        reset_container_slot(cont);
        pthread_mutex_unlock(&container_lock);
        safe_copy(error, error_size, "clone failed");
        return -1;
    }

    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    cont->pid = pid;
    cont->stdout_fd = stdout_pipe[0];
    cont->stderr_fd = stderr_pipe[0];
    safe_copy(cont->state, sizeof(cont->state), "running");
    safe_copy(cont->exit_reason, sizeof(cont->exit_reason), "-");
    cont->stdout_registered = 0;
    cont->stderr_registered = 0;

    if (register_container_monitor_locked(cont) != 0) {
        int status;
        pid_t waited;

        cont->stop_requested = 1;
        pthread_mutex_unlock(&container_lock);

        (void)kill(pid, SIGKILL);
        waited = waitpid(pid, &status, 0);
        while (waited < 0) {
            if (errno != EINTR)
                break;
            waited = waitpid(pid, &status, 0);
        }

        if (waited == pid)
            reap_container(pid, status);
        safe_copy(error,
                  error_size,
                  "failed to register container with kernel monitor; load monitor.ko first");
        return -1;
    }

    pthread_cond_broadcast(&container_cond);
    pthread_mutex_unlock(&container_lock);

    fprintf(stdout, "Started container %s (PID: %d)\n", id, pid);
    fflush(stdout);

    if (out_pid)
        *out_pid = pid;
    return 0;
}

static int stop_container_impl(const char *id, char *message, size_t message_size)
{
    int slot;
    container_t *cont;

    pthread_mutex_lock(&container_lock);
    slot = find_container_slot_by_id_locked(id);
    if (slot < 0) {
        pthread_mutex_unlock(&container_lock);
        safe_copy(message, message_size, "container not found");
        return -1;
    }

    cont = &containers[slot];
    if (!container_is_live_locked(cont)) {
        pthread_mutex_unlock(&container_lock);
        safe_copy(message, message_size, "container is not running");
        return 0;
    }

    cont->stop_requested = 1;
    if (kill(cont->pid, SIGTERM) != 0 && errno != ESRCH) {
        pthread_mutex_unlock(&container_lock);
        safe_copy(message, message_size, "failed to send SIGTERM");
        return -1;
    }
    pthread_mutex_unlock(&container_lock);

    fprintf(stdout, "Sent SIGTERM to container %s\n", id);
    fflush(stdout);

    safe_copy(message, message_size, "stop requested");
    return 0;
}

static void reap_container(pid_t pid, int status)
{
    int slot;

    pthread_mutex_lock(&container_lock);
    slot = find_container_slot_by_pid_locked(pid);
    if (slot < 0) {
        pthread_mutex_unlock(&container_lock);
        return;
    }

    {
        container_t *cont = &containers[slot];

        cont->end_time = time(NULL);
        safe_copy(cont->state, sizeof(cont->state), "stopped");

        if (WIFEXITED(status)) {
            cont->exit_code = WEXITSTATUS(status);
            cont->term_signal = 0;
            if (cont->stop_requested)
                safe_copy(cont->exit_reason, sizeof(cont->exit_reason), "stopped");
            else
                safe_copy(cont->exit_reason, sizeof(cont->exit_reason), "normal");
        } else if (WIFSIGNALED(status)) {
            cont->term_signal = WTERMSIG(status);
            cont->exit_code = 128 + cont->term_signal;
            if (cont->stop_requested)
                safe_copy(cont->exit_reason, sizeof(cont->exit_reason), "stopped");
            else if (cont->term_signal == SIGKILL)
                safe_copy(cont->exit_reason, sizeof(cont->exit_reason), "hard_limit_killed");
            else
                safe_copy(cont->exit_reason, sizeof(cont->exit_reason), "signaled");
        } else {
            safe_copy(cont->exit_reason, sizeof(cont->exit_reason), "unknown");
        }

        if (cont->monitor_fd >= 0) {
            struct monitor_request req;

            memset(&req, 0, sizeof(req));
            req.pid = pid;
            safe_copy(req.container_id, sizeof(req.container_id), cont->id);
            (void)ioctl(cont->monitor_fd, MONITOR_UNREGISTER, &req);
            close(cont->monitor_fd);
            cont->monitor_fd = -1;
        }

        pthread_cond_broadcast(&container_cond);

        fprintf(stdout,
                "Container %s reaped (PID: %d, reason: %s)\n",
                cont->id,
                pid,
                cont->exit_reason);
        fflush(stdout);
    }

    pthread_mutex_unlock(&container_lock);
}

static int wait_for_container_exit(const char *id, container_t *snapshot)
{
    pthread_mutex_lock(&container_lock);

    for (;;) {
        int slot = find_container_slot_by_id_locked(id);

        if (slot < 0) {
            pthread_mutex_unlock(&container_lock);
            return -1;
        }

        if (strcmp(containers[slot].state, "stopped") == 0) {
            if (snapshot)
                *snapshot = containers[slot];
            pthread_mutex_unlock(&container_lock);
            return 0;
        }

        pthread_cond_wait(&container_cond, &container_lock);
    }
}

static void compute_deadline(struct timespec *deadline, int timeout_ms)
{
    clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_sec += timeout_ms / 1000;
    deadline->tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec += 1;
        deadline->tv_nsec -= 1000000000L;
    }
}

static void wait_for_quiesce(int timeout_ms)
{
    struct timespec deadline;

    compute_deadline(&deadline, timeout_ms);

    pthread_mutex_lock(&container_lock);
    while (any_running_locked() || any_open_pipes_locked()) {
        int rc = pthread_cond_timedwait(&container_cond, &container_lock, &deadline);
        if (rc == ETIMEDOUT)
            break;
    }
    pthread_mutex_unlock(&container_lock);
}

static void stop_all_containers(int sig)
{
    int i;

    pthread_mutex_lock(&container_lock);
    for (i = 0; i < MAX_CONTAINERS; i++) {
        if (!container_is_live_locked(&containers[i]))
            continue;

        containers[i].stop_requested = 1;
        if (kill(containers[i].pid, sig) != 0 && errno != ESRCH)
            perror("kill");
    }
    pthread_mutex_unlock(&container_lock);
}

static void request_supervisor_shutdown(const char *signal_name)
{
    if (!graceful_shutdown) {
        graceful_shutdown = 1;
        fprintf(stdout,
                "Supervisor received %s, initiating shutdown...\n",
                signal_name ? signal_name : "signal");
        fflush(stdout);
    }
}

static void *signal_thread(void *arg)
{
    (void)arg;
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    for (;;) {
        int sig;

        if (sigwait(&supervisor_signal_set, &sig) != 0)
            continue;

        if (sig == SIGCHLD) {
            pid_t pid;
            int status;

            while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
                reap_container(pid, status);
            continue;
        }

        if (sig == SIGTERM) {
            request_supervisor_shutdown("SIGTERM");
            continue;
        }

        if (sig == SIGINT) {
            request_supervisor_shutdown("SIGINT");
            continue;
        }
    }
    return NULL;
}

static void cmd_start(const char *cmd_str, int client_fd)
{
    start_request_t req;
    char error[256];
    char response[512];
    pid_t pid;

    if (parse_start_request(cmd_str, &req, error, sizeof(error)) != 0) {
        snprintf(response, sizeof(response), "ERROR %s\n", error);
        write_response(client_fd, response);
        return;
    }

    if (start_container_impl(req.id,
                             req.rootfs,
                             req.command,
                             req.soft_mib,
                             req.hard_mib,
                             req.nice_value,
                             &pid,
                             error,
                             sizeof(error)) != 0) {
        snprintf(response, sizeof(response), "ERROR %s\n", error);
        write_response(client_fd, response);
        return;
    }

    snprintf(response, sizeof(response), "OK id=%s pid=%d\n", req.id, pid);
    write_response(client_fd, response);
}

static void cmd_run(const char *cmd_str, int client_fd)
{
    start_request_t req;
    container_t finished;
    char error[256];
    char response[512];

    if (parse_start_request(cmd_str, &req, error, sizeof(error)) != 0) {
        snprintf(response, sizeof(response), "ERROR %s\n", error);
        write_response(client_fd, response);
        return;
    }

    if (start_container_impl(req.id,
                             req.rootfs,
                             req.command,
                             req.soft_mib,
                             req.hard_mib,
                             req.nice_value,
                             NULL,
                             error,
                             sizeof(error)) != 0) {
        snprintf(response, sizeof(response), "ERROR %s\n", error);
        write_response(client_fd, response);
        return;
    }

    if (wait_for_container_exit(req.id, &finished) != 0) {
        snprintf(response, sizeof(response), "ERROR container %s disappeared\n", req.id);
        write_response(client_fd, response);
        return;
    }

    if (finished.term_signal > 0) {
        snprintf(response,
                 sizeof(response),
                 "RUN_RESULT id=%s status=%d signal=%d reason=%s\n",
                 finished.id,
                 128 + finished.term_signal,
                 finished.term_signal,
                 finished.exit_reason);
    } else {
        snprintf(response,
                 sizeof(response),
                 "RUN_RESULT id=%s status=%d reason=%s\n",
                 finished.id,
                 finished.exit_code,
                 finished.exit_reason);
    }

    write_response(client_fd, response);
}

static void cmd_ps(int client_fd)
{
    char response[RESPONSE_BUFFER_SIZE];
    int i;

    response[0] = '\0';
    appendf(response,
            sizeof(response),
            "ID          PID      STATE      ROOTFS                     LIMITS(soft/hard)  REASON\n");
    appendf(response,
            sizeof(response),
            "-------------------------------------------------------------------------------------\n");

    pthread_mutex_lock(&container_lock);
    for (i = 0; i < MAX_CONTAINERS; i++) {
        container_t *cont = &containers[i];
        const char *reason;

        if (!cont->in_use)
            continue;

        reason = cont->exit_reason[0] ? cont->exit_reason : "-";
        appendf(response,
                sizeof(response),
                "%-11s %-8d %-10s %-25.25s %4luMB/%-4luMB  %s\n",
                cont->id,
                cont->pid,
                cont->state,
                cont->rootfs_path,
                cont->soft_limit_bytes / (1024UL * 1024UL),
                cont->hard_limit_bytes / (1024UL * 1024UL),
                reason);
    }
    pthread_mutex_unlock(&container_lock);

    write_response(client_fd, response);
}

static void cmd_logs(const char *container_id, int client_fd)
{
    char log_path[PATH_MAX];
    FILE *log_file;
    char buffer[BUFFER_SIZE];
    size_t nread;

    snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, container_id);
    log_file = fopen(log_path, "rb");
    if (!log_file) {
        char response[512];

        snprintf(response, sizeof(response), "ERROR logs not found for %s\n", container_id);
        write_response(client_fd, response);
        return;
    }

    while ((nread = fread(buffer, 1, sizeof(buffer), log_file)) > 0) {
        if (write_all(client_fd, buffer, nread) != 0)
            break;
    }

    fclose(log_file);
}

static void cmd_stop(const char *container_id, int client_fd)
{
    char message[256];
    char response[320];

    if (stop_container_impl(container_id, message, sizeof(message)) != 0) {
        snprintf(response, sizeof(response), "ERROR %s\n", message);
        write_response(client_fd, response);
        return;
    }

    snprintf(response, sizeof(response), "OK id=%s %s\n", container_id, message);
    write_response(client_fd, response);
}

static void cmd_klist(int client_fd)
{
    int fd;
    struct monitor_snapshot snapshot;
    char response[RESPONSE_BUFFER_SIZE];
    unsigned int i;

    response[0] = '\0';

    fd = open("/dev/container_monitor", O_RDWR);
    if (fd < 0) {
        write_response(client_fd, "ERROR opening /dev/container_monitor\n");
        return;
    }

    memset(&snapshot, 0, sizeof(snapshot));
    if (ioctl(fd, MONITOR_LIST, &snapshot) != 0) {
        close(fd);
        write_response(client_fd, "ERROR MONITOR_LIST ioctl failed\n");
        return;
    }
    close(fd);

    appendf(response, sizeof(response), "KERNEL_MONITOR: tracked entries\n");
    appendf(response, sizeof(response), "ID          PID      SOFT(MB) HARD(MB) SOFT_EXCEEDED\n");
    appendf(response, sizeof(response), "------------------------------------------------------\n");

    if (snapshot.count == 0)
        appendf(response, sizeof(response), "<none>\n");

    for (i = 0; i < snapshot.count; i++) {
        appendf(response,
                sizeof(response),
                "%-11s %-8d %-8lu %-8lu %d\n",
                snapshot.entries[i].container_id,
                snapshot.entries[i].pid,
                snapshot.entries[i].soft_limit_bytes / (1024UL * 1024UL),
                snapshot.entries[i].hard_limit_bytes / (1024UL * 1024UL),
                snapshot.entries[i].soft_limit_exceeded);
    }

    write_response(client_fd, response);
}

static void cmd_schedtest(const char *cmd_str, int client_fd)
{
    char id1[MONITOR_NAME_LEN];
    char rootfs1[PATH_MAX];
    char cmd1[MAX_COMMAND_LEN];
    char id2[MONITOR_NAME_LEN];
    char rootfs2[PATH_MAX];
    char cmd2[MAX_COMMAND_LEN];
    int nice1;
    int nice2;
    char response[1024];
    char error[256];
    container_t result1;
    container_t result2;

    if (parse_schedtest_request(cmd_str,
                                id1,
                                sizeof(id1),
                                rootfs1,
                                sizeof(rootfs1),
                                cmd1,
                                sizeof(cmd1),
                                &nice1,
                                id2,
                                sizeof(id2),
                                rootfs2,
                                sizeof(rootfs2),
                                cmd2,
                                sizeof(cmd2),
                                &nice2) != 0) {
        write_response(client_fd,
                       "USAGE schedtest <id1> <rootfs1> <cmd1> <nice1> <id2> <rootfs2> <cmd2> <nice2>\n");
        return;
    }

    if (start_container_impl(id1,
                             rootfs1,
                             cmd1,
                             DEFAULT_SOFT_MIB,
                             DEFAULT_HARD_MIB,
                             nice1,
                             NULL,
                             error,
                             sizeof(error)) != 0) {
        snprintf(response, sizeof(response), "ERROR %s\n", error);
        write_response(client_fd, response);
        return;
    }

    if (start_container_impl(id2,
                             rootfs2,
                             cmd2,
                             DEFAULT_SOFT_MIB,
                             DEFAULT_HARD_MIB,
                             nice2,
                             NULL,
                             error,
                             sizeof(error)) != 0) {
        char stop_message[128];

        (void)stop_container_impl(id1, stop_message, sizeof(stop_message));
        (void)wait_for_container_exit(id1, NULL);
        snprintf(response, sizeof(response), "ERROR %s\n", error);
        write_response(client_fd, response);
        return;
    }

    if (wait_for_container_exit(id1, &result1) != 0 ||
        wait_for_container_exit(id2, &result2) != 0) {
        write_response(client_fd, "ERROR failed while waiting for schedtest containers\n");
        return;
    }

    snprintf(response,
             sizeof(response),
             "SCHEDTEST_RESULT\n"
             "A id=%s nice=%d elapsed_s=%ld reason=%s\n"
             "B id=%s nice=%d elapsed_s=%ld reason=%s\n",
             result1.id,
             nice1,
             (long)(result1.end_time - result1.start_time),
             result1.exit_reason,
             result2.id,
             nice2,
             (long)(result2.end_time - result2.start_time),
             result2.exit_reason);
    write_response(client_fd, response);
}

static void handle_client_command(int client_fd, const char *command)
{
    char buffer[CLIENT_BUFFER_SIZE];
    char *saveptr = NULL;
    char *verb;
    char *rest;

    safe_copy(buffer, sizeof(buffer), command);
    verb = strtok_r(buffer, " ", &saveptr);
    if (!verb)
        return;

    rest = saveptr;
    while (rest && *rest == ' ')
        rest++;

    if (strcmp(verb, "start") == 0) {
        cmd_start(rest ? rest : "", client_fd);
    } else if (strcmp(verb, "run") == 0) {
        cmd_run(rest ? rest : "", client_fd);
    } else if (strcmp(verb, "ps") == 0) {
        cmd_ps(client_fd);
    } else if (strcmp(verb, "logs") == 0) {
        if (!rest || *rest == '\0')
            write_response(client_fd, "ERROR missing container id\n");
        else
            cmd_logs(rest, client_fd);
    } else if (strcmp(verb, "stop") == 0) {
        if (!rest || *rest == '\0')
            write_response(client_fd, "ERROR missing container id\n");
        else
            cmd_stop(rest, client_fd);
    } else if (strcmp(verb, "klist") == 0) {
        cmd_klist(client_fd);
    } else if (strcmp(verb, "schedtest") == 0) {
        cmd_schedtest(rest ? rest : "", client_fd);
    } else {
        write_response(client_fd, "ERROR unknown command\n");
    }
}

static void *client_thread(void *arg)
{
    client_request_t *request = (client_request_t *)arg;
    char command[CLIENT_BUFFER_SIZE];
    size_t used = 0;

    for (;;) {
        ssize_t nread = read(request->client_fd,
                             command + used,
                             sizeof(command) - used - 1);

        if (nread < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (nread == 0)
            break;

        used += (size_t)nread;
        if (used >= sizeof(command) - 1)
            break;
    }

    if (used > 0) {
        command[used] = '\0';
        handle_client_command(request->client_fd, command);
    }

    close(request->client_fd);
    free(request);
    return NULL;
}

static void run_supervisor(const char *rootfs_base)
{
    int server_sock = -1;
    pthread_t producer_tid;
    pthread_t consumer_tid;
    pthread_t signal_tid;
    struct sockaddr_un addr;

    safe_copy(base_rootfs, sizeof(base_rootfs), rootfs_base);
    initialize_container_slots();
    log_buffer_init(&log_buffer);
    graceful_shutdown = 0;
    producer_shutdown_requested = 0;

    if (pthread_create(&signal_tid, NULL, signal_thread, NULL) != 0) {
        perror("pthread_create signal_thread");
        log_buffer_cleanup(&log_buffer);
        return;
    }

    if (pthread_create(&producer_tid, NULL, producer_thread, NULL) != 0) {
        perror("pthread_create producer_thread");
        pthread_cancel(signal_tid);
        pthread_join(signal_tid, NULL);
        log_buffer_cleanup(&log_buffer);
        return;
    }

    if (pthread_create(&consumer_tid, NULL, consumer_thread, &log_buffer) != 0) {
        perror("pthread_create consumer_thread");
        producer_shutdown_requested = 1;
        pthread_join(producer_tid, NULL);
        pthread_cancel(signal_tid);
        pthread_join(signal_tid, NULL);
        log_buffer_cleanup(&log_buffer);
        return;
    }

    unlink(SOCKET_PATH);
    server_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket");
        request_supervisor_shutdown("startup failure");
    } else {
        if (set_fd_cloexec(server_sock) != 0) {
            perror("fcntl(FD_CLOEXEC)");
            request_supervisor_shutdown("socket setup failure");
        }

        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        safe_copy(addr.sun_path, sizeof(addr.sun_path), SOCKET_PATH);

        if (!graceful_shutdown && bind(server_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            perror("bind");
            request_supervisor_shutdown("bind failure");
        } else if (!graceful_shutdown && listen(server_sock, 16) != 0) {
            perror("listen");
            request_supervisor_shutdown("listen failure");
        }
    }

    if (!graceful_shutdown) {
        fprintf(stdout, "Supervisor started. Socket: %s\n", SOCKET_PATH);
        fprintf(stdout, "Base rootfs: %s\n", base_rootfs);
        fprintf(stdout, "Ready to accept commands.\n");
        fflush(stdout);
    }

    while (!graceful_shutdown) {
        struct pollfd pfd;
        int poll_rc;

        if (server_sock < 0)
            break;

        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = server_sock;
        pfd.events = POLLIN;

        poll_rc = poll(&pfd, 1, 250);
        if (poll_rc < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        if (poll_rc == 0)
            continue;

        if (pfd.revents & POLLIN) {
            int client_fd = accept(server_sock, NULL, NULL);
            if (client_fd >= 0) {
                client_request_t *request = malloc(sizeof(*request));
                pthread_t worker;

                if (set_fd_cloexec(client_fd) != 0) {
                    close(client_fd);
                    continue;
                }

                if (!request) {
                    close(client_fd);
                    continue;
                }

                request->client_fd = client_fd;
                if (pthread_create(&worker, NULL, client_thread, request) != 0) {
                    perror("pthread_create client_thread");
                    close(client_fd);
                    free(request);
                    continue;
                }
                pthread_detach(worker);
            } else if (errno != EINTR) {
                perror("accept");
            }
        }
    }

    if (server_sock >= 0)
        close(server_sock);
    unlink(SOCKET_PATH);

    fprintf(stdout, "Terminating all containers...\n");
    fflush(stdout);

    stop_all_containers(SIGTERM);
    wait_for_quiesce(2000);
    stop_all_containers(SIGKILL);
    wait_for_quiesce(5000);

    producer_shutdown_requested = 1;
    pthread_cond_broadcast(&container_cond);
    pthread_join(producer_tid, NULL);

    pthread_mutex_lock(&log_buffer.lock);
    log_buffer.shutdown = 1;
    pthread_cond_broadcast(&log_buffer.not_empty);
    pthread_cond_broadcast(&log_buffer.not_full);
    pthread_mutex_unlock(&log_buffer.lock);
    pthread_join(consumer_tid, NULL);

    pthread_cancel(signal_tid);
    pthread_join(signal_tid, NULL);

    log_buffer_cleanup(&log_buffer);

    fprintf(stdout, "Supervisor shutdown complete.\n");
    fflush(stdout);
}

static int open_runtime_socket(void)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;

    if (sock < 0)
        return -1;

    if (set_fd_cloexec(sock) != 0) {
        close(sock);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    safe_copy(addr.sun_path, sizeof(addr.sun_path), SOCKET_PATH);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock);
        return -1;
    }

    return sock;
}

static int send_stop_command_from_client(const char *id)
{
    char command[128];
    int sock = open_runtime_socket();
    char buffer[256];

    if (sock < 0)
        return -1;

    snprintf(command, sizeof(command), "stop %s", id);
    if (write_all(sock, command, strlen(command)) != 0) {
        close(sock);
        return -1;
    }

    shutdown(sock, SHUT_WR);
    while (read(sock, buffer, sizeof(buffer)) > 0)
        ;
    close(sock);
    return 0;
}

static void run_client_signal_handler(int sig)
{
    (void)sig;
    run_client_stop_requested = 1;
}

static int parse_run_status_code(const char *response)
{
    const char *status_ptr = strstr(response, "status=");

    if (!status_ptr)
        return 0;
    return (int)strtol(status_ptr + strlen("status="), NULL, 10);
}

static int cli_send_command(const char *command, int is_run, const char *run_id)
{
    char response[BUFFER_SIZE];
    char run_result[1024];
    int sock;
    int exit_code = 0;
    int forwarded_stop = 0;
    struct sigaction old_int;
    struct sigaction old_term;
    struct sigaction sa;

    run_result[0] = '\0';
    run_client_stop_requested = 0;

    sock = open_runtime_socket();
    if (sock < 0) {
        fprintf(stderr, "Failed to connect to supervisor (is it running?)\n");
        return 1;
    }

    if (is_run) {
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = run_client_signal_handler;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, &old_int);
        sigaction(SIGTERM, &sa, &old_term);
    }

    if (write_all(sock, command, strlen(command)) != 0) {
        perror("write");
        close(sock);
        if (is_run) {
            sigaction(SIGINT, &old_int, NULL);
            sigaction(SIGTERM, &old_term, NULL);
        }
        return 1;
    }

    shutdown(sock, SHUT_WR);

    for (;;) {
        struct pollfd pfd;
        int poll_rc;
        ssize_t nread;

        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = sock;
        pfd.events = POLLIN;

        poll_rc = poll(&pfd, 1, is_run ? 200 : -1);
        if (poll_rc < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            exit_code = 1;
            break;
        }

        if (poll_rc == 0) {
            if (is_run && run_client_stop_requested && !forwarded_stop && run_id) {
                if (send_stop_command_from_client(run_id) == 0)
                    forwarded_stop = 1;
            }
            continue;
        }

        nread = read(sock, response, sizeof(response) - 1);
        if (nread < 0) {
            if (errno == EINTR)
                continue;
            perror("read");
            exit_code = 1;
            break;
        }

        if (nread == 0)
            break;

        response[nread] = '\0';
        fputs(response, stdout);

        if (strncmp(response, "ERROR", 5) == 0)
            exit_code = 1;

        if (is_run) {
            size_t used = strlen(run_result);
            size_t copy_len = (size_t)nread;

            if (copy_len > sizeof(run_result) - used - 1)
                copy_len = sizeof(run_result) - used - 1;
            memcpy(run_result + used, response, copy_len);
            run_result[used + copy_len] = '\0';
        }
    }

    close(sock);

    if (is_run) {
        sigaction(SIGINT, &old_int, NULL);
        sigaction(SIGTERM, &old_term, NULL);
        if (strstr(run_result, "status=") != NULL)
            exit_code = parse_run_status_code(run_result);
    }

    return exit_code;
}

int main(int argc, char *argv[])
{
    signal(SIGPIPE, SIG_IGN);

    if (argc < 2) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s supervisor <base-rootfs>\n", argv[0]);
        fprintf(stderr, "  %s start <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        fprintf(stderr, "  %s run <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        fprintf(stderr, "  %s ps\n", argv[0]);
        fprintf(stderr, "  %s logs <id>\n", argv[0]);
        fprintf(stderr, "  %s stop <id>\n", argv[0]);
        fprintf(stderr, "  %s klist\n", argv[0]);
        fprintf(stderr, "  %s schedtest <id1> <rootfs1> <cmd1> <nice1> <id2> <rootfs2> <cmd2> <nice2>\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }

        sigemptyset(&supervisor_signal_set);
        sigaddset(&supervisor_signal_set, SIGCHLD);
        sigaddset(&supervisor_signal_set, SIGTERM);
        sigaddset(&supervisor_signal_set, SIGINT);
        if (pthread_sigmask(SIG_BLOCK, &supervisor_signal_set, NULL) != 0) {
            perror("pthread_sigmask");
            return 1;
        }

        run_supervisor(argv[2]);
        return 0;
    }

    if (strcmp(argv[1], "start") == 0 || strcmp(argv[1], "run") == 0) {
        char command[CLIENT_BUFFER_SIZE];

        if (build_command_string(argc, argv, 1, command, sizeof(command)) != 0) {
            fprintf(stderr, "Command is too long\n");
            return 1;
        }

        return cli_send_command(command,
                                strcmp(argv[1], "run") == 0,
                                argc >= 3 ? argv[2] : NULL);
    }

    if (strcmp(argv[1], "ps") == 0)
        return cli_send_command("ps", 0, NULL);

    if (strcmp(argv[1], "logs") == 0) {
        char command[128];

        if (argc < 3) {
            fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
            return 1;
        }

        snprintf(command, sizeof(command), "logs %s", argv[2]);
        return cli_send_command(command, 0, NULL);
    }

    if (strcmp(argv[1], "stop") == 0) {
        char command[128];

        if (argc < 3) {
            fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
            return 1;
        }

        snprintf(command, sizeof(command), "stop %s", argv[2]);
        return cli_send_command(command, 0, NULL);
    }

    if (strcmp(argv[1], "klist") == 0)
        return cli_send_command("klist", 0, NULL);

    if (strcmp(argv[1], "schedtest") == 0) {
        char command[CLIENT_BUFFER_SIZE];

        if (argc < 10) {
            fprintf(stderr,
                    "Usage: %s schedtest <id1> <rootfs1> <cmd1> <nice1> <id2> <rootfs2> <cmd2> <nice2>\n",
                    argv[0]);
            return 1;
        }

        if (build_command_string(argc, argv, 1, command, sizeof(command)) != 0) {
            fprintf(stderr, "Command is too long\n");
            return 1;
        }

        return cli_send_command(command, 0, NULL);
    }

    fprintf(stderr, "Unknown command: %s\n", argv[1]);
    return 1;
}
