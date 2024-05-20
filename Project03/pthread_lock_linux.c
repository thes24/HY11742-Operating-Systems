#include <stdio.h>
#include <pthread.h>

int shared_resource = 0;

#define NUM_ITERS 100000
#define NUM_THREADS 100

void lock();
void unlock();

volatile int lock_flag = 0;

static inline int xchg(volatile int *addr, int val) {
    int result;
    __asm__ volatile("lock xchg %0, %1" :
                    "+m" (*addr), "=a" (result) :
                    "1" (val) :
                    "cc");
    return result;
}

void lock(volatile int *lock) {
    while (xchg(lock, 1) == 1) { }
}

void unlock(volatile int *lock) {
    xchg(lock, 0);
}

void* thread_func(void* arg) {
    int tid = *(int*)arg;

    lock(&lock_flag);;

        for(int i = 0; i < NUM_ITERS; i++)    shared_resource++;

    unlock(&lock_flag);

    pthread_exit(NULL);
}

int main() {
    pthread_t threads[NUM_THREADS];
    int tids[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        tids[i] = i;
        pthread_create(&threads[i], NULL, thread_func, &tids[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("shared: %d\n", shared_resource);

    return 0;
}
