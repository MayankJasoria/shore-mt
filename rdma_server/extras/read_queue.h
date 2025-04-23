#ifndef READ_QUEUE_H
#define READ_QUEUE_H

#include "common.h" // common.h contains definitions needed by RdmaReadFilePayload
#include <stdint.h> // For uint64_t and size_t
#include <stdbool.h> // For bool
#include <stddef.h>  // For off_t
#include <semaphore.h> // For sem_t
#include <pthread.h> // For pthread_spinlock_t


typedef struct ReadQueue ReadQueue;
typedef struct ReadOperation ReadOperation;

struct ReadOperation {
  RdmaReadFilePayload* payload;
  struct ReadOperation* next;
};

struct ReadQueue {
  ReadOperation* head;
  ReadOperation* tail;
  sem_t sem;
  pthread_spinlock_t headLock;
  pthread_spinlock_t tailLock;
} __attribute__((aligned(128)));

ReadQueue* readQueueInit();

void readEnqueue(ReadQueue* queue, RdmaReadFilePayload* payload);

RdmaReadFilePayload* readDequeue(ReadQueue* queue);

void destroyReadQueue(ReadQueue* queue);

#endif // READ_QUEUE_H