// Host side of the single-image builds (32-bit Android, Linux test build): the guest is the same program, so its
// threads are ordinary threads and any memory is addressable by it.
#include "port.h"

#include <pthread.h>
#include <stdlib.h>

void *host_low_alloc(size_t size) { return calloc(1, size); }

typedef struct Start {
    void (*fn)(void *);
    void *arg;
} Start;

static void *trampoline(void *p) {
    Start s = *(Start *) p;
    free(p);
    s.fn(s.arg);
    return NULL;
}

void host_start_guest_thread(void (*fn)(void *), void *arg) {
    Start *s = malloc(sizeof(*s));
    s->fn = fn;
    s->arg = arg;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    if (pthread_create(&t, &attr, trampoline, s) != 0) {
        port_fatal("host: pthread_create failed");
    }
    pthread_attr_destroy(&attr);
}
