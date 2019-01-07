#include <unistd.h> 
#include <stdio.h> 
#include <sys/socket.h> 
#include <stdlib.h> 
#include <netinet/in.h> 
#include <string.h> 
#include <poll.h>
#include <stdbool.h>
#include <signal.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>

#define BUFFER_SIZE 8192
static char buffer[BUFFER_SIZE];
enum ncr_mode{
    NCR_MODE_PAIR,
    NCR_MODE_PAIRS,
    NCR_MODE_ROULETTE,
};
static inline int server_init(int* server_fd, unsigned short port, int somaxconn){
    if ((*server_fd = socket(AF_INET6, SOCK_STREAM, 0)) == 0) return -1;
    int opt = 1; 
    if (setsockopt(*server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))){
         fputs("Failed to set server socket options\n", stderr);
         if(close(*server_fd))fputs("Failed to close server socket\n", stderr);
         return -1;
    }
    struct sockaddr_in6 addr; 
    addr.sin6_family = AF_INET6; 
    addr.sin6_addr = in6addr_any;
    addr.sin6_flowinfo = 0;
    addr.sin6_port = htons(port); 
    if (bind(*server_fd, (struct sockaddr*)&addr, sizeof(addr))<0){
         fprintf(stderr, "Failed to bind server socket to localhost:%hd\n", port);
         if(close(*server_fd))fputs("Failed to close server socket\n", stderr);
         return -1;
    }
    if (listen(*server_fd, somaxconn) < 0){
        fprintf(stderr, "Failed to listen on localhost:%hd\n", port);
        if(close(*server_fd))fputs("Failed to close server socket\n", stderr);
        return -1;
    }
    return 0;
}
static inline int strtoushort(char* s, unsigned short* res){
    char* end = s + strlen(s);
    char* fin = end;
    long int r = strtol(s, &fin, 10);
    if(r<0) return -1;
    if(r >= (1<<sizeof(short)*8)) return -1;
    if(fin != end) return -1;
    *res = (unsigned short)r;
    return 0;
}
static inline int strtoint(char* str, int* res){
    char* end = str + strlen(str);
    char* fin = end;
    long int r = strtol(str, &fin, 10);
    if((size_t)r >= (size_t)1 << (sizeof(int)*8-1)) return -1;
    if(fin != end) return -1;
    *res = (int)r;
    return 0;
}
static inline int server_accept(int server_fd, int* client_fd){
    int addrlen = sizeof(struct sockaddr_in6); 
    struct sockaddr_in6 client_addr;
    *client_fd = accept(
        server_fd,
        (struct sockaddr *)&client_addr,
        (socklen_t*)&addrlen
    );
    if (*client_fd<0){
        fputs("Failed to accept client connection", stderr);    
        return -1;
    }
    return 0;
}
static inline int get_somaxconn(int* somaxconn){
    FILE* f = fopen("/proc/sys/net/core/somaxconn", "r");
    //ceil(lb(2^63-1)/lb(10)) = 20
    char buff[22];
    size_t cnt = fread(buff, 1, 21, f);
    if(cnt>0 && buff[cnt-1] == '\n')cnt--;
    if(cnt >= 21){
        fputs(
            "/proc/sys/net/core/somaxconn exceeds expected limit of 20 chars\n",
            stderr
        );
        return -1;
    }
    buff[cnt] = 0;
    if(strtoint(buff, somaxconn)){
        fputs(
            "/proc/sys/net/core/somaxconn couldn't be "
                "interpreted as an integer\n",
            stderr
        );
        return -1;
    }
    if(*somaxconn < 0){
        fputs("/proc/sys/net/core/somaxconn shouldn't be negative\n", stderr);
        return -1;
    }
    return 0;
}
static inline int parse_cmd_args(int argc, char** argv, unsigned short* port){
}
static inline void close_all(struct pollfd* polls, size_t client_count){
    if(close(polls[0].fd))fputs("Failed to close server socket\n", stderr);
    for(struct pollfd* p = polls + 1; p != polls + client_count + 1; p++){
        if(close(p->fd))fputs("Failed to close client socket\n", stderr);
    }
}
int transmit_data(struct pollfd* src, struct pollfd* tgt, char* buffer, size_t buffer_size){
     if(src->revents & POLLIN){
        size_t copy_count;
        do{
            copy_count = read(src->fd, buffer, buffer_size);
            if(copy_count == (size_t)-1){
                fputs("socket read error\n", stderr);
                return EXIT_FAILURE;
            }
            if(copy_count == 0) return 1;
            if(write(tgt->fd, buffer, copy_count) != copy_count) return 1;
        }while(copy_count == buffer_size);
    }
    return 0;
}
void print_help(){
    puts(
        "netcat roulette usage: \n"
        "ncr <port> <mode> [<args>]\n"
        "<mode>:\n"
        "  pair [-t <timeout>]\n"
        "  pairs [-c <count>] [-t <timeout>]\n"
        "  roulette [c <max_peers>] [-t <timeout>]"
    );
}
int roulette_mode(int server_fd){
}
int pair_mode(int server_fd){
    size_t client_count = 0;
    struct pollfd polls[3];
    for(struct pollfd* p = polls; p != polls + 3; p++){
        p->events = POLLIN | POLLERR;
    }
    polls[0].fd = server_fd;
    while(true) {
        poll(polls, client_count + 1, -1); 
        if(polls[0].revents & POLLIN && client_count < 2){
            if(server_accept(server_fd, &polls[client_count + 1].fd)){
                close_all(polls, client_count);
                return EXIT_FAILURE;
            }
            client_count++;
        }
        else if (client_count == 1 && polls[1].revents & (POLLIN)){
            close(polls[1].fd);
            client_count = 0;
        }
        if(client_count == 2){
            int r;
            r = transmit_data(&polls[1], &polls[2], buffer, BUFFER_SIZE);
            if (r == 0){
                r = transmit_data(&polls[2], &polls[1], buffer, BUFFER_SIZE);
                if(r == 0)continue;
                else if(r == 1){
                    close(polls[1].fd);
                    close(polls[2].fd);
                    client_count = 0;
                }
                else{
                    close_all(polls, client_count);
                    return EXIT_FAILURE;
                }
            }
            else if (r == 1){
                r = transmit_data(&polls[2], &polls[1], buffer, BUFFER_SIZE);
                if(r == -1){
                    close_all(polls, client_count);
                    return EXIT_FAILURE;
                }
                else{
                    close(polls[1].fd);
                    close(polls[2].fd);
                    client_count = 0;
                }
            }
            else{
                close_all(polls, client_count);
                return EXIT_FAILURE;
            }
        }
    }
    return EXIT_SUCCESS;
}
int main(int argc, char** argv){
    unsigned short port;
    enum ncr_mode mode;
    int somaxconn;
    int server_fd;
    if(argc < 3){
        print_help();
        return EXIT_FAILURE;
    }
    if(strtoushort(argv[1], &port)){
         fprintf(stderr, "Invalid local port %s\n",argv[1]);
         return -1;
    }
    const char* mode_str = argv[2];
    if(strcmp(mode_str, "pair")) mode = NCR_MODE_PAIR;
    else if(strcmp(mode_str, "pairs")) mode = NCR_MODE_PAIRS;
    else if(strcmp(mode_str, "roulette"))  mode = NCR_MODE_ROULETTE;
    else{
        print_help();
        return EXIT_FAILURE;
    }
    if(get_somaxconn(&somaxconn))return EXIT_FAILURE;
    if(server_init(&server_fd, port, somaxconn))return EXIT_FAILURE;
    switch (mode){
        case NCR_MODE_PAIR: return pair_mode(server_fd); 
        case NCR_MODE_PAIRS: return pair_mode(server_fd);
        case NCR_MODE_ROULETTE: return roulette_mode(server_fd);
    }
}