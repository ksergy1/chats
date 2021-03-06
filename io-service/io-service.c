#include "io-service.h"
#include "common.h"

#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <assert.h>

#include <sys/eventfd.h>
#include <sys/epoll.h>

typedef struct job {
    iosvc_job_function_t job;
    void *ctx;
    bool oneshot;
} job_t;

typedef struct lookup_table_element {
    int fd;
    struct epoll_event event;
    job_t job[IO_SVC_OP_COUNT];
} lookup_table_element_t;

struct io_service {
    /* are we still running flag */
    bool allow_new;
    bool running;
    /* used for notification purposes */
    int event_fd;
    /* job list by fd lookup table  */
    list_t *lookup_table;

    int epoll_fd;
    struct epoll_event event_fd_event;

    pthread_mutex_t object_mutex;
};

static const int OP_FLAGS[IO_SVC_OP_COUNT] = {
    [IO_SVC_OP_READ] = EPOLLIN,
    [IO_SVC_OP_WRITE] = EPOLLOUT
};

static
void notify_svc(int fd) {
    eventfd_write(fd, 1);
}

static
eventfd_t svc_notified(int fd) {
    eventfd_t v;
    eventfd_read(fd, &v);
    return v;
}

io_service_t *io_service_init() {
    io_service_t *iosvc = allocate(sizeof(io_service_t));
    int r;

    memset(iosvc, 0, sizeof(io_service_t));

    r = pthread_mutex_init(&iosvc->object_mutex, NULL);

    if (r) {
        errno = r;
        return NULL;
    }

    iosvc->event_fd = eventfd(0, EFD_CLOEXEC | EFD_SEMAPHORE);

    if (iosvc->event_fd < 0) {
        pthread_mutex_destroy(&iosvc->object_mutex);
        deallocate(iosvc);
        return NULL;
    }

    iosvc->lookup_table = list_init(sizeof(struct lookup_table_element));
    iosvc->allow_new = true;
    iosvc->running = false;

    iosvc->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (iosvc->epoll_fd < 0) {
        close(iosvc->event_fd);
        pthread_mutex_destroy(&iosvc->object_mutex);
        deallocate(iosvc);
        return NULL;
    }

    memset(&iosvc->event_fd_event, 0, sizeof(iosvc->event_fd_event));

    iosvc->event_fd_event.events = EPOLLIN;
    iosvc->event_fd_event.data.fd = iosvc->event_fd;

    if (epoll_ctl(iosvc->epoll_fd, EPOLL_CTL_ADD, iosvc->event_fd, &iosvc->event_fd_event)) {
        io_service_deinit(iosvc);
        return NULL;
    }

    return iosvc;
}

void io_service_stop(io_service_t *iosvc, bool wait_pending) {
    pthread_mutex_lock(&iosvc->object_mutex);
    iosvc->allow_new = false;
    iosvc->running = wait_pending;
    notify_svc(iosvc->event_fd);
    pthread_mutex_unlock(&iosvc->object_mutex);
}

void io_service_deinit(io_service_t *iosvc) {
    pthread_mutex_destroy(&iosvc->object_mutex);
    close(iosvc->event_fd);
    close(iosvc->epoll_fd);

    list_deinit(iosvc->lookup_table);

    deallocate(iosvc);
}

void io_service_post_job(io_service_t *iosvc,
                         int fd, io_svc_op_t op, bool oneshot,
                         iosvc_job_function_t job,
                         void *ctx) {
    pthread_mutex_lock(&iosvc->object_mutex);

    if (iosvc->allow_new && job) {
        job_t *new_job;

        lookup_table_element_t *lte;
        for (lte = list_first_element(iosvc->lookup_table);
             lte;
             lte = list_next_element(iosvc->lookup_table, lte))
            if (lte->fd == fd) break;

        if (!lte) {
            lte = list_append(iosvc->lookup_table);

            assert(lte);

            lte->event.data.fd = lte->fd = fd;
            lte->event.events = 0;
            memset(lte->job, 0, sizeof(lte->job));
        }

        if (lte->job[op].job == NULL) {
            lte->event.events |= OP_FLAGS[op];
            lte->job[op].job = job;
            lte->job[op].ctx = ctx;
            lte->job[op].oneshot = oneshot;

            if (iosvc->running) notify_svc(iosvc->event_fd);
        }
    }

    pthread_mutex_unlock(&iosvc->object_mutex);
}

void io_service_run(io_service_t *iosvc) {
    pthread_mutex_t *mutex = &iosvc->object_mutex;
    bool *running = &iosvc->running;
    struct epoll_event event;
    int epoll_fd = iosvc->epoll_fd;
    int event_fd = iosvc->event_fd;
    int r, fd;
    ssize_t idx;
    io_svc_op_t op;
    lookup_table_element_t *lte, *prev_lte;
    iosvc_job_function_t job;
    void *ctx;

    pthread_mutex_lock(mutex);

    for (lte = list_first_element(iosvc->lookup_table);
         lte;
         lte = list_next_element(iosvc->lookup_table, lte))
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, lte->fd, &lte->event);

    *running = true;

    while (*running) {
        pthread_mutex_unlock(mutex);
        r = epoll_wait(epoll_fd, &event, 1, -1);
        pthread_mutex_lock(mutex);

        if (r < 0) continue;

        fd = event.data.fd;

        if (fd == event_fd) {
            svc_notified(fd);

            if ((list_size(iosvc->lookup_table) == 0) && (iosvc->allow_new == false))
                *running = false;

            for (lte = list_first_element(iosvc->lookup_table); lte;) {
                if (lte->event.events == 0) {
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, lte->fd, NULL);
                    lte = list_remove_next(iosvc->lookup_table, lte);
                    continue;
                }

                if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, lte->fd, &lte->event))
                    if (errno == ENOENT)
                        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, lte->fd, &lte->event);

                lte = list_next_element(iosvc->lookup_table, lte);
            }

            continue;
        } /* if (fd == event_fd) */

        for (op = 0; op < IO_SVC_OP_COUNT; ++op) {
            if (!(event.events & OP_FLAGS[op])) continue;

            for (lte = list_first_element(iosvc->lookup_table);
                 lte;
                 lte = list_next_element(iosvc->lookup_table, lte))
                if (lte->fd == fd) {
                    if (lte->job[op].job != NULL) break;
                    else {
                        lte = NULL;
                        break;
                    }
                }

            if (lte) {
                job = lte->job[op].job;
                ctx = lte->job[op].ctx;

                if (lte->job[op].oneshot) {
                    lte->job[op].ctx = lte->job[op].job = NULL;
                    lte->event.events &= ~OP_FLAGS[op];

                    if (lte->event.events == 0) {
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, lte->fd, NULL);
                        list_remove_element(iosvc->lookup_table, lte);
                    }
                }

                pthread_mutex_unlock(mutex);
                (*job)(fd, op, ctx);
                pthread_mutex_lock(mutex);
            } /* if (lte) */
        }   /* for (op = 0; op < IO_SVC_OP_COUNT; ++op) */
    }   /* while (*running) */

    pthread_mutex_unlock(mutex);
}

void io_service_remove_job(io_service_t *iosvc,
                           int fd, io_svc_op_t op,
                           iosvc_job_function_t job, void *ctx) {
    lookup_table_element_t *lte;
    bool done = false;

    pthread_mutex_lock(&iosvc->object_mutex);

    for (lte = list_first_element(iosvc->lookup_table);
         lte;
         lte = list_next_element(iosvc->lookup_table, lte))
        if (lte->fd == fd)
            if (lte->job[op].job == job && lte->job[op].ctx == ctx) {
                lte->job[op].job = NULL;
                lte->job[op].ctx = NULL;
                lte->event.events &= ~OP_FLAGS[op];
                done = true;
                break;
            }

    if (done && iosvc->running) notify_svc(iosvc->event_fd);
    pthread_mutex_unlock(&iosvc->object_mutex);
}