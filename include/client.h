#ifndef CLIENT_H
#define CLIENT_H

// Writes to stdout and a log file.
#define LOGD(format, ... ) \
do { \
    printf(format, ##__VA_ARGS__); \
    if(log_fd != -1) \
        dprintf(log_fd, format, ##__VA_ARGS__); \
} while(0)\


typedef struct file_node {
    char *path;
    time_t mtime;
    time_t last_check;
    bool is_dir;
} file_node_t;

struct file_llist_node {
    file_node_t *file;
    struct file_llist_node *next;
};

typedef enum {
    RECENT,
    OLD,
    NEW
} state_t;


/**
 * @brief Get the file node with the given path 
 * in the list starting with head.
 * 
 * @param head Head of the linked list.
 * @param path Path of the target file.
 * @return The node if found, or else NULL.
 */
struct file_llist_node* get_file_node(const struct file_llist_node *head, const char *path);

/**
 * @brief Insert a new node with the given path 
 * and mtime to the list starting with head. If a 
 * node with the same path exists, replace the 
 * mtime field. The list sorted alphabetically based
 * on the path.
 * 
 * @param head The head of the linked list.
 * @param path Path of the new file node.
 * @param mtime Last modification time of the file.
 * @return The new head of the list.
 */
struct file_llist_node* sorted_insert(struct file_llist_node *head, const char *path, time_t mtime, time_t last_check, bool is_dir);

/**
 * @brief Download the file in path from socket sd.
 * 
 * @param sd Socket descriptor connected to the server.
 * @param path Path of the file.
 * @return true for success, false for failure.
 */
bool download_file(int sd, const char* path);

/**
 * @brief Upload the file in path through socket sd.
 * 
 * @param sd Socket descriptor connected to the server.
 * @param path Path of the file.
 * @return true for success, false for failure.
 */
bool upload_file(int sd, const char* path, time_t mtime);

/**
 * @brief Recursively Sync all of the local files with 
 * the server.If a file does not exist in the server, 
 * upload it and add it to the file list. If the file 
 * exists both locally and on the server, and the server 
 * version is newer, upload the file to the server and 
 * refresh the list.
 * 
 * @param sd Socket descriptor connected to the server.
 * @param dir Directory to start from. (If NULL, start 
 * from the current directory)
 * @param list_head Head of the file list.
 * @return The new head of the list.
 */
struct file_llist_node* sync_files(int sd, const char *dir, struct file_llist_node *list_head, time_t last_update);

/**
 * @brief Check for any files existing in the file list
 * but not existing locally. If any were found, issue
 * a download request.
 * 
 * @param sd Socket descriptor connected to the server.
 * @param list_head Head of the file list.
 */
void check_untracked(int sd, struct file_llist_node **list_head, time_t last_update);

/**
 * @brief SIGPIPE handler. Just raises SIGINT to terminate
 * the main loop.
 */
void sigpipe_handler(int sig);

/**
 * @brief Main client function. Keeps checking for 
 * file changes on both ends.
 * 
 * @param dir Working directory of the client.
 * @param server_addr Address of the server.
 * @param port Port of the server.
 */
void run_client(const char *dir, const char *server_addr, char *username, int port);


#endif
