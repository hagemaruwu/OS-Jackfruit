/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

static volatile sig_atomic_t g_child_exited = 0;
static volatile sig_atomic_t g_shutdown_requested = 0;

static void sigchld_handler(int sig)
{
    (void)sig;
    g_child_exited = 1;
}

static void shutdown_handler(int sig)
{
    (void)sig;
    g_shutdown_requested = 1;
}

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

typedef struct {
    supervisor_ctx_t *ctx;
    int read_fd;
    char container_id[CONTAINER_ID_LEN];
} log_reader_ctx_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;
    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * TODO:
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 *   - wait correctly while the buffer is empty
 *   - return a useful status when shutdown is in progress
 *   - avoid races with producers and shutdown
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;
    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

static void *log_reader_thread(void *arg)
{
    log_reader_ctx_t *reader_ctx = (log_reader_ctx_t *)arg;
    char chunk[LOG_CHUNK_SIZE];

    while (1) {
        ssize_t n = read(reader_ctx->read_fd, chunk, sizeof(chunk));
        if (n > 0) {
            log_item_t item;
            memset(&item, 0, sizeof(item));
            strncpy(item.container_id, reader_ctx->container_id, sizeof(item.container_id) - 1);
            item.length = (size_t)n;
            memcpy(item.data, chunk, (size_t)n);
            if (bounded_buffer_push(&reader_ctx->ctx->log_buffer, &item) != 0)
                break;
            continue;
        }

        if (n == 0)
            break;
        if (errno == EINTR)
            continue;
        break;
    }

    close(reader_ctx->read_fd);
    free(reader_ctx);
    return NULL;
}

/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    mkdir(LOG_DIR, 0755);
    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        char path[PATH_MAX];
        int fd;
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            (void)write(fd, item.data, item.length);
            close(fd);
        }
    }
    return NULL;
}

/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    if (cfg->nice_value != 0)
        (void)setpriority(PRIO_PROCESS, 0, cfg->nice_value);

    (void)sethostname(cfg->id, strlen(cfg->id));

    if (chdir(cfg->rootfs) < 0)
        return 1;
    if (chroot(cfg->rootfs) < 0)
        return 1;
    if (chdir("/") < 0)
        return 1;

#ifdef __linux__
    (void)mount("proc", "/proc", "proc", 0, NULL);
#endif

    dup2(cfg->log_write_fd, STDOUT_FILENO);
    dup2(cfg->log_write_fd, STDERR_FILENO);
    close(cfg->log_write_fd);

    char *argv[] = {"/bin/sh", "-c", cfg->command, NULL};
    execvp(argv[0], argv);
    return 1;
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

static void add_container_record(supervisor_ctx_t *ctx_local, container_record_t *rec)
{
    pthread_mutex_lock(&ctx_local->metadata_lock);
    rec->next = ctx_local->containers;
    ctx_local->containers = rec;
    pthread_mutex_unlock(&ctx_local->metadata_lock);
}

static container_record_t *find_container_record(supervisor_ctx_t *ctx_local, const char *id)
{
    container_record_t *curr;
    pthread_mutex_lock(&ctx_local->metadata_lock);
    curr = ctx_local->containers;
    while (curr) {
        if (strcmp(curr->id, id) == 0)
            break;
        curr = curr->next;
    }
    pthread_mutex_unlock(&ctx_local->metadata_lock);
    return curr;
}

static void reap_children(supervisor_ctx_t *ctx_local)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&ctx_local->metadata_lock);
        for (container_record_t *curr = ctx_local->containers; curr; curr = curr->next) {
            if (curr->host_pid != pid)
                continue;

            if (WIFEXITED(status)) {
                curr->state = CONTAINER_EXITED;
                curr->exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                curr->exit_signal = WTERMSIG(status);
                if (curr->state != CONTAINER_STOPPED && curr->exit_signal == SIGKILL)
                    curr->state = CONTAINER_KILLED;
                else if (curr->state != CONTAINER_STOPPED)
                    curr->state = CONTAINER_EXITED;
            }
            break;
        }
        pthread_mutex_unlock(&ctx_local->metadata_lock);
    }
}

static int handle_client_request(supervisor_ctx_t *ctx_local, int client_fd)
{
    control_request_t req;
    control_response_t res;
    ssize_t n;

    n = read(client_fd, &req, sizeof(req));
    if (n < (ssize_t)sizeof(req))
        return -1;

    memset(&res, 0, sizeof(res));
    res.status = 0;

    switch (req.kind) {
    case CMD_START:
    case CMD_RUN: {
        child_config_t *cfg;
        char *stack;
        int pipe_fds[2];
        pid_t pid;
        pthread_t reader_tid;
        log_reader_ctx_t *reader_ctx;
        container_record_t *rec;

        cfg = calloc(1, sizeof(*cfg));
        if (!cfg) {
            res.status = 1;
            snprintf(res.message, sizeof(res.message), "allocation failed");
            break;
        }

        strncpy(cfg->id, req.container_id, sizeof(cfg->id) - 1);
        strncpy(cfg->rootfs, req.rootfs, sizeof(cfg->rootfs) - 1);
        strncpy(cfg->command, req.command, sizeof(cfg->command) - 1);
        cfg->nice_value = req.nice_value;

        stack = malloc(STACK_SIZE);
        if (!stack) {
            free(cfg);
            res.status = 1;
            snprintf(res.message, sizeof(res.message), "stack allocation failed");
            break;
        }

        if (pipe(pipe_fds) < 0) {
            free(stack);
            free(cfg);
            res.status = 1;
            snprintf(res.message, sizeof(res.message), "pipe failed");
            break;
        }
        cfg->log_write_fd = pipe_fds[1];

#ifdef __linux__
        pid = clone(child_fn, stack + STACK_SIZE,
                    CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD, cfg);
#else
        pid = -1;
#endif
        if (pid < 0) {
            close(pipe_fds[0]);
            close(pipe_fds[1]);
            free(stack);
            free(cfg);
            res.status = 1;
            snprintf(res.message, sizeof(res.message), "clone failed");
            break;
        }

        close(pipe_fds[1]);

        reader_ctx = calloc(1, sizeof(*reader_ctx));
        if (reader_ctx) {
            reader_ctx->ctx = ctx_local;
            reader_ctx->read_fd = pipe_fds[0];
            strncpy(reader_ctx->container_id, req.container_id, sizeof(reader_ctx->container_id) - 1);
            if (pthread_create(&reader_tid, NULL, log_reader_thread, reader_ctx) == 0)
                pthread_detach(reader_tid);
            else {
                close(pipe_fds[0]);
                free(reader_ctx);
            }
        } else {
            close(pipe_fds[0]);
        }

        rec = calloc(1, sizeof(*rec));
        if (rec) {
            strncpy(rec->id, req.container_id, sizeof(rec->id) - 1);
            rec->host_pid = pid;
            rec->started_at = time(NULL);
            rec->state = CONTAINER_RUNNING;
            rec->soft_limit_bytes = req.soft_limit_bytes;
            rec->hard_limit_bytes = req.hard_limit_bytes;
            snprintf(rec->log_path, sizeof(rec->log_path), "%s/%s.log", LOG_DIR, req.container_id);
            add_container_record(ctx_local, rec);
        }

        if (ctx_local->monitor_fd >= 0) {
            (void)register_with_monitor(ctx_local->monitor_fd, req.container_id, pid,
                                        req.soft_limit_bytes, req.hard_limit_bytes);
        }

        if (req.kind == CMD_RUN) {
            int status;
            (void)waitpid(pid, &status, 0);
            reap_children(ctx_local);
        }

        snprintf(res.message, sizeof(res.message), "Container %s started with PID %d", req.container_id, pid);
        free(stack);
        free(cfg);
        break;
    }
    case CMD_PS: {
        int used = 0;
        pthread_mutex_lock(&ctx_local->metadata_lock);
        used += snprintf(res.message + used, sizeof(res.message) - (size_t)used,
                         "ID PID STATE\n");
        for (container_record_t *curr = ctx_local->containers; curr && used < (int)sizeof(res.message) - 32;
             curr = curr->next) {
            used += snprintf(res.message + used, sizeof(res.message) - (size_t)used,
                             "%s %d %s\n", curr->id, curr->host_pid, state_to_string(curr->state));
        }
        pthread_mutex_unlock(&ctx_local->metadata_lock);
        if (used == 0)
            snprintf(res.message, sizeof(res.message), "No containers tracked");
        break;
    }
    case CMD_LOGS: {
        char path[PATH_MAX];
        int fd;
        ssize_t r;
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, req.container_id);
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            res.status = 1;
            snprintf(res.message, sizeof(res.message), "log not found for %s", req.container_id);
            break;
        }
        r = read(fd, res.message, sizeof(res.message) - 1);
        close(fd);
        if (r < 0) {
            res.status = 1;
            snprintf(res.message, sizeof(res.message), "failed to read log");
        } else if (r == 0) {
            snprintf(res.message, sizeof(res.message), "log is empty");
        } else {
            res.message[r] = '\0';
        }
        break;
    }
    case CMD_STOP: {
        container_record_t *rec = find_container_record(ctx_local, req.container_id);
        if (!rec) {
            res.status = 1;
            snprintf(res.message, sizeof(res.message), "container %s not found", req.container_id);
            break;
        }
        rec->state = CONTAINER_STOPPED;
        (void)kill(rec->host_pid, SIGTERM);
        if (ctx_local->monitor_fd >= 0)
            (void)unregister_from_monitor(ctx_local->monitor_fd, rec->id, rec->host_pid);
        snprintf(res.message, sizeof(res.message), "Sent SIGTERM to container %s", req.container_id);
        break;
    }
    default:
        res.status = 1;
        snprintf(res.message, sizeof(res.message), "unsupported command");
        break;
    }

    (void)write(client_fd, &res, sizeof(res));
    return 0;
}

/*
 * TODO:
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    struct sockaddr_un addr;
    struct sigaction sa;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    (void)rootfs;
    mkdir(LOG_DIR, 0755);

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "Warning: monitor device unavailable, continuing without kernel monitor\n");

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = shutdown_handler;
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx) != 0) {
        perror("pthread_create");
        if (ctx.monitor_fd >= 0)
            close(ctx.monitor_fd);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        bounded_buffer_begin_shutdown(&ctx.log_buffer);
        pthread_join(ctx.logger_thread, NULL);
        if (ctx.monitor_fd >= 0)
            close(ctx.monitor_fd);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    unlink(CONTROL_PATH);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(ctx.server_fd, 16) < 0) {
        perror("control socket");
        close(ctx.server_fd);
        unlink(CONTROL_PATH);
        bounded_buffer_begin_shutdown(&ctx.log_buffer);
        pthread_join(ctx.logger_thread, NULL);
        if (ctx.monitor_fd >= 0)
            close(ctx.monitor_fd);
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    while (!g_shutdown_requested) {
        int client_fd;

        if (g_child_exited) {
            g_child_exited = 0;
            reap_children(&ctx);
        }

        client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd >= 0) {
            (void)handle_client_request(&ctx, client_fd);
            close(client_fd);
            continue;
        }

        if (errno == EINTR)
            continue;
        perror("accept");
        break;
    }

    close(ctx.server_fd);
    unlink(CONTROL_PATH);
    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);

    while (ctx.containers) {
        container_record_t *tmp = ctx.containers;
        ctx.containers = ctx.containers->next;
        free(tmp);
    }
    return 0;
}

/*
 * TODO:
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
static int send_control_request(const control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    control_response_t res;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Error: could not connect to supervisor\n");
        close(fd);
        return 1;
    }

    if (write(fd, req, sizeof(*req)) != (ssize_t)sizeof(*req)) {
        perror("write");
        close(fd);
        return 1;
    }

    if (read(fd, &res, sizeof(res)) != (ssize_t)sizeof(res)) {
        perror("read");
        close(fd);
        return 1;
    }

    if (res.status == 0)
        printf("%s\n", res.message);
    else
        fprintf(stderr, "%s\n", res.message);

    close(fd);
    return res.status;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
