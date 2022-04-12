#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int 
main(int argc, char* argv[]) {
    int pptc[2], pctp[2];
    pipe(pptc);
    pipe(pctp);
    char buf;
    if (fork() == 0) {
        close(pptc[1]);
        close(pctp[0]);
        if (read(pptc[0], &buf, 1) != 1) {
            fprintf(2, "Can't read from parent!\n");
            exit(1);
        };
        close(pptc[0]);
        printf("%d: received ping\n", getpid());
        if (write(pctp[1], "c", 1) != 1) {
            fprintf(2, "Can't write to parent!\n");
            exit(1);
        }
        close(pctp[1]);
        exit(0);
    }
    else {
        close(pptc[0]);
        close(pctp[1]);
        if (write(pptc[1], "p", 1) != 1) {
            fprintf(2, "Can't write to child!\n");
            exit(1);
        }
        close(pptc[1]);
        char buf;
        if (read(pctp[0], &buf, 1) != 1) {
            fprintf(2, "Can't read from child!");
            exit(1);
        }
        printf("%d: received pong\n", getpid());
        close(pctp[0]);
        wait(0);
    }
    exit(0);
}