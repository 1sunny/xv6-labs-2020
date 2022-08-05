//
// Created by will on 2022/8/4.
//

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// 不知道为什么这份是错的
int
main(int argc, char *argv[]) {
    int first, num, fd[100][2], u = 0;
    pipe(fd[0]);
    for (int i = 2; i <= 31; ++i) {
        write(fd[u][1], &i, sizeof(int));
    }
    close(fd[u][1]);
    while (1) {
        int rd = read(fd[u][0], &first, sizeof(int));
        printf("prime %d\n", first);
        if (rd == 0) {
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
            break;
        }
    }
    exit(0);
}
