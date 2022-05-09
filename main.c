#define _GNU_SOURCE
#include <stdatomic.h>
#include "lib.h"

#define ver_num 5
uint64_t data[ver_num+1] = {10, 11, 12, 13, 14, 15}; // lock? atomic?
// uint64_t version = 0;
atomic_uint_fast64_t version = 0;

// #define OUTPUT_TO_BUF 1
#ifdef OUTPUT_TO_BUF
pthread_mutex_t mu;
char output_buf[50000];
char output_len = 0;
#endif

void *writer(void *arg)
{
    struct qsbr *q = (struct qsbr *) arg;
    for (int i = 0; i < ver_num; ++i) {
        sleep(4);
        atomic_fetch_add_explicit(&version, 1, memory_order_relaxed); 
        // version++;
        
    #ifdef OUTPUT_TO_BUF
        pthread_mutex_lock(&mu);
        sprintf(output_buf+output_len, "\n---- begin of grace period: %lu ------\n", version);
        output_len = strlen(output_buf);
        pthread_mutex_unlock(&mu);
    #else
        printf("\n---- begin of grace period: %lu -----\n", version);
        fflush(stdout);
    #endif

        qsbr_wait(q, version);
        
    #ifdef OUTPUT_TO_BUF
        pthread_mutex_lock(&mu);
        sprintf(output_buf+output_len, "---- end of grace period: %lu ------\n\n", version);
        output_len = strlen(output_buf);
        pthread_mutex_unlock(&mu);
    #else
        printf("---- end of grace period: %lu ------\n\n", version);
        fflush(stdout);
    #endif
        
        // reclamation memory, TODO
    }
    pthread_exit(NULL);
}

void *reader(void *arg)
{
    struct qsbr_ref *ref = (struct qsbr_ref *) arg;

    while (1) {
        uint64_t v = atomic_load_explicit(&version, memory_order_relaxed);
        // uint64_t v = version;
        uint64_t tmp = data[v]; // access data
        printf("tid: %d access  data, %lu\n", gettid(), tmp);
        fflush(stdout);
        sleep(rand() % 3 + 2); // hold data and do nothing
        
        // qsbr_update(ref, v); // on quiescent
        qsbr_update(ref, atomic_load_explicit(&version, memory_order_relaxed)); // on quiescent

    #ifdef OUTPUT_TO_BUF
        pthread_mutex_lock(&mu);
        sprintf(output_buf+output_len, "tid: %d release data, %lu\n", gettid(), tmp);
        output_len = strlen(output_buf);
        pthread_mutex_unlock(&mu);
    #else
        printf("tid: %d release data, %lu\n", gettid(), tmp);
        fflush(stdout);
    #endif
        
        if(v == ver_num) {
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
        uint64_t tmp = data[v]; // access data
        printf("tid: %d access  data, %lu\n", gettid(), tmp);
        fflush(stdout);
        sleep(rand() % 1 + 1); // hold data and do nothing
        
        // qsbr_update(ref, v); // on quiescent
        qsbr_update(ref, atomic_load_explicit(&version, memory_order_relaxed)); // on quiescent
        printf("tid: %d release data, %lu\n", gettid(), tmp);
        fflush(stdout);

        if(v == ver_num) {
            break;
        }

        sleep(rand() % 1 + 1);
    }
    
    pthread_exit(NULL);
}

int main()
{
    srand(time(NULL));
    struct qsbr *q = qsbr_create();
    struct qsbr_ref qref, qref2, qref3;
    qsbr_register(q, &qref);
    qsbr_register(q, &qref2);
    qsbr_register(q, &qref3);
#define tid_n 1 + 3
    pthread_t tid[tid_n];
    pthread_create(tid, NULL, writer, q);
    pthread_create(tid + 1, NULL, reader, &qref);
    pthread_create(tid + 2, NULL, reader, &qref2);
    pthread_create(tid + 3, NULL, reader2, &qref3);
    for (int i = 0; i < tid_n; ++i) {
        pthread_join(tid[i], NULL);
        // if (pthread_join(tid[i], NULL) == 0) {
        //     printf("join\n");
        // }
    }

    printf("After join\n");
    qsbr_unregister(q, &qref);
    qsbr_unregister(q, &qref2);
    qsbr_unregister(q, &qref3);
    qsbr_destroy(q);
#ifdef OUTPUT_TO_BUF
    printf("%s", output_buf);
#endif
    return 0;
}
