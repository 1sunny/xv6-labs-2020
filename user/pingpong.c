//
// Created by will on 2022/8/4.
//

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
    int p_fr[2], p_cr[2];
    char data[2] = "1";
    pipe(p_fr);
    pipe(p_cr);
    int pid = fork();
    if (pid == 0){
        read(p_cr[0], data, sizeof(data));
        close(p_cr[0]);
        close(p_cr[1]);
        close(p_fr[0]);
        printf("%d: received ping\n", getpid());
        write(p_fr[1], data, strlen(data));
        close(p_fr[1]);
    }else{
        write(p_cr[1], data, strlen(data));
        close(p_cr[0]);
        close(p_cr[1]);
        close(p_fr[1]);
        wait((int *)0);
        read(p_fr[0], data, sizeof(data));
        close(p_fr[0]);
        printf("%d: received pong\n", getpid());
    }
    exit(0);
}
