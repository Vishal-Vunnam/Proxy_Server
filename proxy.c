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

#define BUFFER_SIZE 8192
#define MAX_CONNECTIONS 100
#define TIMEOUT_SEC 10

typedef struct { 
    int client_socket;
    struct sockaddr_in client_addr;
} client_info_t;

typedef struct {
    int portno; 
    char hostname[256];
    char f_path[512]; 
} req_info; 

client_info_t clients[MAX_CONNECTIONS];
atomic_int client_count = 0;

int send_response(char *msg_body, int msg_len, int res_num){ 


    
}

// Simple parser: extract hostname and path from HTTP GET line
int parse_request(char *request, req_info *info) {
    // Example: GET http://example.com/index.html HTTP/1.1
    char method[16], url[512], version[32];
    if (sscanf(request, "%15s %511s %31s", method, url, version) != 3)
        return -1;

    if (strncmp(method, "GET", 3) != 0) {
        fprintf(stderr, "Only GET requests are supported\n");
        return -1;
    }

    // Extract hostname and path
    char *host_start = strstr(url, "://");
    host_start = host_start ? host_start + 3 : url; // skip "http://"

    char *path_start = strchr(host_start, '/');
    if (path_start) {
        strncpy(info->hostname, host_start, path_start - host_start);
        info->hostname[path_start - host_start] = '\0';
        strncpy(info->f_path, path_start, sizeof(info->f_path));
    } else {
        strcpy(info->hostname, host_start);
        strcpy(info->f_path, "/");
    }

    info->portno = 80;
    return 0;
}

int handle_request(int client_socket) {   
    char buffer[BUFFER_SIZE] = {0};
    ssize_t bytes_read = read(client_socket, buffer, sizeof(buffer) - 1);

    if (bytes_read == 0) {
        printf("Client closed connection\n");
        return -1;
    } else if (bytes_read < 0) { 
        perror("Read error");
        return -1;
    }

    buffer[bytes_read] = '\0';
    printf("\n--- Received Request ---\n%s\n-------------------------\n", buffer);

    req_info request_info;
    if (parse_request(buffer, &request_info) == 0) { 
        printf("Parsed Request:\n Host: %s\n Path: %s\n Port: %d\n\n",
               request_info.hostname, request_info.f_path, request_info.portno);
    } else { 
        printf("Invalid or unsupported request format.\n");
    }

    return 0;
}

int main(int argc, char **argv) { 
    if (argc < 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]); 
        exit(1); 
    }

    int portno = atoi(argv[1]);
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("ERROR opening socket");
        exit(1);
    }

    struct sockaddr_in serveraddr;
    memset(&serveraddr, 0, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons(portno);

    if (bind(sockfd, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0) {
        perror("ERROR on binding");
        close(sockfd);
        exit(1);
    }

    listen(sockfd, 5);
    printf("Proxy server listening on port %d\n", portno);

    while (1) {
        socklen_t clientlen = sizeof(clients[client_count].client_addr);
        clients[client_count].client_socket =
            accept(sockfd, (struct sockaddr *)&clients[client_count].client_addr, &clientlen);

        if (clients[client_count].client_socket < 0) {
            perror("ERROR on accept");
            continue;
        }

        printf("Accepted connection from %s:%d\n",
               inet_ntoa(clients[client_count].client_addr.sin_addr),
               ntohs(clients[client_count].client_addr.sin_port));

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork failed");
            close(clients[client_count].client_socket);
            continue;
        }

        if (pid == 0) { // child
            close(sockfd);
            handle_request(clients[client_count].client_socket);
            close(clients[client_count].client_socket);
            exit(0);
        } else { // parent
            close(clients[client_count].client_socket);
            client_count++;
        }
    }

    close(sockfd);
    return 0;
}
