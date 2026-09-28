/* pagedump PID ADDR COUNT STEP > out: raw copies of COUNT 16 KB pages every STEP bytes from ADDR. */
#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc, char **argv)
{
    char path[64]; static char pg[16384];
    unsigned long a = strtoul(argv[2], 0, 16), step = strtoul(argv[4], 0, 0); int n = atoi(argv[3]), i, fd;
    snprintf(path, sizeof path, "/proc/%s", argv[1]);
    if ((fd = open(path, O_RDONLY)) < 0) return 1;
    for (i = 0; i < n; i++, a += step)
        if (lseek(fd, (off_t)a, SEEK_SET) < 0 || read(fd, pg, sizeof pg) != sizeof pg || fwrite(pg, 1, sizeof pg, stdout) != sizeof pg) return 1;
    return 0;
}
