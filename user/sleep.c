//
// Created by will on 2022/8/4.
//

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
    char *msg = "sleep error\n";
    if (argc < 2){
        fprintf(2, msg);
        exit(1);
    }
    sleep(atoi(argv[1]));
    exit(0);
}
