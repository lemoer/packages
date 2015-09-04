#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <linux/un.h>

#define BUFSIZE 1024
#define MAX_CLIENTS 16

#define MAX(x,y) ((x < y)? y : x)

struct mplex_globals {
    int nfds;
    int clientsock[MAX_CLIENTS];
    int clientcount;
    int serversock;
    char *path;
    struct sockaddr_un sockaddr;
} G = {
    .clientcount = -1,
    .sockaddr = {},
};

void usage() {
    puts("Usage: multiplexer-server [-s] -p <path>");
}

void close_clientsock(int i) {
    close(G.clientsock[i]);
    if (G.nfds == G.clientsock[i] + 1) {
        G.nfds = G.serversock + 1;
        for (size_t j = 0; j < MAX_CLIENTS; j++) {
            if (G.clientsock[j] > 0 && j != i && G.clientsock[j] >= G.nfds)
                G.nfds = G.clientsock[j] + 1;
        }
    }
    G.clientsock[i] = 0;
    G.clientcount--;
}

void teardown_clients() {
    for (size_t i = 0; i < MAX_CLIENTS; i++)
        if (G.clientsock[i] > 0)
            close(G.clientsock[i]);
}

void teardown() {
    teardown_clients();
    close(G.serversock);
    unlink(G.path);
}

int server() {
    char buf[BUFSIZE];
    int retval = 0;
    size_t size;
    size_t i;
    fd_set rfds, xfds;

    if (bind(G.serversock, (struct sockaddr*)&G.sockaddr, sizeof(G.sockaddr)) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }
    listen(G.serversock, MAX_CLIENTS);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, teardown);
    signal(SIGALRM, teardown);
    signal(SIGUSR1, teardown_clients);
    signal(SIGUSR2, teardown_clients);
    signal(SIGHUP, teardown);
    signal(SIGINT, teardown);

    FD_ZERO(&rfds);
    FD_SET(0, &rfds);
    FD_SET(G.serversock, &rfds);
    G.nfds = G.serversock + 1;

    FD_ZERO(&xfds);
    while (select(G.nfds, &rfds, NULL, &xfds, NULL) >= 0) {
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (G.clientsock[i] > 0 && FD_ISSET(G.clientsock[i], &xfds)) {
                close_clientsock(i);
            }
        }

        if (FD_ISSET(G.serversock, &rfds)) {
            for (i = 0; i < MAX_CLIENTS; i++) {
                if (G.clientsock[i] == 0) {
                    G.clientsock[i] = accept(G.serversock, NULL, NULL);
                    G.clientcount = MAX(G.clientcount + 1, 1);
                    if (G.clientsock[i] >= G.nfds)
                        G.nfds = G.clientsock[i] + 1;
                    break;
                }
            }
        }

        if (FD_ISSET(0, &rfds)) {
            size = read(0, &buf, BUFSIZE);
            if (size < 0) {
                perror("read");
                retval = EXIT_FAILURE;
                break;
            }
            if (size == 0)
                break;
            if (size > 0) {
                for (i = 0; i < MAX_CLIENTS; i++)
                    if (G.clientsock[i] > 0)
                        if (write(G.clientsock[i], &buf, size) <= 0)
                            close_clientsock(i);
            }
        }

        FD_ZERO(&rfds);
        FD_SET(0, &rfds);
        FD_SET(G.serversock, &rfds);

        FD_ZERO(&xfds);
        size = 0;
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (G.clientsock[i] > 0) {
                size++;
                FD_SET(G.clientsock[i], &xfds);
            }
        }
        // exit if all clients are disconnected
        if (size == 0)
            break;
    }

    teardown();
    return retval;
}

int client() {
    char buf[BUFSIZE];
    if (connect(G.serversock, (struct sockaddr*)&G.sockaddr, sizeof(G.sockaddr)) < 0) {
        perror("connect");
        exit(EXIT_FAILURE);
    }

    size_t size;
    while ((size = read(G.serversock, &buf, BUFSIZE)) > 0) {
        if (fwrite(&buf, 1, size, stdout) < 0) {
            perror("write");
        }
        fflush(stdout);
    }
    close(G.serversock);
    return EXIT_SUCCESS;
}

int main(int argc, char* argv[]) {
    int servemode = 0;
    int c;
    while ((c = getopt(argc, argv, "sp:h")) != -1) {
        switch (c) {
            case 'p':
                if (G.path) {
                    fprintf(stderr, "Only one path (-p) is supported.\n");
                    exit(EXIT_FAILURE);
                }
                G.path = optarg;
                break;
            case 's':
                servemode = 1;
                break;
            case 'h':
                usage();
                exit(EXIT_SUCCESS);
                break;
        }
    }

    if (!G.path) {
        fprintf(stderr, "No path option (-p) given.\n");
        usage();
        exit(EXIT_FAILURE);
    }

    G.sockaddr.sun_family = AF_UNIX;
    strncpy(G.sockaddr.sun_path, G.path, UNIX_PATH_MAX);

    G.serversock = socket(AF_UNIX, SOCK_STREAM, 0);

    if (servemode)
        return server();
    else
        return client();
}
