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

static int pivot_root_wrapper(const char* new_root, const char *put_old){
    return syscall(SYS_pivot_root, new_root, put_old);
}

int child_func(void *arg) {
    printf("Inside new PID + Mount namespace\n");
    printf("PID inside container: %d\n", getpid());

    // Make all mounts private to avoid propagation issues with pivot_root
    if (mount("", "/", "", MS_PRIVATE | MS_REC, "") != 0) {
        perror("mount private");
        return 1;
    }

    const char *new_root = "./rootfs"; 
    char old_root[PATH_MAX];

    // Ensure rootfs exists
    if (mkdir(new_root, 0755) && errno != EEXIST) {
        perror("mkdir rootfs"); 
        return 1; 
    }

    // bind mount rootfs onto itself so it's a mountapoint
    // A mountpoint is a mounted directyroy, we have rounted this special fs, the root fs, onto itself 
    // this is imoprtant because we need this for pivot root syscall
    if (mount(new_root, new_root, "bind", MS_BIND | MS_REC, "") != 0) {
        perror("bind_mount rootfs"); 
        return 1; 
    }

    // (3) Prepare the rootfs directory structure BEFORE pivot_root
    snprintf(old_root, sizeof(old_root), "%s/%s", new_root, "oldroot"); 
    if (mkdir(old_root, 0755) && errno != EEXIST) {
        perror("mkdir oldroot"); 
        return 1; 
    }

    // Create mount point directories in rootfs before pivot
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/proc", new_root);
    mkdir(path, 0555);
    
    snprintf(path, sizeof(path), "%s/sys", new_root);
    mkdir(path, 0755);
    
    snprintf(path, sizeof(path), "%s/dev", new_root);
    mkdir(path, 0755);

    // (4) pivot root (new root, old root) now we can pivot to a new pointpoint root 
    if (pivot_root_wrapper(new_root, old_root) != 0) {
        perror("pivot root failed");
        return 1;
    }
    

    // Change the dir to our new /
    if (chdir("/") != 0){
        perror("chdir / "); 
        return 1; 
    }

    // ------ mounting stuff (BEFORE unmounting oldroot) -------
    // Directories were already created before pivot_root

    // Mount /dev from the old root (host) while we still have access to it
    if (mount("/oldroot/dev", "/dev", "", MS_BIND | MS_REC, "") != 0){
        perror("mount /dev failed"); 
        // Continue - not critical for basic functionality
    }

    // (5) mount /proc to new root
    if (mount("proc", "/proc", "proc", 0, "") != 0){
        perror("mount /proc failed"); 
        return 1; 
    }

    // Mount /sys filesystem
    if (mount("sysfs", "/sys", "sysfs", 0, "") != 0){
        perror("mount /sys failed"); 
        return 1; 
    }

    // unmount oldroot and remove it now (after we've grabbed what we need from it)
    if (umount2("/oldroot", MNT_DETACH) != 0){
        perror("umount2 /oldroot"); 
        // Continue anyway - may already be unmounted
    }

    if (rmdir("/oldroot") != 0){
        perror("rmdir /oldroot"); 
        // Continue anyway - directory removal is not critical
    }

    printf("⭐ About to exec shell...\n");

    if (sethostname("cdocker", strlen("cdocker")) != 0) {
        perror("sethostname");
    }    
    // Now run a shell by replacing our current process with a shell process 
    char *const args[] = {"/bin/sh", NULL};
    execv("/bin/sh", args);

    perror("execv /bin/sh failed");
    return 1;
}

int main() {
    void *stack = malloc(STACK_SIZE);
    if (!stack){
        perror("malloc");
        return 1;
    }
    void *stack_top = stack + STACK_SIZE;

    pid_t child = clone(
        child_func,
        stack_top,
        CLONE_NEWPID | CLONE_NEWNET | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD,
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
