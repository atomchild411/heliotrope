/* pmapsgi PID: per-mapping resident and private sizes of an IRIX process (PIOCMAP_SGI). */
#include <sys/types.h>
#include <sys/procfs.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    char path[64];
    prmap_sgi_t maps[4096];
    prmap_sgi_arg_t arg;
    int fd, n, i;
    long pk = getpagesize() / 1024, tv = 0, tp = 0;
    if (argc != 2) { fprintf(stderr, "usage: pmapsgi PID\n"); return 2; }
    snprintf(path, sizeof path, "/proc/%s", argv[1]);
    if ((fd = open(path, O_RDONLY)) < 0) { perror(path); return 1; }
    arg.pr_vaddr = (caddr_t)maps;
    arg.pr_size = sizeof maps;
    if ((n = ioctl(fd, PIOCMAP_SGI, &arg)) < 0) { perror("PIOCMAP_SGI"); return 1; }
    printf("page %ld KB\n%10s %10s %9s %9s %6s %s\n", pk, "vaddr", "sizeKB", "residKB", "privKB", "flags", "dev/ino");
    for (i = 0; i < n; i++) {
        prmap_sgi_t *m = &maps[i];
        tv += m->pr_vsize; tp += m->pr_psize;
        printf("%10lx %10lu %9ld %9ld %6lx %ld/%ld\n", (unsigned long)m->pr_vaddr, (unsigned long)m->pr_size / 1024,
            (long)m->pr_vsize * pk, (long)m->pr_psize * pk, (unsigned long)m->pr_mflags & 0xffff, (long)m->pr_dev, (long)m->pr_ino);
    }
    printf("total resident %ld KB, private %ld KB, %d maps\n", tv * pk, tp * pk, n);
    return 0;
}
