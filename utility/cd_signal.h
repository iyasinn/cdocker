#include <unistd.h>
#include <stdio.h>


struct cd_signal {
    int read_fd; 
    int write_fd; 
};

int cd_signal_init(struct cd_signal *sig)
{
    int pipefd[2];

    if (pipe(pipefd) < 0) {
        perror("pipe");
        return -1;
    }

    sig->read_fd  = pipefd[0];
    sig->write_fd = pipefd[1];

    return 0;
}

int cd_signal_wait(struct cd_signal *sig)
{
    // read one character to unblock 
    char buf; 
    int n = read(sig->read_fd, &buf, 1);
    if (n < 0) {
        perror("read");
        return -1;
    }
    return 0;
}

int cd_signal_write(struct cd_signal *sig)
{
    // read one character to unblock 
    char buf = '\0';
    int n = write(sig->read_fd, &buf, 1);
    if (n < 0) {
        perror("write");
        return -1;
    }
    return 0;
}


int cd_signal_close(struct cd_signal *sig)
{
    close(sig->read_fd);
    close(sig->write_fd);
}


// cd_sync_init()
// cd_sync_child_wait()
// cd_sync_parent_notify()
// cd_sync_close()



