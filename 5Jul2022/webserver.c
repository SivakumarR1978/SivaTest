#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <sys/socket.h>

typedef struct connect_stat connect_stat_t;
typedef void (*page_process_func)(connect_stat_t*);

struct connect_stat {
    int fd;
    char name[64];
    char age[64];
    struct epoll_event _ev;
    int status;                 // 0 - not logged in 1 - logged in
    page_process_func handler;  // Processing functions for different pages
};

static int epfd = 0;

static const char* main_header = "HTTP/1.0 200 OK\r\nServer: Martin Server\r\nContent-Type: text/html\r\nConnection: Close\r\n";

// Get default connect_stat_t node
connect_stat_t* get_stat(int fd);
void set_nonblock(int fd);

// The server socket is returned successfully, and - 1 is returned for failure
int init_server(const char *ip, unsigned short port);
// Initialize the connection and wait for the browser to send a request
void add_event_to_epoll(int newfd);

void do_http_request(connect_stat_t* p);
void do_http_respone(connect_stat_t* p);

void welcome_response_handler(connect_stat_t* p);
void commit_respone_handler  (connect_stat_t* p);

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "%s:please input [ip][port]!\n", argv[0]);
        exit(1);
    }

    int sock = init_server(argv[1], atoi(argv[2]));
    if (sock < 0) { exit(2); }

    // 1. Create epoll
    epfd = epoll_create(256);
    if (epfd < 0) {
        fprintf(stderr, "epoll_create(): failed! reason: %s!\n", strerror(errno));
        exit(3);
    }

    struct epoll_event _ev; // epoll structure filling
    connect_stat_t* stat = get_stat(sock);
    _ev.events = EPOLLIN;   // The initial listening event is read
    _ev.data.ptr = stat;

    // trusteeship
    epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &_ev);  //Add the sock to epfd to listen for read events

    struct epoll_event revents[64]; // The array of events when there are events

    int timeout = -1;   // -1 - block indefinitely 0 - return immediately
    int count = 0;      // Number of events
    int done = 0;

    while (!done) {
        switch ((count = epoll_wait(epfd, revents, sizeof(revents)/sizeof(revents[0]), -1)))
        {
        case -1:
            fprintf(stderr, "epoll_wait(): failed! reason: %s!\n", strerror(errno));
            exit(4);
            break;
        case 0:
            fprintf(stderr, "timeout!\n");
            exit(5);
        default:
            for (int i = 0; i < count; ++i) {
                connect_stat_t* p = (connect_stat_t*)revents[i].data.ptr;
                int fd = p->fd;
                // If it is a server fd and a read event, the connection is received
                if (fd == sock && (revents[i].events & EPOLLIN)) {
                    struct sockaddr_in client;
                    int client_len = sizeof(struct sockaddr_in);
                    int newfd = accept(sock, (struct sockaddr*)(&client), &client_len);
                    if (newfd < 0) {
                        fprintf(stderr, "accept(): failed! reason: %s!\n", strerror(errno));
                        continue;
                    }
                    printf("get a new client: %d\n", newfd);
                    add_event_to_epoll(newfd);
                }
                else {  // Processing non server socket s
                    if (revents[i].events & EPOLLIN) {
                        do_http_request((connect_stat_t*)revents[i].data.ptr);
                    }
                    else if(revents[i].events & EPOLLOUT) {
                        do_http_respone((connect_stat_t*)revents[i].data.ptr);
                    }
                    else {

                    }
                }
            }
            break;
        }   // end switch
    }   // end while

    return 0;
}

connect_stat_t* get_stat(int fd) {
    connect_stat_t* p = (connect_stat_t*)malloc(sizeof(connect_stat_t));
    if (!p) { return NULL; }

    bzero(p, sizeof(connect_stat_t));
    p->fd = fd;

    return p;
}

void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int init_server(const char* ip, unsigned short port) {
    if (!ip) {
        fprintf(stderr, "func:%s(): Parameter[ip] is empty!\n", __FUNCTION__);
        return -1;
    }

    int ret = 0;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        fprintf(stderr, "func:%s() - socket(): failed! reason: %s!\n", __FUNCTION__, strerror(errno));
        return -1;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind IP address port number
    struct sockaddr_in local;
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = inet_addr(ip);
    ret = bind(sock, (struct sockaddr*)(&local), sizeof(local));
    if (ret < 0) {
        fprintf(stderr, "func:%s() - bind(): failed! reason: %s!\n", __FUNCTION__, strerror(errno));
        return -1;
    }

    // monitor
    ret = listen(sock, 5);
    if (ret < 0) {
        fprintf(stderr, "func:%s() - listen(): failed! reason: %s!\n", __FUNCTION__, strerror(errno));
        return -1;
    }

    return sock;
}

void add_event_to_epoll(int newfd) {
    if (newfd < 0) { return; }

    connect_stat_t* p = get_stat(newfd);
    set_nonblock(newfd);

    p->_ev.events = EPOLLIN;
    p->_ev.data.ptr = p;

    epoll_ctl(epfd, EPOLL_CTL_ADD, newfd, &p->_ev);
}

void do_http_request(connect_stat_t* p) {
    if (!p) { return; }

    char buffer[4096] = { 0 };

    ssize_t len = read(p->fd, buffer, sizeof(buffer) - 1);
    if (len == 0) {
        printf("close client[%d] socket!\n", p->fd);
        epoll_ctl(epfd, EPOLL_CTL_DEL, p->fd, NULL);
        close(p->fd);
        free(p);
        return;
    }
    else if (len < 0) {
        fprintf(stderr, "%s() - read(): failed! reason: %s\n", __FUNCTION__, strerror(errno));
        return;
    }

    buffer[len] = '\0';
    printf("recv request data: %s\n", buffer);

    char* pos = buffer;

    if (!strncasecmp(pos, "GET", 3)) {
        p->handler = welcome_response_handler;
    }
    else if (!strncasecmp(pos, "POST", 4)) {
        printf("---data: %s\n", buffer);
        // Get URL
        printf("---post---\n");
        pos += strlen("POST");
        while (*pos == ' ' || *pos == '/') ++pos;

        if (!strncasecmp(pos, "commit", 6)) {
            int len = 0;

            printf("post commit --------\n");
            pos = strstr(buffer, "\r\n\r\n");
            char* end = NULL;
            if (end = strstr(pos, "name=")) {
                pos = end + strlen("name=");
                end = pos;
                while (('a' <= *end && *end <= 'z') || ('A' <= *end && *end <= 'Z') || ('0' <= *end && *end <= '9'))	end++;
                len = end - pos;
                if (len > 0) {
                    memcpy(p->name, pos, end - pos);
                    p->name[len] = '\0';
                }
            }

            if (end = strstr(pos, "age=")) {
                pos = end + strlen("age=");
                end = pos;
                while ('0' <= *end && *end <= '9')	end++;
                len = end - pos;
                if (len > 0) {
                    memcpy(p->age, pos, end - pos);
                    p->age[len] = '\0';
                }
            }
            p->handler = commit_respone_handler;

        }
        else {
            p->handler = welcome_response_handler;
        }
    }
    else {
        p->handler = welcome_response_handler;
    }

    // Generate processing results
    p->_ev.events = EPOLLOUT;
    epoll_ctl(epfd, EPOLL_CTL_MOD, p->fd, &p->_ev);

}

void do_http_respone(connect_stat_t* p) {
    if (!p) { return; }
    p->handler(p);
}

void welcome_response_handler(connect_stat_t* p) {
    int fd = open("./index.html", O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "commit_respone_handler() - open: failed! reason: %s\n", strerror(errno));
        return;
    }

    char buffer[4096] = { 0 };
    int rlen = read(fd, buffer, sizeof(buffer));
    close(fd);
    if (rlen < 1) {
        fprintf(stderr, "commit_respone_handler() - read: failed! reason: %s\n", strerror(errno));
        return;
    }

    char* content = (char*)malloc(strlen(main_header) + 128 + rlen);
    char len_buf[64] = { 0 };
    strcpy(content, main_header);
    snprintf(len_buf, sizeof(len_buf), "Content-Length: %d\r\n\r\n", (int)rlen);
    strcat(content, len_buf);
    strcat(content, buffer);
    printf("send reply to client!\n");

    int wlen = write(p->fd, content, strlen(content));
    if (wlen < 1) {
        fprintf(stderr, "commit_respone_handler() - write: failed! reason: %s\n", strerror(errno));
        return;
    }
    
    p->_ev.events = EPOLLIN;
    epoll_ctl(epfd, EPOLL_CTL_MOD, p->fd, &p->_ev);
    free(content);
}

void commit_respone_handler(connect_stat_t* p) {
    int fd = open("./reply.html", O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "commit_respone_handler() - open: failed! reason: %s\n", strerror(errno));
        return;
    }

    char buffer[1024] = { 0 };
    int rlen = read(fd, buffer, sizeof(buffer));
    close(fd);
    if (rlen < 1) {
        fprintf(stderr, "commit_respone_handler() - read: failed! reason: %s\n", strerror(errno));
        return;
    }

    char* content = (char*)malloc(strlen(main_header) + 128 + rlen);
    char* tmp = (char*)malloc(rlen + 128);
    char len_buf[64] = { 0 };
    strcpy(content, main_header);
    snprintf(len_buf, sizeof(len_buf), "Content-Length: %d\r\n\r\n", (int)rlen);
    strcat(content, len_buf);
    snprintf(tmp, rlen + 128, buffer, p->name, p->age);
    strcat(content, tmp);
    printf("send reply to client!\n");

    //printf("write data: %s\n", content);
    int wlen = write(p->fd, content, strlen(content));
    if (wlen < 1) {
        fprintf(stderr, "commit_respone_handler() - write: failed! reason: %s\n", strerror(errno));
        return;
    }

    p->_ev.events = EPOLLIN;
    epoll_ctl(epfd, EPOLL_CTL_MOD, p->fd, &p->_ev);
    free(tmp);
    free(content);

}
