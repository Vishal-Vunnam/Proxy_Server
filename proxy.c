// YOUR CODE HERE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <time.h>
#include <sys/select.h>
#include <sys/time.h>
#include <stdatomic.h>

// #include "proxy.h"

#define BUFFER_SIZE 8192
#define MAX_CONNECTIONS 100
#define TIMEOUT_SEC 10

typedef struct { 
    int client_socket;
    struct sockaddr_in client_addr;
} client_info_t;

typedef struct {
    int server_socket;
    struct sockaddr_in server_addr;
} server_info_t;

typedef struct {
    int portno; 
    char *hostname;
    char *f_path; 
    char *msg_body; 
} req_info; 

client_info_t clients[MAX_CONNECTIONS];
atomic_int client_count = 0;

server_info_t server_info;


int parse_request(char * request, req_info * request_info) {
    req_info->f_path = strtok(request, "\t\r\n");
    req_info->hostname = strtok(NULL, "\t\r\n"); 
    req_info->portno = 80; 
}

int send_request(req_info *request_info) { 
    char response[1024]; 
    int body_len = strlen(msg_body)
}

int handle_request(int clientf) {   
    char buffer[BUFFER_SIZE] = {0}; 
    char request_copy[BUFFER_SIZE] = {0}; 

    ssize_t bytes_read = read(clientf, buffer, sizeof(buffer) - 1);

    if (bytes_read == 0) {
        printf("Client close connection\n");
    }
    else if (bytes_read < 0) { 
        printf("Read error: %s\n", strerror(errno));
    }

    buffer[bytes_read] = '\0';
    strcpy(request_copy, buffer); 
    printf("Received request: \n%s \n", buffer); 

    req_info request_info; 

    if (parse_request(buf, &req_info) == 0){ 
        printf("Request infoooooormation succesffully parsed");    
    }
    else { 
        printf("Invalid info format or error parsing"); 
        exit(0);
    }
}





int main(int argc, char **argv) { 
    int sockfd, portno, n;
    int serverlen;
    struct sockaddr_in serveraddr;
    struct hostent *server;
    char buffer[BUFFER_SIZE];
    int ttl; 

    if (argc < 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]); 
        exit(1); 
    }

    portno = atoi(argv[1]);
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("ERROR opening socket");
        exit(1);
    }

    if (argc  > 2) { 
            ttl = atoi(argv[2]); 
    }

    memset((char *) &serveraddr, 0, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons(portno);
    if (bind(sockfd, (struct sockaddr *) &serveraddr, sizeof(serveraddr)) < 0) {
        perror("ERROR on binding");
        close(sockfd);
        exit(1);
    }

    listen(sockfd, 5);
    printf("Proxy server listening on port %d\n", portno);

    while (1) {
        socklen_t clientlen = sizeof(clients[client_count].client_addr);
        clients[client_count].client_socket = accept(sockfd, (struct sockaddr *) &clients[client_count].client_addr, &clientlen);
        if (clients[client_count].client_socket < 0) {
            perror("ERROR on accept");
            continue;
        }
        printf("Accepted connection from %s:%d\n",
               inet_ntoa(clients[client_count].client_addr.sin_addr),
               ntohs(clients[client_count].client_addr.sin_port));
        client_count++;

        pid_t process_id = fork();
        if (process_id < 0) {
            perror("fork failed");
            close(clients[client_count].client_socket);
            continue;
        }

        if (process_id == 0) { 
            close(sockfd);
            handle_request()
            close(clients[client_count].client_socket);
            exit(0);
        } else { 
            close(clients[client_count].client_socket);
        }

    }

    close(sockfd);
    return 0;
}