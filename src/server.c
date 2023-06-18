#define _POSIX_C_SOURCE 200809L

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/fcntl.h>
#include <sys/signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <memory.h>
#include <dirent.h>
#include <utime.h>

#include "common.h"
#include "server.h"

#define DEF_MAX_CLIENT_NUM 1000


// Array of connected clients.
client_t **connected_list;

// Mutex for connected clients list.
pthread_mutex_t con_list_mut = PTHREAD_MUTEX_INITIALIZER;

// Conditional variable for connected clients list.
pthread_cond_t con_list_cond = PTHREAD_COND_INITIALIZER;

// Set of socket descriptors of connected clients
fd_set clients_sdset;

// Number of connected clients.
int connected_num;

// Atomic flag marking end of execution.
sig_atomic_t running_flag;

int max_client_num_g = 0;

// List of files being accessed.
file_node_t *open_file_list;

// Mutex of open files list.
pthread_mutex_t of_mut = PTHREAD_MUTEX_INITIALIZER;


job_type_t get_job_type(char *str) {
    for(int i = 0; i < 5; i++)
        if(strncmp(str, job_str[i], strlen(job_str[i])) == 0) 
            return (job_type_t)i;

    return UNKNOWN;
}

void add_job(
    client_t *issuer, 
    struct job_queue_s *job_queue, 
    job_type_t type, 
    char *arg) 
{
    if(issuer == NULL)
        return;

    job_t *new_job = (job_t*)malloc(sizeof(job_t));
    if(arg != NULL) {
        new_job->arg = (char*)calloc(strlen(arg) + 1, sizeof(char));
        strcpy(new_job->arg, arg);

    } else {
        new_job->arg = NULL;
    }

    new_job->type = type;
    new_job->issuer = issuer;

    struct job_queue_node *temp_node = 
        (struct job_queue_node *)malloc(sizeof(struct job_queue_node));
    temp_node->job = new_job;
    temp_node->next = NULL;

    if(job_queue->len == 0) {
        job_queue->tail = temp_node;
        job_queue->head = job_queue->tail;
    } else {
        job_queue->tail->next = temp_node;
        job_queue->tail = job_queue->tail->next;
    }

    job_queue->len++;
}


file_node_t* get_file_node(char *path) {
    file_node_t *temp_node = open_file_list;
    file_node_t *prev_node = NULL;
    while(temp_node != NULL) {
        pthread_mutex_lock(&temp_node->m);
        if(!temp_node->is_active) {
            // A non-active file node was encountered, 
            // remove it from the list.
            if(prev_node == NULL)
                open_file_list = open_file_list->next;
            else
                prev_node->next = temp_node->next;

            free(temp_node->path);
            pthread_mutex_unlock(&temp_node->m);
            pthread_mutex_destroy(&temp_node->m);
            pthread_cond_destroy(&temp_node->can_read);
            pthread_cond_destroy(&temp_node->can_write);
            free(temp_node);

            if(prev_node == NULL)
                temp_node = open_file_list;
            else
                temp_node = prev_node;

        } else if(strcmp(temp_node->path, path) == 0) {
            // The file was found.
            temp_node->is_active = true;
            pthread_mutex_unlock(&temp_node->m);
            return temp_node;

        } else 
            pthread_mutex_unlock(&temp_node->m);
    }

    // The file has no entry in the list, create a new one.
    temp_node = (file_node_t *)malloc(sizeof(file_node_t));
    temp_node->active_r = 0;
    temp_node->active_w = 0;
    temp_node->waiting_r = 0;
    temp_node->waiting_w = 0;
    temp_node->is_active = true;
    temp_node->path = (char*)calloc(strlen(path) + 1, sizeof(char));
    strcpy(temp_node->path, path);
    pthread_cond_init(&temp_node->can_read, NULL);
    pthread_cond_init(&temp_node->can_write, NULL);
    pthread_mutex_init(&temp_node->m, NULL);

    temp_node->next = open_file_list;
    open_file_list = temp_node;

    return temp_node;
}

void* worker(void* param) {
    struct job_queue_s *job_queue = (struct job_queue_s*)param;

    // Block SIGINT
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGINT);
    sigaddset(&sigset, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &sigset, NULL);

    job_t *job;
    while (true)
    {
        // Get a job.
        pthread_mutex_lock(&job_queue->mut);
        while(job_queue->len == 0) {
            if(running_flag == 0) {
                // No more jobs are comming, terminate.
                pthread_mutex_unlock(&job_queue->mut);
                pthread_exit(0);
            }
            pthread_cond_wait(&job_queue->cond, &job_queue->mut);
        }

        // Critical section
        job = pop_job(job_queue);

        pthread_cond_broadcast(&job_queue->cond);
        pthread_mutex_unlock(&job_queue->mut);


        if(job == NULL)
            break;

        // Lock the clients socket to prevent interference from other threads.
        pthread_mutex_lock(&job->issuer->sock_mut);
        switch (job->type)
        {
        case CONNECT:
            connect_user(job->issuer, job->arg);
            break;
        
        case GETLIST:
            get_list(job->issuer);
            break;
        
        case GETFILE:
            get_file(job->issuer, job->arg);
            break;
        
        case SETFILE:
            set_file(job->issuer, job->arg);
            break;

        case REMFILE:
            rem_file(job->issuer, job->arg);
            break;
        
        default:
            printf("Unkown job");
            break;
        }
        pthread_mutex_unlock(&job->issuer->sock_mut);

        // Free the job's memory.
        free(job->arg);
        free(job);        
    }

    pthread_exit(0);
}

job_t* pop_job(struct job_queue_s *job_queue) {
    if(job_queue->len == 0)
        return NULL;

    job_t* job = job_queue->head->job;
    struct job_queue_node *temp_node = job_queue->head;
    job_queue->head = temp_node->next;
    free(temp_node);

    job_queue->len--;

    if(job_queue->len == 0)
        job_queue->tail = NULL;

    return job;
}

bool connect_user(client_t *client, char *name) {
    if(client == NULL)
        return false;

    if(client->name != NULL) {
        // Send approval message.
        send_msg(client->sd, "no");
        return false;
    }

    client->name = (char*)malloc((strlen(name) + 1) * sizeof(char));
    strcpy(client->name, name);

    // Check if the client's directory exists
    struct stat sb;
    if (stat(name, &sb) != 0) {
        printf("The path %s does not exist. Creating a new one ...\n", name);
        mkdir(name, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    }

    // Send approval message.
    send_msg(client->sd, "ok");

    printf(
        "%s from ip %s port %d connected.\n", 
        name,
        inet_ntoa(client->addr.sin_addr), 
        ntohs(client->addr.sin_port)
    ); 

    return true;
}

void get_list_rec(client_t *client, char* dir) {
    DIR *dr = opendir(dir);
    if (dr == NULL) {  
        perror("Could not open current directory");
        return;
    }

    // Construct directory path.
    char buffer[512];
    sprintf(buffer, "%s/", dir);
    char *name_buf = &buffer[strlen(buffer)];


    // Read all of the entries in the directory.
    struct dirent *de;
    file_node_t *temp_node;
    struct stat temp_stat;
    char msg_buf[1024];
    char time_buf[128];
    while((de = readdir(dr)) != NULL) {
        // Ignore the current and parent directory entries.
        if(strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        sprintf(name_buf, "%s", de->d_name);
        if(stat(buffer, &temp_stat) < 0) {
            fprintf(stderr, "Error: Could not get file stat \"%s\": %s\n", buffer, strerror(errno));
            continue;
        }

        if(S_ISDIR(temp_stat.st_mode))
            get_list_rec(client, buffer);

        // Get the file's node in the client's open files list.
        pthread_mutex_lock(&of_mut);
        if((temp_node = get_file_node(buffer)) == NULL) {
            fprintf(stderr, "Error: Could not get the file \"%s\": %s", buffer, strerror(errno));
            pthread_mutex_unlock(&of_mut);
            continue;
        }
        pthread_mutex_unlock(&of_mut);


        // Reader-Writer synchronization.
        pthread_mutex_lock(&temp_node->m);
        while((temp_node->waiting_w + temp_node->active_w) > 0){
            temp_node->waiting_r++;
            pthread_cond_wait(&temp_node->can_read, &temp_node->m);
            temp_node->waiting_r--;
        }
        temp_node->active_r++;
        pthread_mutex_unlock(&temp_node->m);


        // Send the file's name with modification date.
        strftime(time_buf, 128, "%d/%m/%Y|%H:%M:%S", localtime(&temp_stat.st_mtime));
        if(S_ISDIR(temp_stat.st_mode))
            sprintf(msg_buf, "dir|%s %s", &buffer[strlen(client->name) + 1], time_buf);
        else 
            sprintf(msg_buf, "%s %s", &buffer[strlen(client->name) + 1], time_buf);
        send_msg(client->sd, msg_buf);


        pthread_mutex_lock(&temp_node->m);
        temp_node->active_r--;
        if(temp_node->active_r == 0) {
            if(temp_node->waiting_w > 0)
                pthread_cond_signal(&temp_node->can_write);
            else 
                temp_node->is_active = false;
        }
        pthread_mutex_unlock(&temp_node->m);
    }

    closedir(dr);
}

void get_list(client_t *client) {
    get_list_rec(client, client->name);
    send_msg(client->sd, "end");
}

bool get_file(client_t *client, char *path) {
    file_node_t *temp_node;
    char path_buf[strlen(client->name) + strlen(path) + 2];
    sprintf(path_buf, "%s/%s", client->name, path);

    pthread_mutex_lock(&of_mut);
    if((temp_node = get_file_node(path_buf)) == NULL) {
        fprintf(stderr, "Error: Could not get the file \"%s\": %s", path_buf, strerror(errno));
        send_msg(client->sd, "end");
        pthread_mutex_unlock(&of_mut);
        return false;
    }
    pthread_mutex_unlock(&of_mut);


    pthread_mutex_lock(&temp_node->m);
    while((temp_node->waiting_w + temp_node->active_w) > 0){
        temp_node->waiting_r++;
        pthread_cond_wait(&temp_node->can_read, &temp_node->m);
        temp_node->waiting_r--;
    }
    temp_node->active_r++;
    pthread_mutex_unlock(&temp_node->m);

    // Critical section
    char buffer[1025];
    struct stat temp_stat;
    stat(path_buf, &temp_stat);
    sprintf(buffer, "mtime %ld", temp_stat.st_mtime);
    send_msg(client->sd, buffer);

    int fd = open(path_buf, O_RDONLY);
    int size;
    if(fd == -1) {
        fprintf(
            stderr, 
            "Error: Could not open file \"%s\": %s", 
            path_buf, 
            strerror(errno)
        );
        send_msg(client->sd, "end");
        return false;

    } else while ((size = read(fd, buffer, 1024)) > 0) {
        buffer[size] = 0;
        send_msg(client->sd, buffer);
    }

    send_msg(client->sd, "end");
    close(fd);

    pthread_mutex_lock(&temp_node->m);
    temp_node->active_r--;
    if(temp_node->active_r == 0) {
        if(temp_node->waiting_w > 0)
            pthread_cond_signal(&temp_node->can_write);
        else 
            temp_node->is_active = false;
    }
    pthread_mutex_unlock(&temp_node->m);

    return true;
}

bool set_file(client_t *client, char *path) {
    file_node_t *temp_node;
    char path_buf[strlen(client->name) + strlen(path) + 2];
    sprintf(path_buf, "%s/%s", client->name, path);

    pthread_mutex_lock(&of_mut);
    if((temp_node = get_file_node(path_buf)) == NULL) {
        fprintf(stderr, "Error: Could not get the file \"%s\": %s", path_buf, strerror(errno));
        pthread_mutex_unlock(&of_mut);
        return false;
    }
    pthread_mutex_unlock(&of_mut);


    pthread_mutex_lock(&temp_node->m);
    while((temp_node->active_w + temp_node->active_r) > 0){
        temp_node->waiting_w++;
        pthread_cond_wait(&temp_node->can_write, &temp_node->m);
        temp_node->waiting_w--;
    }
    temp_node->active_w++;
    pthread_mutex_unlock(&temp_node->m);


    // Critical section
    if(strncmp(path, "dir|", 4) == 0) {
        sprintf(path_buf, "%s/%s", client->name, &path[4]);
        mkdir(path_buf, S_IRWXU);

    } else {
        char buffer[1025];
        send_msg(client->sd, "start");

        time_t mtime = 0;
        if(rec_msg(client->sd, buffer, 1024) > 0) {
            if(strncmp(buffer, "mtime", 5) == 0) {
                mtime = atol(&buffer[6]);
            }

            int fd = open(path_buf, O_WRONLY | O_APPEND | O_TRUNC | O_CREAT, S_IRWXU);
            if(fd == -1) {
                sprintf(buffer, "Error: Could not open file \"%s\": %s", path_buf, strerror(errno));
                send_msg(client->sd, buffer);
                return false;

            } else while (rec_msg(client->sd, buffer, 1024) > 0) {
                if(strncmp(buffer, "end", 3) == 0)
                    break;
                write(fd, buffer, strlen(buffer));
            }

            struct utimbuf temp_utim;
            temp_utim.modtime = mtime;
            temp_utim.actime = mtime;
            utime(path_buf, &temp_utim);

            close(fd);
        }
    }


    pthread_mutex_lock(&temp_node->m);
    temp_node->active_w--;
    if(temp_node->waiting_w > 0)
        pthread_cond_signal(&temp_node->can_write);
    else if(temp_node->waiting_r > 0)
        pthread_cond_broadcast(&temp_node->can_read);
    else
        temp_node->is_active = false;
    pthread_mutex_unlock(&temp_node->m);

    return true;
}

bool rem_file(client_t *client, char *path) {
    file_node_t *temp_node;
    char path_buf[strlen(client->name) + strlen(path) + 2];
    sprintf(path_buf, "%s/%s", client->name, path);

    // Get the file node from the client's list
    pthread_mutex_lock(&of_mut);
    if((temp_node = get_file_node(path_buf)) == NULL) {
        fprintf(stderr, "Error: Could not get the file \"%s\": %s", path_buf, strerror(errno));
        pthread_mutex_unlock(&of_mut);
        return false;
    }
    pthread_mutex_unlock(&of_mut);


    // Enter Reader-Writer synchronization.
    pthread_mutex_lock(&temp_node->m);
    while((temp_node->active_w + temp_node->active_r) > 0){
        temp_node->waiting_w++;
        pthread_cond_wait(&temp_node->can_write, &temp_node->m);
        temp_node->waiting_w--;
    }
    temp_node->active_w++;
    pthread_mutex_unlock(&temp_node->m);


    // Critical section
    if(strncmp(path, "dir|", 4) == 0) {
        sprintf(path_buf, "%s/%s", client->name, &path[4]);
        delete_dir(path_buf);

    } else {
        sprintf(path_buf, "%s/%s", client->name, path);
        remove(path_buf);
    }
    send_msg(client->sd, "ok");

    // Leave Reader-Writer synchronization.
    pthread_mutex_lock(&temp_node->m);
    temp_node->active_w--;
    if(temp_node->waiting_w > 0)
        pthread_cond_signal(&temp_node->can_write);
    else if(temp_node->waiting_r > 0)
        pthread_cond_broadcast(&temp_node->can_read);
    else
        temp_node->is_active = false;
    pthread_mutex_unlock(&temp_node->m);

    return true;
}


void add_client(int sd, struct sockaddr_in *addr, socklen_t addrlen) {
    for(int i = 0; i < max_client_num_g; i++){
        if(connected_list[i] == NULL) {
            connected_list[i] = (struct client_s *)malloc(sizeof(struct client_s));
            connected_list[i]->sd = sd;
            connected_list[i]->addrlen = addrlen;
            connected_list[i]->addr = *addr;
            connected_list[i]->name = NULL;
            pthread_mutex_init(&connected_list[i]->sock_mut, NULL);

            FD_SET(sd, &clients_sdset);

            connected_num++;

            break;
        }
    }

    return;
}

void* listener(void *param) {
    struct job_queue_s *job_queue = (struct job_queue_s*)param;

    // Block SIGINT.
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGINT);
    pthread_sigmask(SIG_BLOCK, &sigset, NULL);

    int activity;
    int max_fd = 0;
    char buffer[1025];
    job_type_t type = UNKNOWN;
    while (running_flag) {
        // Setup the clients' socket descriptor set.
        pthread_mutex_lock(&con_list_mut);
        while(connected_num < 1)
            pthread_cond_wait(&con_list_cond, &con_list_mut);

        // Reset client fd set
        FD_ZERO(&clients_sdset);
        max_fd = 0;

        //add child sockets to set 
        for (int i = 0; i < max_client_num_g; i++) {
            //if valid socket descriptor then add to read list 
            if(connected_list[i] != NULL){
                FD_SET(connected_list[i]->sd, &clients_sdset);

                //highest file descriptor number, need it for the select function 
                if(connected_list[i]->sd > max_fd)
                    max_fd = connected_list[i]->sd;
            }
        }

        pthread_mutex_unlock(&con_list_mut);

        
        // Detect any event client sockets. (Disconnection, incoming messages)
        activity = select(max_fd + 1, &clients_sdset, NULL, NULL, NULL);
       
        if ((activity < 0)) {
            if(errno != EINTR) {
                perror("select");
                return NULL;

            } else {
                continue;
            }
        }
  
        // Search for sockets with events.
        pthread_mutex_lock(&con_list_mut);
        int readlen;
        char *args[2];
        for (int i = 0; i < max_client_num_g; i++) {
            if (connected_list[i] != NULL && FD_ISSET(connected_list[i]->sd, &clients_sdset)) {
                if(pthread_mutex_trylock(&connected_list[i]->sock_mut) == EBUSY) {
                    // This socket is busy talking to a worker, ignore it.
                    continue;
                }
    
                if ((readlen = rec_msg(connected_list[i]->sd, buffer, 2048)) <= 0) {
                    // This client was disconnecting.
                    printf(
                        "\"%s\" disconnected, ip %s, port %d.\n",
                        connected_list[i]->name == NULL ? "Unknown" : connected_list[i]->name,
                        inet_ntoa(connected_list[i]->addr.sin_addr), 
                        ntohs(connected_list[i]->addr.sin_port)
                    );  
                         
                    //Remove the client from the server.
                    FD_CLR(connected_list[i]->sd, &clients_sdset);
                    close(connected_list[i]->sd);
                    free(connected_list[i]->name);
                    pthread_mutex_unlock(&connected_list[i]->sock_mut);
                    pthread_mutex_destroy(&connected_list[i]->sock_mut);

                    free(connected_list[i]);
                    connected_list[i] = NULL;
                    connected_num--;
                }  

                else {
                    // This client was sending a message.
                    // Parse the message.
                    split_args(buffer, args, 2);
                    type = get_job_type(args[0]);
                    switch (type)
                    {
                    case UNKNOWN:
                        break;

                    default:
                        if(type != CONNECT && connected_list[i]->name == NULL) {
                            // The client does not have a username, ignore it.
                            printf(
                                "Error: Request from ip %s port %d was denied: no username.\n", 
                                inet_ntoa(connected_list[i]->addr.sin_addr), 
                                ntohs(connected_list[i]->addr.sin_port)
                            ); 
                        }

                        pthread_mutex_lock(&job_queue->mut);
                        while(job_queue->len >= job_queue->max_len) 
                            pthread_cond_wait(&job_queue->cond, &job_queue->mut);

                        add_job(connected_list[i], job_queue, type, args[1]);
                        pthread_cond_broadcast(&job_queue->cond);
                        pthread_mutex_unlock(&job_queue->mut);
                        break;
                    }

                    pthread_mutex_unlock(&connected_list[i]->sock_mut);
                }
            }
        }  

        pthread_mutex_unlock(&con_list_mut);
    }
    
    pthread_exit(0);
}

void sigint_handler(int sig) {
    running_flag = 0;
}

void sigusr1_handler(int sig) {

}

void run_server(char *dir, int thread_pool_size, int port_num, int max_client_num) {
    // Set SIGINT handler.
    struct sigaction intact;
    memset(&intact, 0, sizeof(intact));
    intact.sa_handler = sigint_handler;
    sigaction(SIGINT, &intact, NULL);

    // Set SIGUSR1 handler for the listener.
    struct sigaction usr1act;
    memset(&usr1act, 0, sizeof(usr1act));
    usr1act.sa_handler = sigusr1_handler;
    sigaction(SIGUSR1, &usr1act, NULL);

    // Open the socket
    int serverfd = socket(AF_INET, SOCK_STREAM, 0);
    if(serverfd < 0)  {
        perror("socket creation failed");
        return;
    }

    //set master socket to allow multiple connections, 
    //this is just a good habit, it will work without this. 
    int opt = true;
    if(setsockopt(serverfd, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt)) < 0 ) {
        perror("setsockopt");
        close(serverfd);
        return;  
    }

    // Initialize the socket info
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_num);
    addr.sin_addr.s_addr = INADDR_ANY;

    // Bind the socket to the given address
    if(bind(serverfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("socket binding failed");
        close(serverfd);
        return;
    }

    // Set the socket as listener
    if(listen(serverfd, 100) < 0) {
        perror("socket listening failed");
        close(serverfd);
        return;
    }


    // Initialize the job queue
    struct job_queue_s job_queue;
    memset(&job_queue, 0, sizeof(job_queue));
    job_queue.max_len = thread_pool_size;
    pthread_cond_init(&job_queue.cond, NULL);
    pthread_mutex_init(&job_queue.mut, NULL);


    // Check if the server's directory exists
    struct stat sb;
    if (stat(dir, &sb) != 0) {
        printf("The Path does not exists. Creating a new one ...\n");
        mkdir(dir, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    }
    chdir(dir);


    // Initialize maximum number of client
    if(max_client_num == 0) 
        max_client_num_g = DEF_MAX_CLIENT_NUM;
    else    
        max_client_num_g = max_client_num;
    
    connected_list = (client_t**)calloc(max_client_num_g, sizeof(client_t*));


    running_flag = 1;

    // Create workers
    pthread_t workers[thread_pool_size];
    memset(workers, 0, thread_pool_size*sizeof(pthread_t));
    for(int i = 0; i < thread_pool_size; i++)
        pthread_create(&workers[i], NULL, worker, (void*)&job_queue);


    // Create listener thread
    pthread_t listener_thread;
    pthread_create(&listener_thread, NULL, listener, (void*)&job_queue);


    printf("Waiting for clients ...\n");
    // Loop for accepting new clients
    struct sockaddr_in temp_addr;
    socklen_t templen = 0;
    int temp_sd;
    while(running_flag) {
        memset(&temp_addr, 0, sizeof(temp_addr));
        temp_sd = accept(serverfd, (struct sockaddr*)&temp_addr, &templen);
        if(temp_sd < 0 && errno != EINTR) {
            perror("Could not connect to client");
            continue;
        }

        pthread_mutex_lock(&con_list_mut);
        while(connected_num >= max_client_num_g)
            pthread_cond_wait(&con_list_cond, &con_list_mut);
        add_client(temp_sd, &temp_addr, templen);
        pthread_cond_signal(&con_list_cond);
        pthread_mutex_unlock(&con_list_mut);
        pthread_kill(listener_thread, SIGUSR1);
    }


    printf("Terminating\n");

    //Exiting
    running_flag = 0;

    // Kill the listener
    pthread_cancel(listener_thread);
    pthread_join(listener_thread, NULL);

    //Close the server's socket
    close(serverfd);

    // Check that all workers have terminated
    pthread_mutex_lock(&job_queue.mut);
    pthread_cond_broadcast(&job_queue.cond);
    pthread_mutex_unlock(&job_queue.mut);
    for(int i = 0; i < thread_pool_size; i++)
        pthread_join(workers[i], NULL);

    // Close all of the connected clients' sockets
    for(int i = 0; i < max_client_num_g; i++)
        if(connected_list[i] != 0) {
            if(connected_list[i]->name != NULL)
                free(connected_list[i]->name);
            close(connected_list[i]->sd);
            pthread_mutex_destroy(&connected_list[i]->sock_mut);
            free(connected_list[i]);
        }
    free(connected_list);

    file_node_t *temp_node;
    while(open_file_list != NULL) {
        free(open_file_list->path);
        pthread_mutex_destroy(&open_file_list->m);
        pthread_cond_destroy(&open_file_list->can_read);
        pthread_cond_destroy(&open_file_list->can_write);
        temp_node = open_file_list->next;
        free(open_file_list);
        open_file_list = temp_node;
    }

    pthread_cond_destroy(&job_queue.cond);
    pthread_mutex_destroy(&job_queue.mut);

    printf("Terminated successfuly\n");
}

int main(int argc, char **argv) {
    if(argc < 4 || argc > 5) {
        fprintf(stderr, "Invalid argument number: %d\n", argc);
        return 0;
    }

    run_server(argv[1], atoi(argv[2]), atoi(argv[3]), atoi(argv[5]));
}
