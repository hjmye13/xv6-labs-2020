#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/param.h"
#include "user/user.h"


int main(int argc, char *argv[]){
    char buff[512];
    char *arg[MAXARG];
    char **p;
    if (argc - 1 > MAXARG || argc < 2){
        fprintf(2, "Error: arguments error!\n");
        exit(1);
    }
    memset(arg, 0 ,sizeof(arg));
    for(int i=1;i < argc;i++){
        arg[i-1] = argv[i];
        //printf("arg[%d]:%s\n", i-1, arg[i-1]);
    }
    while(gets(buff, sizeof(buff))){
        if(buff[0] == 0) break;
        if(buff[strlen(buff) -1 ] == '\n' || buff[strlen(buff) -1 ] == '\r'){
            buff[strlen(buff) -1 ] = 0;
        }
        p = arg + argc -1;
        *p = buff;
        //printf("buff:%s\n", buff);
        if(fork() > 0){
            wait(0);
            memset(buff, 0 ,sizeof(buff));
        }
        else{
            exec(arg[0], arg);
            exit(0);
        }
    }
    exit(0);
}