#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <string.h>
#include <sys/syscall.h>
#include <linux/limits.h>
#include <errno.h>
#include <fcntl.h>

#include "network.h"

#define STACK_SIZE (1024 * 1024)

// Passed to child via clone arg
struct child_args {
    int sync_pipe;  // child reads from this, blocks until parent says go

};

int setup_network(pid_t child_pid) {
    int ret = 0;

    // Save host namespace
    int host_ns = open("/proc/self/ns/net", O_RDONLY);
    if (host_ns < 0) {
        perror("open host netns");
        return -1;
    }

    // Open child's namespace
    char ns_path[64];
    snprintf(ns_path, sizeof(ns_path), "/proc/%d/ns/net", child_pid);
    int child_ns = open(ns_path, O_RDONLY);
    if (child_ns < 0) {
        perror("open child netns");
        close(host_ns);
        return -1;
    }

    // Create veth pair in host namespace
    printf("[parent] Creating veth pair\n");
    if (veth_create("veth_host", "veth_cont") < 0) {
        ret = -1;
        goto cleanup;
    }

    // Move container end to child
    printf("[parent] Moving veth_cont to child netns\n");
    if (if_move_to_pid_ns("veth_cont", child_pid) < 0) {
        ret = -1;
        goto cleanup;
    }

    // Configure host end
    printf("[parent] Configuring host side\n");
    if_add_addr("veth_host", "10.0.0.1/24");
    if_up("veth_host");

    // Enter child namespace and configure its end
    printf("[parent] Entering child netns to configure\n");
    if (setns(child_ns, CLONE_NEWNET) < 0) {
        perror("setns to child");
        ret = -1;
        goto cleanup;
    }

    if_add_addr("veth_cont", "10.0.0.2/24");
    if_up("veth_cont");
    if_up("lo");

    // Return to host namespace
    if (setns(host_ns, CLONE_NEWNET) < 0) {
        perror("setns to host");
        ret = -1;
    }

cleanup:
    close(host_ns);
    close(child_ns);
    return ret;
}



/*
```

The flow:
```
PARENT                              CHILD
──────                              ─────
clone() ──────────────────────────► starts, blocks on pipe read
    │
setup_network()
  - create veth pair
  - move veth_cont to child
  - configure host end
  - setns into child, configure its end
  - setns back to host
    │
write to pipe ────────────────────► unblocks
    │                               setup_rootfs()
    │                               exec /bin/sh
waitpid()

*/