#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/queue.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <pthread.h>
#include <sys/ioctl.h>

#ifndef USE_AESD_CHAR_DEVICE
#define USE_AESD_CHAR_DEVICE 1
#endif

#if USE_AESD_CHAR_DEVICE
#include "../aesd-char-driver/aesd_ioctl.h"
#endif

#define PORT 9000
#if USE_AESD_CHAR_DEVICE
#define DATA_FILE "/dev/aesdchar"
#else
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define TIMESTAMP_INTERVAL_SECONDS 10
#endif

volatile sig_atomic_t exit_requested = 0;
int server_fd = -1;

/* Protects all access (reads and writes) to DATA_FILE so that a write from
 * one connection/timestamp thread is never interleaved with another. */
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

struct thread_node {
    pthread_t thread_id;
    int client_fd;
    struct sockaddr_in client_addr;
    volatile int thread_complete;
    SLIST_ENTRY(thread_node) entries;
};

SLIST_HEAD(thread_list, thread_node);
static struct thread_list head = SLIST_HEAD_INITIALIZER(head);

void signal_handler(int signo)
{
    (void)signo;
    exit_requested = 1;

    /* Only async-signal-safe calls here: unblock the accept() loop by
     * shutting down the listening socket. Per-connection cleanup and
     * unlinking the data file happens in main() after threads are joined. */
    if (server_fd != -1) {
        shutdown(server_fd, SHUT_RDWR);
    }
}

void setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

void daemon_mode(void)
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

static void write_all(int fd, const char *buf, size_t len)
{
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, buf + written, len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        written += (size_t)n;
    }
}

/* Writes packet to DATA_FILE and sends the file's full contents back to the
 * client, holding file_mutex for the whole operation so no other thread's
 * write or the timestamp thread can be interleaved with it. */
static void append_packet_and_reply(const char *packet, size_t packet_len, int client_fd)
{
    char send_buf[1024];

    pthread_mutex_lock(&file_mutex);

    FILE *fp = fopen(DATA_FILE, "a+");
    if (!fp) {
        syslog(LOG_ERR, "fopen failed: %s", strerror(errno));
        pthread_mutex_unlock(&file_mutex);
        return;
    }

    fwrite(packet, 1, packet_len, fp);
    fflush(fp);

    fseek(fp, 0, SEEK_SET);
    size_t n;
    while ((n = fread(send_buf, 1, sizeof(send_buf), fp)) > 0) {
        write_all(client_fd, send_buf, n);
    }

    fclose(fp);
    pthread_mutex_unlock(&file_mutex);
}

#if USE_AESD_CHAR_DEVICE
#define IOCTL_SEEK_PREFIX "AESDCHAR_IOCSEEKTO:"

/* Parses a packet as "AESDCHAR_IOCSEEKTO:X,Y\n" with X/Y unsigned decimal
 * integers. Returns 1 and fills *seekto on a full match, 0 otherwise. */
static int try_parse_seekto(const char *packet, size_t packet_len, struct aesd_seekto *seekto)
{
    char tmp[64];
    unsigned int write_cmd, write_cmd_offset;
    int consumed = 0;

    if (packet_len < 1 || packet[packet_len - 1] != '\n')
        return 0;
    if (packet_len - 1 >= sizeof(tmp))
        return 0;

    memcpy(tmp, packet, packet_len - 1);
    tmp[packet_len - 1] = '\0';

    if (sscanf(tmp, IOCTL_SEEK_PREFIX "%u,%u%n", &write_cmd, &write_cmd_offset, &consumed) != 2)
        return 0;
    if ((size_t)consumed != packet_len - 1)
        return 0;

    seekto->write_cmd = write_cmd;
    seekto->write_cmd_offset = write_cmd_offset;
    return 1;
}

/* Issues the AESDCHAR_IOCSEEKTO ioctl and reads the reply from the same fd,
 * so the seeked file position is honored by the read, per assignment 9. */
static void handle_seekto_and_reply(const struct aesd_seekto *seekto, int client_fd)
{
    char send_buf[1024];
    ssize_t n;

    pthread_mutex_lock(&file_mutex);

    int fd = open(DATA_FILE, O_RDWR);
    if (fd < 0) {
        syslog(LOG_ERR, "open failed for ioctl: %s", strerror(errno));
        pthread_mutex_unlock(&file_mutex);
        return;
    }

    if (ioctl(fd, AESDCHAR_IOCSEEKTO, seekto) != 0) {
        syslog(LOG_ERR, "AESDCHAR_IOCSEEKTO failed: %s", strerror(errno));
        close(fd);
        pthread_mutex_unlock(&file_mutex);
        return;
    }

    while ((n = read(fd, send_buf, sizeof(send_buf))) > 0) {
        write_all(client_fd, send_buf, (size_t)n);
    }

    close(fd);
    pthread_mutex_unlock(&file_mutex);
}
#endif

static void *handle_connection(void *arg)
{
    struct thread_node *node = (struct thread_node *)arg;
    int client_fd = node->client_fd;
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &node->client_addr.sin_addr, client_ip, sizeof(client_ip));

    syslog(LOG_INFO, "Accepted connection from %s", client_ip);

    char *recv_buffer = NULL;
    size_t recv_buffer_size = 0;
    char buf[1024];
    ssize_t bytes;

    while ((bytes = recv(client_fd, buf, sizeof(buf), 0)) > 0) {
        for (ssize_t i = 0; i < bytes; i++) {
            char c = buf[i];
            char *tmp = realloc(recv_buffer, recv_buffer_size + 1);
            if (!tmp) {
                syslog(LOG_ERR, "realloc failed");
                free(recv_buffer);
                recv_buffer = NULL;
                recv_buffer_size = 0;
                continue;
            }
            recv_buffer = tmp;
            recv_buffer[recv_buffer_size++] = c;

            if (c == '\n') {
#if USE_AESD_CHAR_DEVICE
                struct aesd_seekto seekto;
                if (try_parse_seekto(recv_buffer, recv_buffer_size, &seekto)) {
                    handle_seekto_and_reply(&seekto, client_fd);
                } else {
                    append_packet_and_reply(recv_buffer, recv_buffer_size, client_fd);
                }
#else
                append_packet_and_reply(recv_buffer, recv_buffer_size, client_fd);
#endif
                free(recv_buffer);
                recv_buffer = NULL;
                recv_buffer_size = 0;
            }
        }
    }

    free(recv_buffer);

    syslog(LOG_INFO, "Closed connection from %s", client_ip);

    close(client_fd);
    node->thread_complete = 1;
    return NULL;
}

#if !USE_AESD_CHAR_DEVICE
static void *timestamp_thread_func(void *arg)
{
    (void)arg;

    while (!exit_requested) {
        for (int i = 0; i < TIMESTAMP_INTERVAL_SECONDS && !exit_requested; i++) {
            sleep(1);
        }
        if (exit_requested) break;

        time_t now = time(NULL);
        struct tm tm_info;
        localtime_r(&now, &tm_info);

        char timestamp[64];
        strftime(timestamp, sizeof(timestamp), "%a, %d %b %Y %H:%M:%S %z", &tm_info);

        char line[128];
        int len = snprintf(line, sizeof(line), "timestamp:%s\n", timestamp);

        pthread_mutex_lock(&file_mutex);
        FILE *fp = fopen(DATA_FILE, "a");
        if (fp) {
            fwrite(line, 1, (size_t)len, fp);
            fclose(fp);
        } else {
            syslog(LOG_ERR, "fopen failed for timestamp: %s", strerror(errno));
        }
        pthread_mutex_unlock(&file_mutex);
    }

    return NULL;
}
#endif

/* Joins and frees any connection threads that have finished, without
 * blocking on ones still in progress. */
static void reap_completed_threads(void)
{
    struct thread_node *node = SLIST_FIRST(&head);

    while (node != NULL) {
        struct thread_node *next = SLIST_NEXT(node, entries);

        if (node->thread_complete) {
            pthread_join(node->thread_id, NULL);
            SLIST_REMOVE(&head, node, thread_node, entries);
            free(node);
        }

        node = next;
    }
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

#if !USE_AESD_CHAR_DEVICE
    pthread_t timestamp_tid;
    pthread_create(&timestamp_tid, NULL, timestamp_thread_func, NULL);
#endif

    while (!exit_requested) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (exit_requested) break;
            perror("accept");
            break;
        }

        struct thread_node *node = malloc(sizeof(struct thread_node));
        if (!node) {
            syslog(LOG_ERR, "malloc failed for connection node");
            close(client_fd);
            continue;
        }
        node->client_fd = client_fd;
        node->client_addr = client_addr;
        node->thread_complete = 0;

        if (pthread_create(&node->thread_id, NULL, handle_connection, node) != 0) {
            syslog(LOG_ERR, "pthread_create failed: %s", strerror(errno));
            close(client_fd);
            free(node);
            continue;
        }

        SLIST_INSERT_HEAD(&head, node, entries);

        reap_completed_threads();
    }

    /* Request exit from each still-running connection thread by shutting
     * down its socket, which unblocks any thread waiting in recv(). */
    struct thread_node *node;
    SLIST_FOREACH(node, &head, entries) {
        shutdown(node->client_fd, SHUT_RDWR);
    }

    while (!SLIST_EMPTY(&head)) {
        node = SLIST_FIRST(&head);
        pthread_join(node->thread_id, NULL);
        SLIST_REMOVE_HEAD(&head, entries);
        free(node);
    }

#if !USE_AESD_CHAR_DEVICE
    pthread_join(timestamp_tid, NULL);
#endif

    if (server_fd != -1) {
        close(server_fd);
    }
#if !USE_AESD_CHAR_DEVICE
    /* /dev/aesdchar is a device node managed by the driver, not a regular
     * file we created - leave it in place on exit. */
    unlink(DATA_FILE);
#endif
    closelog();

    return 0;
}
