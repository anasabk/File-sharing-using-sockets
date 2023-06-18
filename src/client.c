#define _GNU_SOURCE

#include <sys/socket.h>
#include <signal.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdlib.h>
#include <netdb.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <sys/fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <utime.h>

#include "common.h"
#include "client.h"


int log_fd = -1;

struct file_llist_node* get_file_node(const struct file_llist_node *head, const char *path) {
    const struct file_llist_node *temp = head;

    int comp;   
    while(temp != NULL) {
        comp = strcmp(temp->file->path, path);
        if(comp == 0) {
            return (struct file_llist_node*)temp;
        
        } else if(comp > 0) {
            return NULL;
        
        } else {
            temp = temp->next;
        }
    }

    return NULL;
}

struct file_llist_node* sorted_insert(struct file_llist_node *head, const char *path, time_t mtime, time_t last_check, bool is_dir) {
    struct file_llist_node *prev = NULL;
    struct file_llist_node *temp = head;
    struct file_llist_node *new_head = head;
    
    int comp;
    while(temp != NULL) {
        comp = strcmp(temp->file->path, path);
        if(comp == 0 && temp->file->is_dir == is_dir) {
            temp->file->last_check = last_check;
            temp->file->mtime = mtime;
            break;
        
        } else if(comp > 0) {
            struct file_llist_node *node = (struct file_llist_node *)malloc(sizeof(struct file_llist_node));
            node->file = (file_node_t*)calloc(1, sizeof(file_node_t));
            node->file->path = (char*)calloc(strlen(path) + 1, sizeof(char));
            strcpy(node->file->path, path);
            node->file->last_check = last_check;
            node->file->mtime = mtime;
            node->file->is_dir = is_dir;
            node->next = NULL;
            
            if(prev == NULL) {
                new_head = node;
                new_head->next = head;
            } else {
                node->next = temp;
                prev->next = node;
            }
            
            break;
        
        } else {
            prev = temp;
            temp = temp->next;
        }
    }

    if(temp == NULL) {
        struct file_llist_node *node = (struct file_llist_node *)malloc(sizeof(struct file_llist_node));
        node->file = (file_node_t*)calloc(1, sizeof(file_node_t));
        node->file->path = (char*)calloc(strlen(path) + 1, sizeof(char));
        strcpy(node->file->path, path);
        node->file->last_check = last_check;
        node->file->mtime = mtime;
        node->file->is_dir = is_dir;
        node->next = NULL;

        if(prev != NULL)
            prev->next = node;
        else
            new_head = node;
    }
    
    return new_head;
}

bool download_file(int sd, const char* path) {
    LOGD("Downloading \"%s\"\n", path);
    char msg_buf[strlen(path) + 9];
    sprintf(msg_buf, "GETFILE %s", path);
    send_msg(sd, msg_buf);

    // Get last modification time.
    char buffer[1025];
    time_t mtime;
    if(rec_msg(sd, buffer, 1024) > 0) {
        if(strncmp(buffer, "mtime", 5) == 0) {
            mtime = atol(&buffer[6]);
        }
    }

    // Open the file
    int file_fd = open(path, O_WRONLY | O_APPEND | O_TRUNC | O_CREAT, S_IRWXU);
    int size;
    if(file_fd == -1) {
        LOGD("Error: Could not open file \"%s\": %s\n", path, strerror(errno));
        while(rec_msg(sd, buffer, 1024) > 0)
            if(strncmp(buffer, "end", 3) == 0)
                break;
        return false;

    } else while ((size = rec_msg(sd, buffer, 1024)) > 0) {
        // Keep reading until end message arrives.
        if(strncmp(buffer, "end", 3) == 0)
            break;
        write(file_fd, buffer, size);
    }

    // Set the modification time of the file.
    struct utimbuf temp_utim;
    temp_utim.modtime = mtime;
    temp_utim.actime = mtime;
    utime(path, &temp_utim);

    close(file_fd);
    LOGD("Downloaded successfuly.\n");
    return true;
}

bool upload_file(int sd, const char* path, time_t mtime) {
    LOGD("Uploading \"%s\"\n", path);
    char msg_buf[strlen(path) + 9];
    sprintf(msg_buf, "SETFILE %s", path);
    send_msg(sd, msg_buf);

    // Wait for server's approval to start.
    char buffer[1025];
    rec_msg(sd, buffer, 1024);
    if(strncmp(buffer, "start", 5) != 0) {
        LOGD("Error: Server refused to start uploading\n");
        return false;
    }

    // Send modification time.
    sprintf(buffer, "mtime %ld", mtime);
    send_msg(sd, buffer);

    int file_fd = open(path, O_RDONLY);
    int size;
    if(file_fd == -1) {
        LOGD( 
            "Error: Could not open file \"%s\": %s\n", 
            path, 
            strerror(errno)
        );
        send_msg(sd, "end");
        return false;

    } else while ((size = read(file_fd, buffer, 1024)) > 0) {
        buffer[size] = 0;
        send_msg(sd, buffer);
    }

    // Send end message.
    send_msg(sd, "end");
    close(file_fd);
    LOGD("Uploaded successfuly.\n");
    return true;
}

bool remove_file(int sd, const char *path) {
    LOGD("Removing \"%s\"\n", path);
    char msg_buf[strlen(path) + 9];
    sprintf(msg_buf, "REMFILE %s", path);
    send_msg(sd, msg_buf);

    // Wait for server's approval to start.
    char buffer[1025];
    rec_msg(sd, buffer, 1024);
    if(strncmp(buffer, "ok", 5) != 0) {
        LOGD("Error: Server refused to delete the file\n");
        return false;
    }

    LOGD("Removed successfuly.\n");
    return true;
}

struct file_llist_node* sync_files(int sd, const char *dir, struct file_llist_node *list_head, time_t last_update) {
    DIR *dr = NULL;
    char buffer[512];
    char *name_buf;

    if(dir == NULL) {
        dr = opendir(".");
        buffer[0] = 0;
        name_buf = buffer;

    } else {
        dr = opendir(dir);
        sprintf(buffer, "%s/", dir);
        name_buf = &buffer[strlen(buffer)];
    }

    if (dr == NULL) {  
        LOGD("Error: Could not open current directory: %s\n", strerror(errno));
        return list_head;
    }


    struct dirent *de;
    struct stat temp_stat;
    struct file_llist_node *temp_node;
    while((de = readdir(dr)) != NULL) {
        if (strncmp(de->d_name, ".", 1) == 0 || strncmp(de->d_name, "..", 2) == 0 ||
            (dir == NULL && strncmp(de->d_name, "log.txt", 7) == 0))
            continue;

        sprintf(name_buf, "%s", de->d_name);
        if(stat(buffer, &temp_stat) < 0) {
            LOGD("Error: Could not get file stat \"%s\": %s\n", buffer, strerror(errno));
            continue;
        }

        temp_node = get_file_node(list_head, buffer);

        if(S_ISDIR(temp_stat.st_mode)) {
            if(temp_node == NULL) {
                char msg_buf[strlen(buffer) + 9];
                sprintf(msg_buf, "SETFILE dir|%s", buffer);
                send_msg(sd, msg_buf);

            } else if(difftime(temp_node->file->last_check, last_update) <= 0) {
                delete_dir(buffer);
                continue;
            }

            sync_files(sd, buffer, list_head, last_update);
            continue;
        }

        if(temp_node == NULL) {
            // New locally and not in the server
            if(!upload_file(sd, buffer, temp_stat.st_mtime)) 
                LOGD("Error: File %s is not in the server and could not upload\n", buffer);

        } else {
            // The file existed both locally and in the server 
            int comp = difftime(temp_node->file->last_check, last_update);
            if(comp > 0) {
                // The file exists both in the server and locally.
                comp = difftime(temp_node->file->mtime, temp_stat.st_mtime);
                if(comp > 0){
                    // The file is newer in the server.
                    if(!download_file(sd, buffer))
                        LOGD("Error: File %s is newer on the server but could not download\n", temp_node->file->path);

                } else if(comp < 0 && !upload_file(sd, buffer, temp_stat.st_mtime))
                    LOGD("Error: File %s is newer locally but could not upload\n", temp_node->file->path);

            } else if(comp < 0) {
                // The file was deleted from the server.
                remove(buffer);
            }
        }
    }

    closedir(dr);
    return list_head;
}

void check_untracked(int sd, struct file_llist_node **list_head, time_t last_update) {
    struct file_llist_node *prev_node = NULL;
    struct file_llist_node *temp_node = *list_head;
    struct stat s;
    while (temp_node != NULL) {
        if (stat(temp_node->file->path, &s) != 0) {
            // file doesn't exist
            if(difftime(temp_node->file->last_check, last_update) > 0) {
                // File still exists in the server.
                if(difftime(temp_node->file->mtime, last_update) > 0) {
                    if(temp_node->file->is_dir) {
                        mkdir(temp_node->file->path, S_IRWXU);

                    } else if(!download_file(sd, temp_node->file->path)) {
                        LOGD("Error: File %s does not exist locally and failed to download\n", temp_node->file->path);
                    }
                } else {
                    remove_file(sd, temp_node->file->path);
                }

            } else {
                if(prev_node != NULL){
                    prev_node->next = temp_node->next;
                    free(temp_node->file->path);
                    free(temp_node->file);
                    free(temp_node);
                    temp_node = prev_node;

                } else {
                    *list_head = (*list_head)->next;
                    free(temp_node->file->path);
                    free(temp_node->file);
                    free(temp_node);
                    temp_node = *list_head;
                    continue;
                }
            }

        }

        prev_node = temp_node;
        temp_node = temp_node->next;
    }
}

void sigpipe_handler(int sig) {
    raise(SIGINT);
}

void run_client(const char *dir, const char *server_addr, char *username, int port) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    // Check if the client's directory exists
    struct stat sb;
    if (stat(dir, &sb) != 0) {
        LOGD("The Path does not exists. Creating a new one ...\n");
        mkdir(dir, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    }
    chdir(dir);

    log_fd = open("log.txt", O_CREAT | O_APPEND | O_WRONLY, S_IRWXU);
    if(log_fd < 0)  {
        LOGD("Error: Log file creation failed: %s\n", strerror(errno));
    } else {
        LOGD("Log file created/opened ...\n");
    }


    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0)  {
        LOGD("Error: socket creation failed: %s\n", strerror(errno));
        return;
    } else {
        LOGD("\nsocket created ...\n");
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if(server_addr != NULL)
        addr.sin_addr.s_addr = inet_addr(server_addr);
    else{
        char hostbuffer[256];
        char *IPbuffer;
        struct hostent *host_entry;

        gethostname(hostbuffer, sizeof(hostbuffer));
        host_entry = gethostbyname(hostbuffer);

        // Converting Internet network address to ASCII string
        IPbuffer = inet_ntoa(*((struct in_addr*)host_entry->h_addr_list[0]));
        addr.sin_addr.s_addr = inet_addr(IPbuffer);
    }

    if(connect(fd, &addr, sizeof(addr)) != 0) {
        LOGD("Error: socket connection failed: %s\n", strerror(errno));
        close(fd);
        return;

    } else {
        LOGD("connected to server ...\n");
    }


    struct itimerval timer;
    timer.it_interval.tv_sec = 10;
    timer.it_interval.tv_usec = 0;
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = 500000;
    if(setitimer(ITIMER_REAL, &timer, NULL)) {
        LOGD("Error: setitimer failed: %s\n", strerror(errno));
    }


    struct sigaction pipeact;
    memset(&pipeact, 0, sizeof(pipeact));
    pipeact.sa_handler = sigpipe_handler;
    sigaction(SIGPIPE, &pipeact, NULL);
    

    int sig;
    char buffer[1025];
    sprintf(buffer, "CONNECT %s", username);
    send_msg(fd, buffer);
    rec_msg(fd, buffer, 1024);
    if(strncmp(buffer, "ok", 3) != 0) {
        LOGD("Error: Could not connect, aborting ...\n");
        close(fd);
        return;
    }


    struct file_llist_node *file_list = NULL;

    char *args[3] = {NULL, NULL, NULL};
    struct tm time_buf;
    memset(&time_buf, 0, sizeof(time_buf));
    time_t last_update = 0;
    time_t temp_last_update = 0;
    time_t mtime;
    int read_size;
    while (sigwait(&set, &sig) == 0) {
        if(sig == SIGINT)
            break;

        temp_last_update = time(NULL);
        send_msg(fd, "GETLIST");
        LOGD("sending GETLIST\n");
        while((read_size = rec_msg(fd, buffer, 1024)) > 0) {
            LOGD("response: %s\n", buffer);
            if(strncmp(buffer, "end", 3) == 0)
                break;

            split_args(buffer, args, 2);
            strptime(args[1], "%d/%m/%Y|%H:%M:%S", &time_buf);
            mtime = mktime(&time_buf);

            if(strncmp(args[0], "dir|", 4) == 0)
                file_list = sorted_insert(file_list, &args[0][4], mtime, temp_last_update, true);
            else
                file_list = sorted_insert(file_list, args[0], mtime, temp_last_update, false);
        }

        file_list = sync_files(fd, NULL, file_list, last_update);
        check_untracked(fd, &file_list, last_update);

        last_update = temp_last_update;
    }
    
    LOGD("exiting\n");
    struct file_llist_node *temp;
    while(file_list != NULL) {
        free(file_list->file->path);
        free(file_list->file);
        temp = file_list->next;
        free(file_list);
        file_list = temp;
    }

    close(fd);
    LOGD("exited successfuly\n");
    
    close(log_fd);
}

int main(int argc, char **argv) {
    if(argc < 4 || argc > 5) {
        LOGD("Invalid number of arguments, aborting ...\n");
    }

    run_client(argv[1], argv[4], argv[3], atoi(argv[2]));
}
