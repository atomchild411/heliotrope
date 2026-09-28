/* pfd PID...: list each process's open descriptors as fd dev inode (PIOCGETINODE),
 * to find descriptors shared between processes. */
#include <sys/types.h>
#include <sys/procfs.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int a;
    for (a = 1; a < argc; a++) {
        char path[64];
        int pfd, fd;
        snprintf(path, sizeof path, "/proc/%s", argv[a]);
        if ((pfd = open(path, O_RDONLY)) < 0) {
            perror(path);
            continue;
        }
        for (fd = 0; fd < 2500; fd++) {
            prinodeinfo_t info;
            info.pi_fd = fd;
            if (ioctl(pfd, PIOCGETINODE, &info) == 0)
                printf("%s %d %lx %llu\n", argv[a], fd, (unsigned long)info.pi_dev, (unsigned long long)info.pi_inum);
        }
        close(pfd);
    }
    return 0;
}
