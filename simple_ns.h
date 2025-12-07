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

    // 4) Enable IP forwarding and NAT on host
    printf("[parent] Setting up NAT and forwarding\n");
    system("sysctl -w net.ipv4.ip_forward=1 > /dev/null");
    
    // NAT for outbound traffic (adjust enp0s1 to match your interface)
    system("iptables -t nat -C POSTROUTING -s 10.0.0.0/24 -o enp0s1 -j MASQUERADE 2>/dev/null || "
           "iptables -t nat -A POSTROUTING -s 10.0.0.0/24 -o enp0s1 -j MASQUERADE");
    
    // Allow forwarding
    system("iptables -C FORWARD -i veth_host -o enp0s1 -j ACCEPT 2>/dev/null || "
           "iptables -A FORWARD -i veth_host -o enp0s1 -j ACCEPT");
    system("iptables -C FORWARD -i enp0s1 -o veth_host -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || "
           "iptables -A FORWARD -i enp0s1 -o veth_host -m state --state RELATED,ESTABLISHED -j ACCEPT");

    // 5) Configure container side by entering its netns
    printf("[parent] Entering child netns to configure child side\n");
    char ns_path[64];
    snprintf(ns_path, sizeof(ns_path), "/proc/%d/ns/net", child_pid);

    int child_ns = open(ns_path, O_RDONLY);
    if (child_ns < 0) {
        perror("open child netns");
        ret = -1;
        goto cleanup;
    }

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

    // Now in child netns
    system("ip link set lo up");
    system("ip addr add 10.0.0.2/24 dev veth_cont");
    system("ip link set veth_cont up");
    system("ip link set veth_cont name eth0");
    
    // Add default route for internet access
    system("ip route add default via 10.0.0.1");

    // Go back to host netns
    if (setns(host_ns, CLONE_NEWNET) < 0) {
        perror("setns back to host");
    }

    close(child_ns);
    close(host_ns);

cleanup:
    return ret;
}