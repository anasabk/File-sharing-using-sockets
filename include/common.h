#ifndef COMMON_H
#define COMMON_H


/**
 * @brief Send a message through the given socket 
 * in the form: "length|message". The length of
 * the message is obtained using strlen().
 * 
 * @param sd Socket descriptor. 
 * @param buf Buffer conatining the message.
 * @return Number of bytes sent.
 */
int send_msg(int sd, const char* buf);

/**
 * @brief Receive a message coming through the given
 * socket. The message should come in the form: 
 * "length|message", and only the message part will be 
 * written to buf.
 * 
 * @param sd Socket descriptor. 
 * @param buf A buffer to write the message to.
 * @param max_len The maximum length of the message to 
 * be read.
 * @return Number of bytes received.
 */
int rec_msg(int sd, char* buf, int max_len);

/**
 * @brief Parse multiple arguments seperated by whitespaces
 * in str into argc number of args, and assign each in 
 * args array.
 * 
 * @note Whitespaces are replaced with null characters to 
 * ensure that arguments are recognizable by string function.
 * 
 * @param str String containing whitespace seperated words.
 * @param args Array of char pointers to store arguments inside.
 * @param argc Max length of args array.
 */
void split_args(char* str, char** args, int argc);

/**
 * @brief 
 * 
 * @param path 
 */
void delete_dir(const char *path);

#endif
