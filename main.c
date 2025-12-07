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

#include "rootfs.h"
// #include "ns.h"
#include "simple_ns.h"

#define STACK_SIZE (1024 * 1024)
/*


1. Bind mount rootfs onto intself -> So we mount rootfs to rootfs ig
2. Create rotfs/oldroot
4. Pivot root rotfs to oldroot so we have an old root fielsystem
changdirectory
unmount oldroot
remove oldroot mo
mount /prov to new root

*/

int child_func(void *arg)
{
    printf("Inside new PID + Mount namespace\n");
    printf("PID inside container: %d\n", getpid());

    struct child_args *cargs = (struct child_args *)arg;

    setup_rootfs();
    // 1. Wait for parent to finish network setup
    char buf;
    printf("⭐ [child] Waiting for parent to set up network...\n");
    if (read(cargs->sync_pipe, &buf, 1) != 1)
    {
        perror("child read sync_pipe");
        return 1;
    }
    close(cargs->sync_pipe); // done with it

    printf("⭐ About to exec shell...\n");

    if (sethostname("cdocker", strlen("cdocker")) != 0)
    {
        perror("sethostname");
    }
    // Now run a shell by replacing our current process with a shell process
    char *const args[] = {"/bin/sh", NULL};
    execv("/bin/sh", args);

    perror("execv /bin/sh failed");
    return 1;
}

int main()
{
    // Create sync pipe
    int pipefd[2];
    if (pipe(pipefd) < 0)
    {
        perror("pipe");
        return 1;
    }

    void *stack = malloc(STACK_SIZE);
    if (!stack)
    {
        perror("malloc");
        return 1;
    }

    struct child_args args = {
        .sync_pipe = pipefd[0] // child gets read end
    };

    pid_t child = clone(
        child_func,
        stack + STACK_SIZE,
        CLONE_NEWPID | CLONE_NEWNET | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD,
        &args);

    if (child < 0)
    {
        perror("clone");
        return 1;
    }

    close(pipefd[0]); // parent doesn't need read end

    printf("[parent] Child PID = %d\n", child);

    // Set up networking from parent
    if (setup_network(child) < 0)
    {
        fprintf(stderr, "[parent] Network setup failed\n");
        // Continue anyway, container just won't have networking
    }

    // Signal child to proceed
    printf("[parent] Signaling child\n");
    write(pipefd[1], "x", 1);
    close(pipefd[1]);

    // Wait for child to exit
    waitpid(child, NULL, 0);

    printf("[parent] Child exited, cleaning up\n");
    // veth pair auto-deleted when child namespace dies

    free(stack);
    return 0;
}