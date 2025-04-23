#ifndef FSYNC_QUEUE_H
#define FSYNC_QUEUE_H

#include <stdint.h> // For uint64_t
#include <aio.h>
#include <semaphore.h> // For sem_t
#include <pthread.h> // For pthread_spinlock_t

typedef struct FsyncQueue FsyncQueue;
typedef struct FsyncOperation FsyncOperation;

struct FsyncOperation {
    uint32_t lsn_partition;
    uint64_t lsn_offset;
    struct aiocb* aio_context;
	struct HashNode* fileNode;
};

struct FsyncQueue {
    struct FsyncNode* head;
    struct FsyncNode* tail;
    sem_t sem;
    pthread_spinlock_t headLock;
    pthread_spinlock_t tailLock;
} __attribute__((aligned(128)));

FsyncQueue* fsyncQueueInit();

void fsyncEnqueue(FsyncQueue* queue, FsyncOperation* operation);

FsyncOperation* fsyncDequeue(FsyncQueue* queue);

void destroyFsyncQueue(FsyncQueue* queue);

#endif // FSYNC_QUEUE_H