#define _GNU_SOURCE
#include <stdatomic.h>
#include "lib.h"

#define NUM_VER 5
uint64_t *data[NUM_VER+1];
atomic_uint_fast64_t version = 0;

void *writer(void *arg)
{
    struct qsbr *q = (struct qsbr *) arg;
    for (int i = 0; i < NUM_VER; ++i) {
        sleep(2);
        atomic_fetch_add_explicit(&version, 1, memory_order_relaxed);
        
        printf("\n---- begin of grace period: %lu -----\n", atomic_load_explicit(&version, memory_order_relaxed));
        fflush(stdout);

        qsbr_wait(q, atomic_load_explicit(&version, memory_order_relaxed));

        printf("---- end of grace period: %lu ------\n\n", atomic_load_explicit(&version, memory_order_relaxed));
        fflush(stdout);

        // reclamation memory
        free(data[atomic_load_explicit(&version, memory_order_acquire)-1]);
        fflush(stdout);
    }
    pthread_exit(NULL);
}

void *reader(void *arg)
{
    struct qsbr_ref *ref = (struct qsbr_ref *) arg;

    while (1) {
        uint64_t v = atomic_load_explicit(&version, memory_order_relaxed);
        // uint64_t v = version;
        uint64_t tmp = *data[v]; // access data
        // printf("tid: %d access data, %lu\n", gettid(), tmp);
        // fflush(stdout);
        sleep(rand() % 3 + 2); // hold data and do nothing
        
        printf("tid: %d release data, %lu\n", gettid(), tmp);
        fflush(stdout);

        qsbr_update(ref, atomic_load_explicit(&version, memory_order_relaxed)); // on quiescent

        if(v == NUM_VER) {
            break;
        }

        sleep(rand() % 1 + 1);
    }
    
    pthread_exit(NULL);
}
void *reader2(void *arg)
{
    struct qsbr_ref *ref = (struct qsbr_ref *) arg;

    while (1) {
        uint64_t v = atomic_load_explicit(&version, memory_order_relaxed);
        uint64_t tmp = *data[v]; // access data
        // printf("tid: %d access data, %lu\n", gettid(), tmp);
        // fflush(stdout);
        sleep(rand() % 1 + 1); // hold data and do nothing
        
        printf("tid: %d release data, %lu\n", gettid(), tmp);
        fflush(stdout);
        qsbr_update(ref, atomic_load_explicit(&version, memory_order_relaxed)); // on quiescent
        

        if(v == NUM_VER) {
            break;
        }

        sleep(rand() % 1 + 1);
    }
    
    pthread_exit(NULL);
}

#define NUM_READER 5
#define NUM_TID 1 + NUM_READER

int main()
{
    for(int i=0; i<=NUM_VER; ++i) {
        data[i] = malloc(sizeof(uint64_t));
        *data[i] = 20 + i;
    }

    srand(time(NULL));
    struct qsbr *q = qsbr_create();


    struct qsbr_ref qrefs[NUM_READER];
    for (int i=0; i<NUM_READER; ++i) {
        qsbr_register(q, &qrefs[i]);
    }

    pthread_t tid[NUM_TID];
    pthread_create(tid, NULL, writer, q);
    for (int i=0; i<NUM_READER; ++i) {
        if(i%2 == 0) {
            pthread_create(tid + 1 + i, NULL, reader, &qrefs[i]);
        } else {
            pthread_create(tid + 1 + i, NULL, reader2, &qrefs[i]);
        }
    }

    for (int i = 0; i < NUM_TID; ++i) {
        pthread_join(tid[i], NULL);
    }

    printf("After join\n");
    for (int i=0; i<NUM_READER; ++i) {
        qsbr_unregister(q, &qrefs[i]);
    }
    qsbr_destroy(q);
    return 0;
}
