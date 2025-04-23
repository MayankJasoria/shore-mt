#ifndef WRITE_QUEUE_H
#define WRITE_QUEUE_H

#include "common.h"
#include <stdint.h> // For uint64_t and size_t
#include <stdbool.h> // For bool
#include <stddef.h>  // For off_t
#include <semaphore.h> // For sem_t
#include <pthread.h> // For pthread_spinlock_t

typedef struct WriteQueue WriteQueue;
typedef struct WriteOperation WriteOperation;

struct WriteOperation {
    RdmaWriteFilePayload* payload;
    struct WriteOperation* next;
};

struct WriteQueue {
    WriteOperation* head;
    WriteOperation* tail;
    sem_t sem;
    pthread_spinlock_t headLock;
    pthread_spinlock_t tailLock;
} __attribute__((aligned(128)));

WriteQueue* writeQueueInit();

void writeEnqueue(WriteQueue* queue, RdmaWriteFilePayload* payload);

RdmaWriteFilePayload* writeDequeue(WriteQueue* queue);

void destroyWriteQueue(WriteQueue* queue);

#endif // WRITE_QUEUE_H