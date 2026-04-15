#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <sys/ioctl.h>
#include <sys/types.h>
#endif

#define MONITOR_NAME_LEN 32
#define MONITOR_MAX_SNAPSHOT 64

struct monitor_request {
    pid_t pid;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    char container_id[MONITOR_NAME_LEN];
};

struct monitor_entry_info {
    pid_t pid;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int soft_limit_exceeded;
    char container_id[MONITOR_NAME_LEN];
};

struct monitor_snapshot {
    unsigned int count;
    struct monitor_entry_info entries[MONITOR_MAX_SNAPSHOT];
};

#define MONITOR_MAGIC 'M'
#define MONITOR_REGISTER _IOW(MONITOR_MAGIC, 1, struct monitor_request)
#define MONITOR_UNREGISTER _IOW(MONITOR_MAGIC, 2, struct monitor_request)
#define MONITOR_LIST _IOR(MONITOR_MAGIC, 3, struct monitor_snapshot)

#endif
