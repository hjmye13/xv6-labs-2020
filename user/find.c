//find all the files in a directory tree with a specific name
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user.h"
#include "kernel/fs.h"

void find(char* path, char* filename);

int main (int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(2, "Please enter a dir and a filename!\n");
        exit(1);
    }
    else {
        char *path = argv[1];
        char *filename = argv[2];
        find(path, filename);
        exit(0);
    }
}

void find(char* path, char* filename) {
    int fd;
    struct dirent de;
    struct stat st;
    if ((fd = open(path, 0)) < 0) {
        fprintf(2, "find: cannot open %s\n", path);
        exit(1);
    }

    if(fstat(fd, &st) < 0){
        fprintf(2, "find: cannot stat %s\n", path);
        close(fd);
        exit(1);
    }

    char buf[512];
    char *p;
    switch(st.type) {
        case T_FILE:
            if (strcmp(path + strlen(path) - strlen(filename), filename)== 0) {
                printf("%s\n", path);
            }
            break;
        case T_DIR:
            if (strlen(path) + 1 + DIRSIZ + 1 > sizeof (buf)) {
                fprintf(2, "find: path too long\n");
                break;
            }

            strcpy(buf, path);
            p = buf + strlen(buf);
            *p = '/';
            p++;
            while (read(fd, &de, sizeof(de)) == sizeof(de)) {
                if (de.inum == 0 || strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0){
                    continue;
                }
                memmove(p, de.name, DIRSIZ);
                p[DIRSIZ] = 0;
                //获得文件描述
                if(stat(buf, &st) < 0){
                    fprintf(2, "find: cannot stat %s\n", buf);
                    continue;
                }
                if (st.type == T_FILE) {
                    if (strcmp(de.name, filename)== 0) {
                        printf("%s\n", buf);
                    }
                }
                else if (st.type == T_DIR) {
                    find(buf, filename);
                }
            }
            break;
    }
    close(fd);
}