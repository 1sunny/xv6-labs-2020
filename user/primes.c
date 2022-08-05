//
// Created by will on 2022/8/4.
//

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

//void primes(int rd) {
//    int first, latter;
//    read(rd, &first, sizeof(first));
//    printf("prime %d\n", first);
//    int p[2];
//    int created = 0;
//
//    while (read(rd, &latter, sizeof(latter))) {
//        if (!created) {
//            created = 1;
//            pipe(p);
//            int pid = fork();
//            if (pid == 0) {
//                close(p[1]);
//                primes(p[0]);
//                return;
//            } else {
//                close(p[0]);
//            }
//        }
//        if (latter % first) {
//            write(p[1], &latter, sizeof(latter));
//        }
//    }
//    close(rd);
//    close(p[1]);
//    wait(0);
//}
//
//int
//main(int argc, char *argv[]) {
//    int fd[2];
//    pipe(fd);
//    if (fork() != 0) {
//        close(fd[0]);
//        for (int i = 2; i <= 35; ++i) {
//            write(fd[1], &i, sizeof(int));
//        }
//        close(fd[1]);
//        wait(0);
//    } else {
//        close(fd[1]);
//        primes(fd[0]);
//        close(fd[0]);
//    }
//    exit(0);
//}


int fd[100][2], u = 0;

void solve(){
    int first, num;

    while (1) {
//        printf("read %d--%d\n", fd[u][0], fd[u][1]);
        read(fd[u][0], &first, sizeof(int));
//        printf("here\n");
        printf("prime %d\n", first);
        if (first == 31) {
            break;
        }
        pipe(fd[++u]);
        int pid = fork();
        if (pid) {
            while (read(fd[u - 1][0], &num, sizeof(int))) {
                if (num % first) {
                    write(fd[u][1], &num, sizeof(int));
                }
            }
            close(fd[u][1]);
            close(fd[u - 1][0]);
            wait(0);
            break;
        }
    }
}
int
main(int argc, char *argv[]) {
    pipe(fd[0]);
    if (fork()){
        close(fd[0][0]);
        for (int i = 2; i <= 31; ++i) {
            write(fd[0][1], &i, sizeof(int));
        }
        close(fd[0][1]);
        wait(0);
    }else{
        solve();
    }
    exit(0);
}
