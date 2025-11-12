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
#include <sys/wait.h>
#include <sys/mman.h>  // For shared memory
#include <semaphore.h>  // For semaphores

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

typedef struct cache_entry_t { 
    uint64_t key; 
    char file_path[512];
    time_t timestamp;
    int next_index;  // Changed from pointer to index for shared memory
    int in_use;      // Flag to indicate if this entry is active
} cache_entry_t; 

// Shared memory structures
typedef struct {
    int hash_table[HASH_TABLE];  // Indices into cache_entries array (-1 = empty)
    cache_entry_t cache_entries[MAX_CONNECTIONS * 10];  // Pool of cache entries
    int cached_count;
    int cache_timeout;
    sem_t cache_lock;  // Semaphore for synchronization
} shared_cache_t;

shared_cache_t *shared_cache = NULL;

// SIG Functions
//---------------------------------------------


volatile sig_atomic_t running = 1;

void sigchld_handler(int signo) {
    (void)signo;
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

void sigint_handler(int signo) {
    (void)signo;
    // printf("\n[!] Caught SIGINT/SIGTERM — shutting down proxy server...\n");

    for (int i = 0; i < client_count; i++) {
        if (clients[i].client_socket > 0)
            close(clients[i].client_socket);
    }

    // Cleanup shared memory
    if (shared_cache) {
        sem_destroy(&shared_cache->cache_lock);
        munmap(shared_cache, sizeof(shared_cache_t));
    }

    kill(0, SIGTERM);
    running = 0;
}

// Initialize shared memory for cache
int init_shared_cache(int timeout) {
    // Create shared memory region
    shared_cache = mmap(NULL, sizeof(shared_cache_t),
                        PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    
    if (shared_cache == MAP_FAILED) {
        perror("mmap failed");
        return -1;
    }

    // Initialize the cache structure
    memset(shared_cache, 0, sizeof(shared_cache_t));
    
    for (int i = 0; i < HASH_TABLE; i++) {
        shared_cache->hash_table[i] = -1;  // -1 means empty
    }
    
    for (int i = 0; i < MAX_CONNECTIONS * 10; i++) {
        shared_cache->cache_entries[i].in_use = 0;
        shared_cache->cache_entries[i].next_index = -1;
    }
    
    shared_cache->cached_count = 0;
    shared_cache->cache_timeout = timeout;
    
    // Initialize semaphore for process-safe access
    if (sem_init(&shared_cache->cache_lock, 1, 1) < 0) {  // 1 = shared between processes
        perror("sem_init failed");
        munmap(shared_cache, sizeof(shared_cache_t));
        return -1;
    }
    
    printf("Shared cache initialized with timeout: %d seconds\n", timeout);
    return 0;
}

// INIT Functions
//---------------------------------------------

int init_blocked_list() { 
    FILE *file = fopen("blocklist", "r");
    if (file == NULL) {
        perror("Could not open blocklist");
        return -1;
    }
    int i = 0;
    while (fgets(blocked_hosts[i], sizeof(blocked_hosts[i]), file) != NULL && i < 100) {
        blocked_hosts[i][strcspn(blocked_hosts[i], "\n")] = 0; 
        i++;
    }
    fclose(file);
    return 0; 
}

// Sender Functions
//---------------------------------------------

int send_error(char *msg_body, int msg_len, int res_num, int client_ptr){ 
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

int send_response(char *msg_body, int msg_len, int res_num, int client_ptr) { 
    client_info_t *client = &clients[client_ptr];
    printf("Sending response %d with body length %d to client %s:%d\n", res_num, msg_len,
           inet_ntoa(client->client_addr.sin_addr), ntohs(client->client_addr.sin_port));
    
    ssize_t bytes_sent = send(client->client_socket, msg_body, msg_len, 0);
    if (bytes_sent < 0) {
        perror("Send error");
        return -1;
    }
    return 0; 
}

// Helper Functions

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

// Cache Functions with Shared Memory
//---------------------------------------------

uint64_t generate_key(char *hostpath) {
    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5((unsigned char*)hostpath, strlen(hostpath), digest);

    uint64_t key = 0;
    for (int i = 0; i < 8; i++) {
        key = (key << 8) | digest[i];
    }
    return key;
}

// Find a free cache entry slot
int find_free_entry() {
    for (int i = 0; i < MAX_CONNECTIONS * 10; i++) {
        if (!shared_cache->cache_entries[i].in_use) {
            return i;
        }
    }
    return -1;  // No free slots
}

int cache_file(char *hostname, char *file_path, char *contents) {
    char hostpath[512]; 
    snprintf(hostpath, sizeof(hostpath), "%s%s", hostname, file_path);
    uint64_t key = generate_key(hostpath);
    int index = key % HASH_TABLE;

    // Lock the cache for thread-safe access
    sem_wait(&shared_cache->cache_lock);

    // Check if already cached
    int entry_idx = shared_cache->hash_table[index];
    while (entry_idx != -1) {
        if (shared_cache->cache_entries[entry_idx].key == key) {
            sem_post(&shared_cache->cache_lock);
            printf("File for hostname %s is already cached\n", hostname);
            return 0;
        }
        entry_idx = shared_cache->cache_entries[entry_idx].next_index;
    }

    // Find free entry slot
    int new_idx = find_free_entry();
    if (new_idx == -1) {
        sem_post(&shared_cache->cache_lock);
        printf("Cache is full, cannot cache file\n");
        return -1;
    }

    // Initialize new entry
    shared_cache->cache_entries[new_idx].key = key;
    strncpy(shared_cache->cache_entries[new_idx].file_path, file_path, 512);
    shared_cache->cache_entries[new_idx].timestamp = time(NULL);
    shared_cache->cache_entries[new_idx].in_use = 1;
    shared_cache->cache_entries[new_idx].next_index = shared_cache->hash_table[index];
    shared_cache->hash_table[index] = new_idx;
    shared_cache->cached_count++;

    sem_post(&shared_cache->cache_lock);

    // Write to file (outside the lock for performance)
    char file_name[64];
    snprintf(file_name, sizeof(file_name), CACHE_DIR "%lu", key);
    FILE *file = fopen(file_name, "w"); 
    if (file) { 
        fwrite(contents, sizeof(char), strlen(contents), file);
        fclose(file);
        printf("Cached file %s for hostpath %s (key: %lu)\n", file_name, hostpath, key);
    } else {
        perror("Failed to write cache file");
        return -1;
    }

    return 0;
}

int delete_cache_file(char *hostname, char *file_path) { 
    char hostpath[512];
    snprintf(hostpath, sizeof(hostpath), "%s%s", hostname, file_path);
    uint64_t key = generate_key(hostpath);
    int index = key % HASH_TABLE;

    sem_wait(&shared_cache->cache_lock);
    
    int entry_idx = shared_cache->hash_table[index];
    int prev_idx = -1;
    
    while (entry_idx != -1) {
        cache_entry_t *entry = &shared_cache->cache_entries[entry_idx];
        if (entry->key == key && entry->in_use) {
            if (prev_idx == -1) {
                shared_cache->hash_table[index] = entry->next_index;
            } else {
                shared_cache->cache_entries[prev_idx].next_index = entry->next_index;
            }
            entry->in_use = 0;  // Mark as free
            shared_cache->cached_count--;
            sem_post(&shared_cache->cache_lock);
            
            // Delete file
            char file_name[64];
            snprintf(file_name, sizeof(file_name), CACHE_DIR "%lu", key);
            if (remove(file_name) == 0) {
                printf("Deleted cached file %s for hostname %s\n", file_name, hostname);
            } else {
                perror("Failed to delete cache file");
            }
            return 0;
        }
        prev_idx = entry_idx;
        entry_idx = entry->next_index;
    }

    sem_post(&shared_cache->cache_lock);
    return -1;  // Not found
}


char *get_cached_file(char *hostname, char *file_path) { 
    if (shared_cache->cached_count == 0) {
        return NULL; 
    }
    
    char hostpath[512];
    snprintf(hostpath, sizeof(hostpath), "%s%s", hostname, file_path);
    uint64_t key = generate_key(hostpath);
    int index = key % HASH_TABLE;

    sem_wait(&shared_cache->cache_lock);

    int entry_idx = shared_cache->hash_table[index];
    while (entry_idx != -1) {
        cache_entry_t *entry = &shared_cache->cache_entries[entry_idx];
        if (entry->key == key && entry->in_use) {
            if (difftime(time(NULL), entry->timestamp) > shared_cache->cache_timeout) {
                printf("Cache entry for hostname %s has expired\n", hostname);
                sem_post(&shared_cache->cache_lock);
                delete_cache_file(hostname, file_path);
                return NULL;
            }
            
            sem_post(&shared_cache->cache_lock);
            
            // Read file outside the lock
            char file_name[64];
            snprintf(file_name, sizeof(file_name), CACHE_DIR "%lu", key);
            FILE *file = fopen(file_name, "r");
            if (file) {
                fseek(file, 0, SEEK_END);
                long file_size = ftell(file);
                fseek(file, 0, SEEK_SET);
                
                char *buffer = malloc(file_size + 1);
                if (buffer) {
                    size_t bytes_read = fread(buffer, 1, file_size, file);
                    buffer[bytes_read] = '\0';
                    fclose(file);
                    printf("Retrieved cached file %s for hostname %s\n", file_name, hostname);
                    return buffer;
                }
                fclose(file);
            }
            return NULL;
        }
        entry_idx = entry->next_index;
    }

    sem_post(&shared_cache->cache_lock);
    return NULL;
}

int check_cache(char *hostname, char *file_path) {  
    char hostpath[512];
    snprintf(hostpath, sizeof(hostpath), "%s%s", hostname, file_path);
    uint64_t key = generate_key(hostpath);
    int index = key % HASH_TABLE;

    sem_wait(&shared_cache->cache_lock);

    int entry_idx = shared_cache->hash_table[index];
    time_t now = time(NULL);
    
    while (entry_idx != -1) {
        cache_entry_t *entry = &shared_cache->cache_entries[entry_idx];
            if (entry->key == key && entry->in_use) {
                printf("Timestamp now: %ld, entry timestamp: %ld, timeout: %d\n",
                       now, entry->timestamp, shared_cache->cache_timeout);
                if (difftime(now, entry->timestamp) <= shared_cache->cache_timeout) {
                    sem_post(&shared_cache->cache_lock);
                    return 1;
                } else {
                    // Cache expired
                    sem_post(&shared_cache->cache_lock);
                    return -1;
                }
            }
        entry_idx = entry->next_index;
    }

    sem_post(&shared_cache->cache_lock);
    return 0;
}

// Request Handling Functions
//---------------------------------------------


int parse_request(char *request, req_info *info, int client_ptr) {
    char method[16], url[512], version[32];
    
    if (sscanf(request, "%15s %511s %31s", method, url, version) != 3)
        return -1;

    printf("Request Method: %s, URL: %s, Version: %s\n", method, url, version);

    char *host_start = strstr(url, "://");
    host_start = host_start ? host_start + 3 : url;

    char *path_start = strchr(host_start, '/');
    if (path_start) {
        strncpy(info->hostname, host_start, path_start - host_start);
        info->hostname[path_start - host_start] = '\0';
        strncpy(info->f_path, path_start, sizeof(info->f_path));
    } else {
        strcpy(info->hostname, host_start);
        strcpy(info->f_path, "/");
    }

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
        send_error(msg_body, strlen(msg_body), 403, client_ptr);
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
        if (check_cache(request_info.hostname, request_info.f_path)) {
            printf("Cache hit for host %s%s\n", request_info.hostname, request_info.f_path);
            char *cached_content = get_cached_file(request_info.hostname, request_info.f_path);
            if (cached_content) {
                send_response(cached_content, strlen(cached_content), 200, client_ptr);
                free(cached_content);
                return 0;
            }
        } else {
            printf("Cache miss for host %s%s\n", request_info.hostname, request_info.f_path);
        }

        // Forward request to target server
        printf("Forwarding request to %s:%d%s\n",
               request_info.hostname, request_info.portno, request_info.f_path);

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
        
        // Accumulate complete response before caching
        char *full_response = malloc(BUFFER_SIZE * 10);
        size_t total_size = 0;
        size_t allocated_size = BUFFER_SIZE * 10;
        
        ssize_t resp_bytes;
        while ((resp_bytes = read(server_sock, buffer, sizeof(buffer))) > 0) {
            // Expand buffer if needed
            if (total_size + resp_bytes > allocated_size) {
                allocated_size *= 2;
                full_response = realloc(full_response, allocated_size);
            }
            
            memcpy(full_response + total_size, buffer, resp_bytes);
            total_size += resp_bytes;
            
            // Send chunk to client immediately
            send_response(buffer, resp_bytes, 200, client_ptr);
        }
        
        if (resp_bytes < 0) {
            perror("Read from target server failed");
        }
        
        // Cache the complete response
        if (total_size > 0) {
            full_response[total_size] = '\0';
            cache_file(request_info.hostname, request_info.f_path, full_response);
        }
        
        free(full_response);
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

// Main Function
//---------------------------------------------

int main(int argc, char **argv) { 
    if (argc < 2) {
        fprintf(stderr, "usage: %s <port> [cache_timeout]\n", argv[0]); 
        exit(1); 
    }

    int portno = atoi(argv[1]);
    int cache_timeout = 60;  // default
    
    if (argc >= 3) {
        cache_timeout = atoi(argv[2]); 
    }

    // Initialize shared cache BEFORE forking
    if (init_shared_cache(cache_timeout) < 0) {
        fprintf(stderr, "Failed to initialize shared cache\n");
        exit(1);
    }

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

    // Install SIGCHLD handler
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        perror("sigaction");
        close(sockfd);
        exit(1);
    }

    // Install SIGINT + SIGTERM handler
    struct sigaction sa_int;
    sa_int.sa_handler = sigint_handler;
    sigemptyset(&sa_int.sa_mask);
    sa_int.sa_flags = 0;
    sigaction(SIGINT, &sa_int, NULL);
    sigaction(SIGTERM, &sa_int, NULL);

    int opt = 1; 
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEADDR) failed");
        close(sockfd);
        exit(1);
    }

    if (bind(sockfd, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0) {
        perror("ERROR on binding");
        close(sockfd);
        exit(1);
    }

    listen(sockfd, 5);
    printf("Proxy server listening on port %d\n", portno);
    printf("Cache timeout: %d seconds\n", cache_timeout);

    init_blocked_list();

    while (1) {
        int curr_client = client_count;
        socklen_t clientlen = sizeof(clients[curr_client].client_addr);
        clients[curr_client].client_socket =
            accept(sockfd, (struct sockaddr *)&clients[curr_client].client_addr, &clientlen);

        if (clients[curr_client].client_socket < 0) {
            perror("ERROR on accept");
            continue;
        }

        printf("Accepted connection from %s:%d\n",
               inet_ntoa(clients[curr_client].client_addr.sin_addr),
               ntohs(clients[curr_client].client_addr.sin_port));

        pid_t pid = fork();
        if (pid < 0) {
            if (errno == EINTR) continue; 
            perror("fork failed");
            close(clients[curr_client].client_socket);
            continue;
        }

        if (pid == 0) { // child
            close(sockfd);
            handle_request(curr_client);
            close(clients[curr_client].client_socket);
            exit(0);
        } else { // parent
            close(clients[curr_client].client_socket);
            client_count++;
        }
    }

    close(sockfd);
    
    // Cleanup shared memory
    sem_destroy(&shared_cache->cache_lock);
    munmap(shared_cache, sizeof(shared_cache_t));
    
    return 0;
}