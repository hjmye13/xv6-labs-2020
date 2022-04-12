#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void primes(int *p);

int
main(int argc, char* argv[]) {
    int p[2];
    int end = 35;
    pipe(p);
    if (fork() == 0) {
        primes(p);
    }
    else {
        close(p[0]);
        for (int i = 2; i <= end; i++) {
            if (write(p[1], (void*)&i, sizeof(i)) != sizeof(i)) {
                fprintf(2, "%d: Write fail!\n", getpid());
                exit(1);
            }
        }
        close(p[1]);
        wait(0);
    }
    exit(0);
}

void 
primes(int *p) {
    int num, d;
    close(p[1]);
    if (read(p[0], &num, sizeof(num)) != sizeof(num)) {
        fprintf(2, "Read fail!\n");
        exit(1);
    }
    printf("prime %d\n", num);
    if (read(p[0], &d, sizeof(d))) {
        int pd[2];
        pipe(pd);
        if (fork() ==0) {
            primes(pd);
        }
        else {
            close(pd[0]);
            do {
                if (d % num != 0) {
                    if (write(pd[1], (void*)&d, sizeof(d)) != sizeof(d)) {
                        fprintf(2, "%d: Write fail!\n", getpid());
                        exit(1);
                    }
                }
            }while (read(p[0], &d, sizeof(d)));
            close(pd[1]);
            wait(0);
            exit(0);
        }
    }
    exit(0);
}

