
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>

struct child_args {
    int sync_pipe;  // child reads from this, blocks until parent says go

};

int setup_network(pid_t child_pid) {
    char cmd[256];
    int ret = 0;

    // 1) Create veth pair in host netns
    printf("[parent] Creating veth pair\n");
    snprintf(cmd, sizeof(cmd),
             "ip link add veth_host type veth peer name veth_cont");
    if (system(cmd) != 0) {
        fprintf(stderr, "[parent] failed to create veth pair\n");
        return -1;
    }

    // 2) Move veth_cont into child's netns
    printf("[parent] Moving veth_cont to child netns\n");
    snprintf(cmd, sizeof(cmd),
             "ip link set veth_cont netns %d", child_pid);
    if (system(cmd) != 0) {
        fprintf(stderr, "[parent] failed to move veth_cont to child\n");
        ret = -1;
        goto cleanup;
    }

    // 3) Configure host side: IP + up
    printf("[parent] Configuring host veth_host\n");
    system("ip addr add 10.0.0.1/24 dev veth_host");
    system("ip link set veth_host up");

    // 4) Configure container side by entering its netns
    printf("[parent] Entering child netns to configure child side\n");
    char ns_path[64];
    snprintf(ns_path, sizeof(ns_path), "/proc/%d/ns/net", child_pid);
    int child_ns = open(ns_path, O_RDONLY);
    if (child_ns < 0) {
        perror("open child netns");
        ret = -1;
        goto cleanup;
    }

    // Save host ns
    int host_ns = open("/proc/self/ns/net", O_RDONLY);
    if (host_ns < 0) {
        perror("open host netns");
        close(child_ns);
        ret = -1;
        goto cleanup;
    }

    if (setns(child_ns, CLONE_NEWNET) < 0) {
        perror("setns to child");
        close(child_ns);
        close(host_ns);
        ret = -1;
        goto cleanup;
    }

    // Now "we are in child netns" but still in parent process
    system("ip link set lo up");
    system("ip addr add 10.0.0.2/24 dev veth_cont");
    system("ip link set veth_cont up");

    // Optional: rename veth_cont -> eth0
    system("ip link set veth_cont name eth0");

    // Go back to host netns
    if (setns(host_ns, CLONE_NEWNET) < 0) {
        perror("setns back to host");
    }

    close(child_ns);
    close(host_ns);

cleanup:
    return ret;
}