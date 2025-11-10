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
#include <openssl/md5.h>

#define BUFFER_SIZE 8192
#define MAX_CONNECTIONS 100
#define TIMEOUT_SEC 10
#define HASH_TABLE 256
#define CACHE_DIR "./cache/"
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


char blocked_hosts[100][256];



typedef struct { 
    uint64_t key; 
    char file_path[512];
    time_t timestamp;
    struct cache_entry_t *next;
} cache_entry_t; 

int cache_timeout = 60; 
int cached_count = 0; 
cache_entry_t* cache[HASH_TABLE] = { NULL }; 

// INIT Functions

int init_blocked_list() { 
    FILE *file = fopen("blocklist", "r");
    if (file == NULL) {
        perror("Could not open blocklist");
        return -1;
    }
    int i = 0;
    while (fgets(blocked_hosts[i], sizeof(blocked_hosts[i]), file) !=
              NULL && i < 100) {
          blocked_hosts[i][strcspn(blocked_hosts[i], "\n")] = 0; 
          i++;
     }
    fclose(file);
    return 0; 
}



// Sender Functions

int send_response(char *msg_body, int msg_len, int res_num, int client_ptr){ 
    char send_buffer[BUFFER_SIZE];
    memset(send_buffer, 0, sizeof(send_buffer));
    snprintf(send_buffer, sizeof(send_buffer),
             "HTTP/1.1 %d OK\r\n"
                "Content-Length: %d\r\n"
                "Connection: close\r\n"
                "\r\n",
             res_num, msg_len);

    client_info_t *client = &clients[client_ptr];
    printf("Sending response %d with body length %d to client %s:%d\n", res_num, msg_len,
           inet_ntoa(client->client_addr.sin_addr), ntohs(client->client_addr.sin_port));
    ssize_t bytes_sent = send(client->client_socket, send_buffer, strlen(send_buffer), 0);
    if (bytes_sent < 0) {
        perror("Send error");
        return -1;
    }
    bytes_sent = send(client->client_socket, msg_body, msg_len, 0);
    if (bytes_sent < 0) {
        perror("Send error");
        return -1;
    }

    return 0; 
}



int check_blocked(char *hostname) { 
    for (int i = 0; i < 100; i++) {
        if (strcmp(blocked_hosts[i], "") == 0) {
            break; 
        }
        if (strcmp(blocked_hosts[i], hostname) == 0) {
            printf("Hostname %s is blocked\n", hostname);
            return 1; 
        }
    }
    return 0; 
}

uint64_t generate_key(char *hostname) {
    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5((unsigned char*)hostname, strlen(hostname), digest);

    uint64_t key = 0;
    for (int i = 0; i < 8; i++) {
        key = (key << 8) | digest[i];
    }
    return key;
}

uint64_t hash_url(char *hostname) { 
    return generate_key(hostname);
}
    

int cache_file(char *hostname, char *contents) {
    uint64_t key = generate_key(hostname);
    int index = key % HASH_TABLE;

    cache_entry_t *new_entry = (cache_entry_t *)malloc(sizeof(cache_entry_t));
    if (!new_entry) {
        perror("Memory allocation failed for cache entry");
        return -1;
    }
    // check if already cached
    cache_entry_t *entry = cache[index];
    while (entry) {
        if (entry->key == key) {
            free(new_entry);
            printf("File for hostname %s is already cached\n", hostname);
            return 0;
        }
        entry = entry->next;
    }

    new_entry->key = key;
    // strncpy(new_entry->file_path, file_path, 512);
    new_entry->timestamp = time(NULL);
    new_entry->next = cache[index];
    cache[index] = new_entry;
    cached_count++;
    char file_name[64];
    snprintf(file_name, sizeof(file_name), CACHE_DIR "%lu", key);
    FILE *file = fopen(file_name, "w"); 
    if (file) { 
        // write contents to file 
        fwrite(contents, sizeof(char), strlen(contents), file);
        fclose(file);
    }

    printf("Cached file %s for hostname %s\n", file_name, hostname);
    return 0;
}

int delete_cached_file(char *hostname) { 
    uint64_t key = generate_key(hostname);
    int index = key % HASH_TABLE;
    cache_entry_t *entry = cache[index];
    cache_entry_t *prev = NULL;

    while (entry) {
        if (entry->key == key) {
            if (prev) {
                prev->next = entry->next;
            } else {
                cache[index] = entry->next;
            }
            char file_name[64];
            snprintf(file_name, sizeof(file_name), CACHE_DIR "%lu", key);
            if (remove(file_name) == 0) {
                printf("Deleted cached file %s for hostname %s\n", file_name, hostname);
            } else {
                perror("Error deleting cached file");
            }
            free(entry);
            cached_count--;
            return 0;
        }
        prev = entry;
        entry = entry->next;
    }
    printf("No cached file found for hostname %s to delete\n", hostname);
    return -1;
}

char *get_cached_file(char *hostname){ 
    if (cached_count == 0) {
        return NULL; 
    }
    uint64_t key = generate_key(hostname);
    int index = key % HASH_TABLE;
    cache_entry_t *entry = cache[index];
    while (entry) {
        if (entry->key == key) {
            if (difftime(time(NULL), entry->timestamp) > cache_timeout) {
                printf("Cache entry for hostname %s has expired\n", hostname);
                return NULL;
            }
            char file_name[64];
            snprintf(file_name, sizeof(file_name), CACHE_DIR "%lu", key);
            FILE *file = fopen(file_name, "r");
            if (file) {
                char buffer[BUFFER_SIZE];
                size_t bytes_read = fread(buffer, sizeof(char), sizeof(buffer) - 1, file);
                buffer[bytes_read] = '\0';
                fclose(file);
                printf("Retrieved cached file %s for hostname %s\n", file_name, hostname);
                char *result = strdup(buffer);
                return result;
            }
        }
        entry = entry->next;
    }
    return NULL;
}

int check_cache(char *hostname, char *file_path) {  
    // Generate MD5 hash of hostname
    uint64_t key = generate_key(hostname);

    int index = key % HASH_TABLE;
    cache_entry_t *entry = cache[index];
    time_t now = time(NULL);
    while (entry) {
        if (entry->key == key) {
            if (difftime(now, entry->timestamp) <= cache_timeout) {
                strncpy(file_path, entry->file_path, 512);
                return 1;
            } else {
                // Cache expired
                return 0;
            }
        }
        entry = entry->next;
    }
    return 0;
}

// Simple parser: extract hostname and path from HTTP GET line
int parse_request(char *request, req_info *info, int client_ptr) {
    // Example: GET http://example.com/index.html HTTP/1.1
    char method[16], url[512], version[32];
    // char msg_body[128]; 
    if (sscanf(request, "%15s %511s %31s", method, url, version) != 3)
        return -1;

    printf("Request Method: %s, URL: %s, Version: %s\n", method, url, version);

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

    // Checking for faulty requests or blocked hosts

    if (strncmp(method, "GET", 3) != 0) {
        char msg_body[128];
        strcpy(msg_body, "Only GET requests are supported\n");
        send_response(msg_body, strlen(msg_body), 404, client_ptr);
        fprintf(stderr, "Only GET requests are supported\n");
        return -1;
    }

    if (check_blocked(info->hostname)) {
        char msg_body[128];
        strcpy(msg_body, "Access to this host is blocked by the proxy\n");
        send_response(msg_body, strlen(msg_body), 403, client_ptr);
        fprintf(stderr, "Blocked access to host: %s\n", info->hostname);
        return -1;
    }

    info->portno = 80;
    return 0;
}

int handle_request(int client_ptr) {   
    char buffer[BUFFER_SIZE] = {0};
    int client_socket = clients[client_ptr].client_socket;
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
    if (parse_request(buffer, &request_info, client_ptr) == 0) { 

        printf("Parsed Request:\n Host: %s\n Path: %s\n Port: %d\n\n",
               request_info.hostname, request_info.f_path, request_info.portno);

        // Check cache 
        char cached_file[512];
        if (check_cache(request_info.hostname, cached_file)) {
            printf("Cache hit for host %s. Serving cached file %s\n",
                     request_info.hostname, cached_file);
            char *cached_content = get_cached_file(request_info.hostname);
            if (cached_content) {
                send_response(cached_content, strlen(cached_content), 200, client_ptr);
                free(cached_content);
                return 0;
            }
        } else {
            printf("Cache miss for host %s\n",  request_info.hostname);
        }

        // Forward request to target server
        printf("Forwarding request to %s:%d%s\n",
               request_info.hostname, request_info.portno, request_info.f_path);

        // Here you would add code to connect to the target server,
        // send the request, and relay the response back to the client.
        int server_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (server_sock < 0) {
            perror("Socket creation failed");
            return -1;
        }
        struct hostent *server = gethostbyname(request_info.hostname);
        if (server == NULL) {
            fprintf(stderr, "No such host: %s\n", request_info.hostname);
            close(server_sock);
            return -1;
        }
        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);
        server_addr.sin_port = htons(request_info.portno);



        
        if (connect(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
            perror("Connection to target server failed");
            close(server_sock);
            return -1;
        }
        // Send the original request to the target server
        ssize_t bytes_sent = send(server_sock, buffer, bytes_read, 0);
        if (bytes_sent < 0) {
            perror("Send to target server failed");
            close(server_sock);
            return -1;
        }
        // Receive response from target server and relay to client
        ssize_t resp_bytes;
        while ((resp_bytes = read(server_sock, buffer, sizeof(buffer))) > 0)
        {
            send_response(buffer, resp_bytes, 200, client_ptr);
            cache_file(request_info.hostname, buffer);
        }
        if (resp_bytes < 0) {
            perror("Read from target server failed");
        }
        close(server_sock);

    } else {

        printf("Invalid or unsupported request format.\n");
        char msg_body[128];
        strcpy(msg_body, "Invalid or unsupported request format.\n");
        send_response(msg_body, strlen(msg_body), 400, client_ptr);
        return -1;
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

    if (argc >= 3) {
        cache_timeout = atoi(argv[2]); 
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

    init_blocked_list();

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
            handle_request(client_count);
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
