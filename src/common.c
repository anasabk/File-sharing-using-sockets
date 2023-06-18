// #define _POSIX_C_SOURCE 200809L

#include "common.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>


int send_msg(int fd, const char* buf) {
    char msg_head[64];
    int msg_size = strlen(buf);
    snprintf(msg_head, 1024, "%d|", msg_size);
    send(fd, msg_head, strlen(msg_head), 0);
    return send(fd, buf, msg_size, 0);
}

int rec_msg(int fd, char* buf, int maxlen) {
    int size = 0;
    char num;
    while(recv(fd, &num, 1, 0) > 0) {
        if(num >= '0' && num <= '9') 
            size = size*10 + num - '0';
        else
            break;
    }

    int received = 0;
    if(size > maxlen)
        received = recv(fd, buf, maxlen, 0);
    else if(size > 0)
        received = recv(fd, buf, size, 0);

    buf[received] = 0;
    return received;
}

void split_args(char* str, char** args, int argc) {
    for(int i = 0; i < argc; i++)
        args[i] = 0;
    
    int len = strlen(str);
    int j = 0;
    for(int i = 0; j < argc && i < len; i++) {
        if(str[i] != ' ') {
            if(args[j] == NULL) {
                args[j] = &str[i];
            }
        } else {
            str[i] = 0;
            j++;
        }
    }

    return;
}

void delete_dir(const char *path) {
    DIR *dr = NULL;
    dr = opendir(path);
    
    if (dr == NULL) {  
        printf("Error: Could not open directory: %s\n", strerror(errno));
        return;
    }

    struct dirent *de;
    struct stat temp_stat;
    char buffer[1024];
    while ((de = readdir(dr)) != NULL) {
        if(strncmp(de->d_name, ".", 1) == 0 || strncmp(de->d_name, "..", 2) == 0)
            continue;

        sprintf(buffer, "%s/%s", path, de->d_name);
        stat(buffer, &temp_stat);
        if(S_ISDIR(temp_stat.st_mode))
            delete_dir(buffer);
        else
            remove(buffer);
    }

    closedir(dr);
    remove(path);
}

