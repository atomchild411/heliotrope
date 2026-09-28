/* heapscan PID ADDR SIZE: classify the pages of a region of another process (read via /proc). */
#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    char path[64];
    static unsigned char pg[16384];
    unsigned long addr = strtoul(argv[2], 0, 16), size = strtoul(argv[3], 0, 0), off;
    long zero = 0, total = 0, runstart = -1;
    int fd;
    snprintf(path, sizeof path, "/proc/%s", argv[1]);
    if ((fd = open(path, O_RDONLY)) < 0) { perror(path); return 1; }
    for (off = 0; off < size; off += sizeof pg) {
        int i, z = 1;
        if (lseek(fd, (off_t)(addr + off), SEEK_SET) < 0 || read(fd, pg, sizeof pg) != sizeof pg) break;
        total++;
        for (i = 0; i < (int)sizeof pg; i++) if (pg[i]) { z = 0; break; }
        if (z) { zero++; if (runstart < 0) runstart = off; }
        else {
            if (runstart >= 0 && off - runstart >= 1048576)
                printf("zero run %lx..%lx (%lu KB)\n", addr + runstart, addr + off, (off - runstart) / 1024);
            runstart = -1;
            if (total % 1024 == 1) {
                printf("sample %lx:", addr + off);
                for (i = 0; i < 32; i += 4) printf(" %08x", *(unsigned *)(pg + i));
                printf("\n");
            }
        }
    }
    if (runstart >= 0) printf("zero run %lx..%lx (%lu KB)\n", addr + runstart, addr + off, (off - runstart) / 1024);
    printf("%ld pages, %ld all-zero\n", total, zero);
    return 0;
}
