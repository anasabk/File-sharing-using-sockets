#ifndef SERVER_H
#define SERVER_H

#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdbool.h>


/**
 * @brief Enum representing the types of jobs 
 * that are supported by the system.
 */
typedef enum job_type_s {
    UNKNOWN = -1,
    CONNECT,    // "CONNECT USERNAME"
    GETLIST,    // "GETLIST"
    GETFILE,    // "GETFILE FILENAME"
    SETFILE,    // "SETFILE FILENAME"
    REMFILE
} job_type_t;

/**
 * @brief The equivalent string for every job type.
 */
const char *job_str[] = {
    "CONNECT",
    "GETLIST",
    "GETFILE",
    "SETFILE",
    "REMFILE"
};

/**
 * @brief A node of a linked list storing information 
 * about a recently accessed file of a client.
 */
typedef struct file_node_s {
    // path of the file.
    char *path;

    // Synchronization fields.

    int active_r;
    int waiting_r;
    int active_w;
    int waiting_w;
    pthread_mutex_t m;
    pthread_cond_t can_read;
    pthread_cond_t can_write;

    // Next node in the list
    struct file_node_s *next;
    
    // Inactive nodes are deleted immediately.
    bool is_active;
} file_node_t;

/**
 * @brief Struct storing a client's information.
 */
typedef struct client_s {
    // Socket descriptor.
    int sd;

    // Socket address struct.
    struct sockaddr_in addr;
    // Socket address length.
    socklen_t addrlen;

    // Socket mutex.
    pthread_mutex_t sock_mut;

    // Client username.
    char *name;
} client_t;

/**
 * @brief Struct Storing a job's information.
 */
typedef struct {
    // Issuer client of the job.
    client_t *issuer;

    // Job type.
    job_type_t type;

    // String argument needed for the job.
    char *arg;
} job_t;

/**
 * @brief Node of job queue.
 */
struct job_queue_node {
    job_t *job;
    struct job_queue_node *next;
};

/**
 * @brief Queue storing waiting jobs.
 */
struct job_queue_s {
    struct job_queue_node *head;
    struct job_queue_node *tail;
    int len;
    int max_len;
    pthread_mutex_t mut;
    pthread_cond_t cond;
};


/**
 * @brief A worker thread that takes jobs from 
 * the job queue and performs them.
 * 
 * @note Keeps running until "running flag" is set
 * to 0 by the handler of SIGINT.
 * 
 * @return No return value.
 */
void* worker(void*);

/**
 * @brief Returns the job type associated with
 * the given string.
 * 
 * @param str Job name.
 * @return The job typr.
 */
job_type_t get_job_type(char *str);

/**
 * @brief Add a new job to job_queue
 * 
 * @param issuer Issuer of the job.
 * @param job_queue The job queue.
 * @param type Type of the job.
 * @param arg Argument of the job.
 */
void add_job(
    client_t *issuer, 
    struct job_queue_s *job_queue, 
    job_type_t type, 
    char *arg
);

/**
 * @brief Pop a job from the queue.
 * 
 * @param job_queue The job queue.
 * @return The job popped from the queue.
 */
job_t* pop_job(struct job_queue_s *job_queue);

/**
 * @brief Get a file node for the given client and path.
 * If the node does not exist in the client;s list, create 
 * it, and then return if.
 * 
 * @param client The owner of the file.
 * @param path The path of the file.
 * @return The file node.
 */
file_node_t* get_file_node(char *path);

/**
 * @brief Assign name to client.
 * 
 * @note Clients without names cannot send commands.
 * 
 * @param client The client to be associated with the name.
 * @param name Name of the client.
 * @return true if the name was assigned, and false if the 
 * client was null, or already had a name.
 */
bool connect_user(client_t *client, char *name);

/**
 * @brief Send all of the file entries found in the
 * clients directory to the client. This is the function
 * for GETLIST job.
 * 
 * @param client The owner of the files.
 */
void get_list(client_t *client);

/**
 * @brief Send the file in path to client.
 * 
 * @param client Owner of the file.
 * @param path Path of the file.
 * @return true if successful, false if an error occured.
 */
bool get_file(client_t *client, char *path);

/**
 * @brief Receive the file in path from client.
 * 
 * @param client Owner of the file.
 * @param path Path of the file.
 * @return true if successful, false if an error occured.
 */
bool set_file(client_t *client, char *path);

bool rem_file(client_t *client, char *path);

/**
 * @brief Add a client to the connected clients list.
 * 
 * @param sd Socket descriptor.
 * @param addr pointer to socket address struct.
 * @param addrlen Socket address length.
 */
void add_client(int sd, struct sockaddr_in *addr, socklen_t addrlen);

/**
 * @brief A thread that listens for all of the 
 * connected clients' input through their sockets. 
 * Handles adding new jobs and removing disconnected 
 * clients.
 * 
 * @note Can be interrupted by SIGUSR1 to get out of 
 * blocking state.
 * @note Keeps running until "running flag" is set
 * to 0 by the handler of SIGINT.
 * 
 * @param param Pointer to the job queue.
 */
void* listener(void *param);

/**
 * @brief SIGINT handler. Sets "running_flag" to 0.
 */
void sigint_handler(int sig);

/**
 * @brief The server's main running thread. Keeps waiting for
 * new connections.
 * 
 * @param dir Working directory of the server.
 * @param thread_pool_size Max number of threads allowed.
 * @param port_num Socket's port number.
 */
void run_server(char *dir, int thread_pool_size, int port_num, int max_client_num);


#endif
