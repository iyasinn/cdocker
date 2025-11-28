#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <string.h>

int child_func(void *arg) {
    printf("Inside new PID + Mount namespace\n");
    printf("PID inside container: %d\n", getpid());

    if (chroot("./rootfs") != 0) {
        perror("chroot failed");
        return 1;
    }
    if (chdir("/") != 0) {
        perror("chdir failed"); 
        return 1;
    }

    // Create /proc in case it does not exist
    mkdir("/proc", 0555);
    // Mount a new proc filesystem for this namespace
    if (mount("proc", "/proc", "proc", 0, "") != 0) {
        perror("mount /proc failed");
    }

    // Now run a shell by replacing our current process with a shell process 
    char *const args[] = {"/bin/sh", NULL};
    execv("/bin/sh", args);

    perror("execv failed");
    return 1;
}

int main() {
    const int STACK_SIZE = 1024 * 1024;
    void *stack = malloc(STACK_SIZE);
    void *stack_top = stack + STACK_SIZE;

    pid_t child = clone(
        child_func,
        stack_top,
        CLONE_NEWPID | CLONE_NEWNS | SIGCHLD,
        NULL
    );

    if (child < 0) {
        perror("clone");
        exit(1);
    }

    printf("Child created with host PID = %d\n", child);
    waitpid(child, NULL, 0);
    return 0;
}
