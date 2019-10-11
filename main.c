#include <unistd.h>
#include <stdio.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <string.h>
#include <poll.h>
#include <sys/signalfd.h>
#include <stdbool.h>
#include <signal.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <limits.h>

#define OK 0
#define ERR -1
#define BUFFER_SIZE 8192

volatile int interrupted = 0;
static char buffer[BUFFER_SIZE];

static inline int
server_init(int* server_fd, unsigned short port, int somaxconn)
{
    if ((*server_fd = socket(AF_INET6, SOCK_STREAM, 0)) == 0) return -1;
    int opt = 1;
    if (setsockopt(
            *server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt,
            sizeof(opt))) {
        fputs("Failed to set server socket options\n", stderr);
        if (close(*server_fd)) fputs("Failed to close server socket\n", stderr);
        return -1;
    }
    struct sockaddr_in6 addr;
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_flowinfo = 0;
    addr.sin6_port = htons(port);
    if (bind(*server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(
            stderr, "Failed to bind server socket to localhost:%hd\n", port);
        if (close(*server_fd)) fputs("Failed to close server socket\n", stderr);
        return -1;
    }
    if (listen(*server_fd, somaxconn) < 0) {
        fprintf(stderr, "Failed to listen on localhost:%hd\n", port);
        if (close(*server_fd)) fputs("Failed to close server socket\n", stderr);
        return -1;
    }
    return 0;
}
static inline int server_accept(int server_fd, int* client_fd)
{
    int addrlen = sizeof(struct sockaddr_in6);
    struct sockaddr_in6 client_addr;
    *client_fd =
        accept(server_fd, (struct sockaddr*)&client_addr, (socklen_t*)&addrlen);
    if (*client_fd < 0) {
        fputs("Failed to accept client connection", stderr);
        return ERR;
    }
    return 0;
}
static inline int close_sockets(struct pollfd* begin, struct pollfd* end)
{
    int cse = 0;
    while (begin != end) {
        if (close(begin->fd)) cse++;
        begin++;
    }
    if (cse) {
        fprintf(
            stderr, "Failed to close %d client socket%s\n", cse,
            cse > 1 ? "s" : "");
        return ERR;
    }
    return OK;
}
static inline int close_socket(int fd)
{
    if (close(fd)) fputs("error closing client socket", stderr);
}
static inline void finalize(struct pollfd* polls, size_t client_count)
{
    if (close(polls[1].fd)) fputs("Failed to close server socket\n", stderr);
    close_sockets(polls + 2, polls + client_count + 2);
    free(polls);
}
static inline void
finalize_robin(struct pollfd* polls, size_t client_count, size_t* sent_stamps)
{
    finalize(polls, client_count);
    free(sent_stamps);
}
int isErrounuousInterrupt()
{
    return (interrupted & (SIGHUP | SIGPIPE)) ? OK : ERR;
}
int round_robin_mode(
    int signal_fd, int server_fd, size_t max_connections, int verbosity,
    int timeout, bool unidirectional)
{
    // TODO: find a better way to deduce the slice_count
    int timeout_slice_count = 10;
    timeout *= 100;
    size_t client_count = 0;
    struct pollfd* polls =
        malloc((2 + max_connections + 1) * sizeof(struct pollfd));
    size_t generation = 0;
    size_t idle_gens = 0;
    struct pollfd* clients = polls + 2;
    if (!polls) {
        fputs("allocation failiure", stderr);
        return ERR;
    }
    size_t* active_stamps = malloc(max_connections * sizeof(size_t));
    if (!active_stamps) {
        free(polls);
        fputs("allocation failiure", stderr);
        return ERR;
    }
    for (struct pollfd* p = polls; p != polls + 2; p++) {
        p->events = POLLIN;
    }
    polls[0].fd = signal_fd;
    polls[1].fd = server_fd;
    while (!interrupted) {
        int event_count = poll(polls, client_count + 2, timeout);
        generation++;
        if (polls[0].revents & POLLIN) break;
        if (polls[1].revents & POLLIN) {
            if (server_accept(server_fd, &clients[client_count].fd)) {
                finalize_robin(polls, client_count, active_stamps);
                return ERR;
            }
            if (client_count == max_connections) {
                size_t biggest_diff = 0;
                size_t oldest = 0;
                if (idle_gens == timeout_slice_count) {
                    size_t diff;
                    for (size_t i = 0; i < client_count; i++) {
                        if (active_stamps[i] > generation) {
                            diff = UINTPTR_MAX - active_stamps[i];
                            if (UINTPTR_MAX - generation < diff) {
                                // diff exceeds UINTPTR_MAX
                                oldest = i;
                                biggest_diff = UINTPTR_MAX;
                                break;
                            }
                            diff += generation;
                            if (diff > biggest_diff) {
                                biggest_diff = diff;
                                oldest = i;
                            }
                        }
                        else {
                            diff = active_stamps[i] - generation;
                            if (diff > biggest_diff) {
                                biggest_diff = diff;
                                oldest = i;
                            }
                        }
                    }
                }
                if (biggest_diff >= timeout_slice_count) {
                    if (verbosity > 1)
                        puts("kicking out longest idling socket due to "
                             "overflow");
                    close_socket(clients[oldest].fd);
                    clients[oldest] = clients[client_count];
                    clients[oldest].events = POLLIN;
                    active_stamps[oldest] = generation;
                }
                else {
                    if (verbosity > 1)
                        puts("rejecting new socket due to overflow");
                    close_socket(clients[client_count].fd);
                }
            }
            else {
                clients[client_count].events = POLLIN;
                client_count++;
            }
            event_count--;
        }
        struct pollfd* sender = clients;
        struct pollfd* receiver;
        bool action = false;
        while (event_count > 0) {
            bool found;
            while (sender != clients + client_count) {
                if (sender->revents & POLLIN) {
                    found = true;
                    break;
                }
                sender++;
            }
            if (!found) break;
            event_count--;
            ssize_t read_count;
            do {
                read_count = read(sender->fd, buffer, BUFFER_SIZE);
                if (interrupted) {
                    finalize_robin(polls, client_count, active_stamps);
                    return isErrounuousInterrupt();
                }
                if (read_count <= 0) {
                    close_socket(sender->fd);
                    client_count--;
                    *sender = clients[client_count];
                    active_stamps[sender - clients] =
                        active_stamps[client_count];
                }
                else {
                    action = true;
                    receiver = clients;
                    while (receiver != clients + client_count) {
                        if (receiver != sender) {
                            ssize_t write_count =
                                write(receiver->fd, buffer, read_count);
                            if (interrupted) {
                                finalize_robin(
                                    polls, client_count, active_stamps);
                                return isErrounuousInterrupt();
                            }
                            if (write_count != read_count) {
                                close_socket(receiver->fd);
                                client_count--;
                                *receiver = clients[client_count];
                                continue;
                            }
                        }
                        receiver++;
                    }
                }
            } while (read_count == BUFFER_SIZE);
            ;
        }
        if (action) {
            idle_gens = 0;
        }
        else if (idle_gens < timeout_slice_count) {
            idle_gens++;
        }
    }
    finalize_robin(polls, client_count, active_stamps);
}
int connection_mode(
    int signal_fd, int server_fd, size_t max_pending, int timeout,
    bool unidirectional, int verbosity)
{
    // TODO: allow n people in one closed down connection
    size_t client_count = 0;
    bool established = false;
    struct pollfd* polls =
        malloc((2 + max_pending + 1) * sizeof(struct pollfd));
    struct pollfd* clients = polls + 2;
    if (!polls) {
        fputs("allocation failiure", stderr);
        return ERR;
    }
    for (struct pollfd* p = polls; p != polls + 2; p++) {
        p->events = POLLIN;
    }
    polls[0].fd = signal_fd;
    polls[1].fd = server_fd;
    while (!interrupted) {
        int event_count =
            poll(polls, client_count + 2, established ? timeout * 1000 : 0);
        if (event_count == 0) {
            if (established) {
                if (verbosity > 0)
                    puts("ending established connection due to timeout");
                established = false;
                close_sockets(clients, clients + client_count);
                client_count = 0;
            }
            continue;
        }
        if (polls[0].revents & POLLIN) break;
        if (polls[1].revents & POLLIN) {
            if (server_accept(server_fd, &clients[client_count].fd)) {
                finalize(polls, client_count);
                return ERR;
            }
            if (established) {
                close_socket(clients[client_count].fd);
            }
            else {
                if (client_count == max_pending) {
                    if (verbosity > 1)
                        puts("kicking out pending socket due to overflow");
                    close_socket(clients[0].fd);
                    memmove(
                        clients, clients + 1,
                        (client_count - 1) * sizeof(struct pollfd));
                    clients[client_count - 1] = clients[client_count];
                    clients[client_count - 1].events = POLLIN;
                }
                else {
                    client_count++;
                    clients[client_count].events = POLLIN;
                }
            }
            event_count--;
        }
        else if (client_count == 1 && clients[0].revents & (POLLIN)) {
            close(clients[0].fd);
            client_count = 0;
        }
        if (client_count >= 2) {
            int r;
            struct pollfd* sender = clients;
            struct pollfd* receiver = clients;
            ssize_t read_count, write_count;
            bool found = false;
            while (event_count > 0 && client_count >= 2) {
                while (sender != clients + client_count) {
                    if (sender->revents & POLLIN) {
                        found = true;
                        break;
                    }
                    sender++;
                }
                // we might have closed a socket that also wanted to send,
                // so we just silently ignore the additional event in this case
                if (!found) break;
                found = false;
                event_count--;
                read_count = read(sender->fd, buffer, BUFFER_SIZE);
                if (interrupted) {
                    finalize(polls, client_count);
                    return isErrounuousInterrupt();
                }
                if (read_count <= 0) {
                    if (!established) {
                        close_socket(sender->fd);
                        client_count--;
                        // we can't just swap with the last as we want to keep
                        // the order
                        memmove(
                            sender, sender + 1,
                            (client_count - (sender - clients)) *
                                sizeof(struct pollfd));
                        if (receiver > sender) receiver--;
                        continue;
                    }
                    else {
                        established = false;
                        close_sockets(clients, clients + client_count);
                        client_count = 0;
                        break;
                    }
                }
                receiver = clients;
                while (receiver != clients + client_count) {
                    if (receiver == sender) {
                        receiver++;
                        continue;
                    }
                    write_count = write(receiver->fd, buffer, read_count);
                    if (interrupted) {
                        finalize(polls, client_count);
                        return isErrounuousInterrupt();
                    }
                    // while read errors shouldn't happen,
                    // write errors can regularly occur when the socked closed
                    if (write_count != read_count) {
                        if (!established) {
                            close_socket(receiver->fd);
                            client_count--;
                            // we can't just swap with the last as we want to
                            // keep the order
                            memmove(
                                receiver, receiver + 1,
                                (client_count - (receiver - clients)) *
                                    sizeof(struct pollfd));
                            if (sender > receiver) sender--;
                        }
                        else {
                            established = false;
                            close_sockets(clients, clients + client_count);
                            client_count = 0;
                        }
                    }
                    else {
                        if (!established) {
                            established = true;
                            if (verbosity > 0) puts("connection established");
                        }
                        if (client_count > 2) {
                            int rid, sid;
                            // this is for keeping the order
                            if (receiver > sender) {
                                rid = 1;
                                sid = 0;
                            }
                            else {
                                rid = 0;
                                sid = 1;
                            }
                            struct pollfd temp = clients[sid];
                            clients[sid] = *sender;
                            *sender = temp;
                            temp = clients[rid];
                            clients[rid] = *receiver;
                            *receiver = temp;
                            close_sockets(&clients[2], &clients[client_count]);
                            client_count = 2;
                        }
                        if (unidirectional) {
                            receiver->revents &= ~POLLIN;
                            receiver->events &= ~POLLIN;
                        }
                        while (read_count == BUFFER_SIZE) {
                            read_count = read(sender->fd, buffer, BUFFER_SIZE);
                            if (interrupted) {
                                finalize(polls, client_count);
                                return isErrounuousInterrupt();
                            }
                            if (read_count <= 0) {
                                established = false;
                                close_sockets(clients, clients + client_count);
                                client_count = 0;
                                break;
                            }
                            write_count =
                                write(receiver->fd, buffer, read_count);
                            if (interrupted) {
                                finalize(polls, client_count);
                                return isErrounuousInterrupt();
                            }
                            if (read_count != write_count) {
                                established = false;
                                close_sockets(clients, clients + client_count);
                                client_count = 0;
                                break;
                            }
                        }
                        break;
                    }
                    receiver++;
                }
            }
        }
    }
    finalize(polls, client_count);
    return isErrounuousInterrupt();
}
void signal_handler(int sig)
{
    interrupted = sig;
}
int setup_signal_handling(int* signal_fd)
{
    sigset_t sigset;
    int err = sigemptyset(&sigset);
    err |= sigaddset(&sigset, SIGINT);
    err |= sigaddset(&sigset, SIGTERM);
    err |= sigaddset(&sigset, SIGHUP);
    err |= sigaddset(&sigset, SIGPIPE);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGHUP, signal_handler);
    signal(SIGPIPE, signal_handler);
    if (err == 0) {
        *signal_fd = signalfd(-1, &sigset, 0);
    }
    if (err || *signal_fd < 0) {
        fputs("Failed to create signal file descriptor", stderr);
        return ERR;
    }
    return OK;
}
static inline int strtoushort(char* s, unsigned short* res)
{
    char* end = s + strlen(s);
    char* fin = end;
    long int r = strtol(s, &fin, 10);
    if (r < 0) return -1;
    if (r >= (1 << sizeof(short) * 8)) return -1;
    if (fin != end) return -1;
    *res = (unsigned short)r;
    return 0;
}
static inline int strtosize_s(const char* start, const char* end, size_t* res)
{
    const char* fin = end;
    long r = strtol(start, (char**)&fin, 10);
    if (r < 0 || (size_t)r > ULONG_MAX) return -1;
    if (fin != end) return -1;
    *res = (size_t)r;
    return 0;
}
static inline int strtosize(const char* str, size_t* res)
{
    return strtosize_s(str, str + strlen(str), res);
}
static inline int get_somaxconn(size_t* somaxconn)
{
    FILE* f = fopen("/proc/sys/net/core/somaxconn", "r");
    // ceil(lb(2^63-1)/lb(10)) = 20
    char buff[22];
    size_t cnt = fread(buff, 1, 21, f);
    if (cnt > 0 && buff[cnt - 1] == '\n') cnt--;
    if (cnt >= 21) {
        fputs(
            "/proc/sys/net/core/somaxconn exceeds expected limit of 20 chars\n",
            stderr);
        return -1;
    }
    buff[cnt] = 0;
    if (strtosize(buff, somaxconn)) {
        fputs(
            "/proc/sys/net/core/somaxconn couldn't be "
            "interpreted as an unsigned integer\n",
            stderr);
        return -1;
    }
    if (*somaxconn < 0) {
        fputs("/proc/sys/net/core/somaxconn shouldn't be negative\n", stderr);
        return -1;
    }
    return 0;
}
int parsePort(char* c, unsigned short* port)
{
    if (strtoushort(c, port)) {
        fprintf(stderr, "Invalid port '%s'\n", c);
        return ERR;
    }
    return OK;
}
int parseMaxValue(const char* c, size_t* val, const char* val_name)
{
    if (strtosize(c, val)) {
        fprintf(stderr, "Invalid value for %s '%s'\n", val_name, c);
        return ERR;
    }
    return OK;
}
int parseTimeout(const char* c, int* timeout)
{
    int timeout_seconds = 0;
    const char* i = c;
    unsigned long val;
    int last_found = -1;
    void print_error()
    {
        fprintf(stderr, "Invalid timeout '%s'\n", c);
    }
    void print_error_overflow()
    {
        fprintf(stderr, "timeout '%s' overflows the valid range\n", c);
    }
    int handle_unit(int vals_entry, size_t seconds)
    {
        if (last_found > vals_entry) {
            print_error();
            return ERR;
        }
        last_found = vals_entry;
        if (val > INT_MAX / seconds) {
            print_error_overflow();
            return ERR;
        }
        seconds *= val;
        if (INT_MAX - timeout_seconds < seconds) {
            print_error_overflow();
            return ERR;
        }
        timeout_seconds += seconds;
        return OK;
    }
    while (*i != '\0') {
        while (*i >= '0' && *i <= '9') i++;
        if (strtosize_s(c, i, &val)) {
            print_error();
            return -1;
        }
        switch (*i) {
            case 'd': handle_unit(0, 86400); break;
            case 'h': handle_unit(1, 3600); break;
            case 'm': handle_unit(2, 60); break;
            case 's': handle_unit(3, 1); break;
            default: print_error(); return ERR;
        }
        c = ++i;
    }
    *timeout = timeout_seconds;
    return OK;
}
void print_help()
{
    puts("netcat roulette 0.1\n"
         "ncr [--help] [-ruvvvh] [-m <max listeners>] [-t <timeout>] <port>\n"
         "  -r: round robin mode: send everything to everyone, no connection "
         "of 2\n"
         "  -u: unidirectional stream: when a former receiver sends it's "
         "ignored\n"
         "  -v: verbosity: show more output with each level, up to vvv\n"
         "  --help / -h: show this help, ignore all other options and exit\n"
         "  -t <timeout>: close a close connection when nothing is sent for "
         "<timeout>\n"
         "     <timeout> = [<days>d][<hours>h][<minutes>m][<seconds>s]\n"
         "  -m <max listeners>: max sockets in queue, any more are rejected\n");
}
int parse_cmd_args(
    int argc, char** argv, unsigned short* port, bool* round_robin,
    int* timeout, size_t* max_connections, bool* unidirectional, int* verbosity)
{
    *round_robin = false;
    *timeout = -1;
    *max_connections = 10;
    *unidirectional = false;
    *verbosity = 0;
    if (argc < 2) {
        print_help();
        return ERR;
    }
    int i = 1;
    char* a = argv[i];
    while (*a == '-') {
        if (!strcmp(argv[i], "--help")) {
            print_help();
            return ERR;
        }
        a++;
        if (*a == '\0') {
            fprintf(stderr, "Unknown option '-'\n", *a);
            return ERR;
        }
        do {
            if (*a == 'h') {
                print_help();
                return OK;
            }
            if (*a == 'u') {
                *unidirectional = true;
            }
            else if (*a == 'r') {
                *round_robin = true;
            }
            else if (*a == 'v') {
                *verbosity++;
                if (*verbosity == 4) {
                    fprintf(
                        stderr, "There are no more than 3 verbosity levels\n",
                        *a);
                }
            }
            else if (*a == 't') {
                a++;
                if (*a != '\0') {
                    fputs("Missing <timeout> argument for -t\n", stderr);
                    return ERR;
                }
                i++;
                if (parseTimeout(argv[i], timeout)) return ERR;
                break;
            }
            else if (*a == 'm') {
                char* val_name = (!round_robin) ? "max pending connections"
                                                : "max connections";
                a++;
                if (*a != '\0') {
                    fprintf(stderr, "Missing <%s> argument for -m\n", val_name);
                    return ERR;
                }
                i++;
                parseMaxValue(argv[i], max_connections, val_name);
                if (*max_connections < 2) {
                    fprintf(
                        stderr,
                        "<%s> must be greater than 2 to allow for "
                        "communication\n",
                        val_name);
                    return ERR;
                }
                break;
            }
            else {
                fprintf(stderr, "Unknown option '%c'\n", *a);
                return ERR;
            }
            a++;
        } while (*a != '\0');
        i++;
        a = argv[i];
    }
    if (argc <= i) {
        fputs("No port specified\n", stderr);
    }
    if (parsePort(argv[i], port)) return ERR;
    return OK;
}

int main(int argc, char** argv)
{
    unsigned short port;
    bool round_robin;
    int timeout;
    size_t max_connections;
    bool unidirectional;
    int verbosity;
    size_t somaxconn;
    if (parse_cmd_args(
            argc, argv, &port, &round_robin, &timeout, &max_connections,
            &unidirectional, &verbosity)) {
        return EXIT_FAILURE;
    }
    if (get_somaxconn(&somaxconn)) return EXIT_FAILURE;
    int server_fd;
    int signal_fd;
    if (setup_signal_handling(&signal_fd)) return EXIT_FAILURE;
    if (server_init(&server_fd, port, somaxconn)) return EXIT_FAILURE;
    int rc;
    if (round_robin) {
        rc = round_robin_mode(
            signal_fd, server_fd, max_connections, verbosity, timeout,
            unidirectional);
    }
    else {
        rc = connection_mode(
            signal_fd, server_fd, max_connections, verbosity, timeout,
            unidirectional);
    }
    if (close(signal_fd)) {
        fputs("Failed to close signal filedescriptor\n", stderr);
        rc = ERR;
    }
    return (rc == OK) ? EXIT_SUCCESS : EXIT_FAILURE;
}
