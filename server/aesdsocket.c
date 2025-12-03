#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>

#define PORT 9000
#define DATA_FILE "/var/tmp/aesdsocketdata"

int exit_requested = 0;
int server_fd = -1;
int client_fd = -1;

void signal_handler()
{
    syslog(LOG_INFO, "Caught signal, exiting");
    exit_requested = 1;

    if (client_fd != -1) close(client_fd);
    if (server_fd != -1) close(server_fd);

    unlink(DATA_FILE);
}

void setup_signals()
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

void daemon_mode()
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(EXIT_FAILURE);
    }

    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    if (setsid() < 0) {
        perror("setsid");
        exit(EXIT_FAILURE);
    }

    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    freopen("/dev/null", "r", stdin);
    freopen("/dev/null", "w", stdout);
    freopen("/dev/null", "w", stderr);
}

int main(int argc, char *argv[])
{
    
    if (argc == 2 && strcmp(argv[1], "-d") == 0) {
        daemon_mode();
    }

    setup_signals();
    openlog("aesdsocket", LOG_PID, LOG_USER);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    int optval = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, SOMAXCONN) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    while (!exit_requested) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue; 
            perror("accept");
            break;
        }

        syslog(LOG_INFO, "Accepted connection from %s", inet_ntoa(client_addr.sin_addr));

        FILE *fp = fopen(DATA_FILE, "a+");
        if (!fp) {
            perror("fopen");
            close(client_fd);
            client_fd = -1;
            continue;
        }

        char *recv_buffer = NULL;
        size_t recv_buffer_size = 0;
        char buf[1024];
        ssize_t bytes;

        int done = 0;
        while ((bytes = read(client_fd, buf, sizeof(buf))) > 0) {
            for (ssize_t i = 0; i < bytes; i++) {
                char c = buf[i];
                char *tmp = realloc(recv_buffer, recv_buffer_size + 1);
                if (!tmp) {
                    perror("realloc");
                    free(recv_buffer);
                    recv_buffer = NULL;
                    recv_buffer_size = 0;
                    break;
                }
                recv_buffer = tmp;
                recv_buffer[recv_buffer_size++] = c;

                if (c == '\n') {
                    // Packet complete
                    fwrite(recv_buffer, 1, recv_buffer_size, fp);
                    fflush(fp);

                    // Send full file back
                    fseek(fp, 0, SEEK_SET);
                    ssize_t n;
                    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
                        write(client_fd, buf, n);
                    }
                    fseek(fp, 0, SEEK_END);

                    free(recv_buffer);
                    recv_buffer = NULL;
                    recv_buffer_size = 0;
                    done = 1;
                    break;
                }
            }
            if (done) {
                break;
            }
        }

        fclose(fp);

        syslog(LOG_INFO, "Closed connection from %s", inet_ntoa(client_addr.sin_addr));

        close(client_fd);
        client_fd = -1;
    }

    if (server_fd != -1) {
        close(server_fd);
    }
    unlink(DATA_FILE);
    closelog();

    return 0;
}
