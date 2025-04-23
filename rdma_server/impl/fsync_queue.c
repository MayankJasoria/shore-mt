#include "../extras/fsync_queue.h"
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

typedef struct FsyncNode {
  FsyncOperation* operation;
  struct FsyncNode* next;
} FsyncNode;

FsyncQueue* fsyncQueueInit() {
  FsyncQueue* queue = (FsyncQueue*)malloc(sizeof(FsyncQueue));
  if (queue == NULL) {
    perror("FsyncQueue allocation failed");
    exit(EXIT_FAILURE);
  }
  queue->head = NULL;
  queue->tail = NULL;
  if (sem_init(&queue->sem, 0, 0)) {
    perror("Semaphore initialization failed for FsyncQueue");
    exit(EXIT_FAILURE);
  }

  if (!pthread_spin_init(&queue->headLock, PTHREAD_PROCESS_PRIVATE)) {
    perror("Spinlock init for head failed for FsyncQueue");
    exit(EXIT_FAILURE);
  }

  if (!pthread_spin_init(&queue->tailLock, PTHREAD_PROCESS_PRIVATE)) {
    perror("Spinlock init for tail failed for FsyncQueue");
    exit(EXIT_FAILURE);
  }

  return queue;
}

void fsyncEnqueue(FsyncQueue* queue, FsyncOperation* operation) {
  FsyncNode* newNode = (FsyncNode*) malloc(sizeof(FsyncNode));
  if (newNode == NULL) {
    perror("FsyncNode allocation failed");
    exit(EXIT_FAILURE);
  }
  newNode->operation = operation;
  newNode->next = NULL;

  pthread_spin_lock(&queue->tailLock);

  if (queue->tail) {
    queue->tail->next = newNode;
    queue->tail = newNode;
  } else {
    pthread_spin_lock(&queue->headLock);
    assert(!queue->head);
    queue->head = newNode;
    pthread_spin_unlock(&queue->headLock);
    queue->tail = newNode;
  }

  pthread_spin_unlock(&queue->tailLock);
  sem_post(&queue->sem);
  return;
}

FsyncOperation* fsyncDequeue(FsyncQueue* queue) {
  sem_wait(&queue->sem);
  pthread_spin_lock(&queue->headLock);
retry:
  ;
  FsyncNode* dequeueNode = queue->head;
  if (!dequeueNode) {
    pthread_spin_unlock(&queue->headLock);
    return NULL; // Queue is empty, should not happen if sem_wait succeeded
  }
  FsyncNode* next = dequeueNode->next;

  if (!next) {
    pthread_spin_lock(&queue->tailLock);
    if (dequeueNode->next) {
      pthread_spin_unlock(&queue->tailLock);
      goto retry;
    }
    queue->tail = NULL;
    pthread_spin_unlock(&queue->tailLock);
  }
  queue->head = next; // handles both - next was a node or next was NULL
  FsyncOperation* operation = dequeueNode->operation;
  free(dequeueNode);
  pthread_spin_unlock(&queue->headLock);
  return operation;
}

void destroyFsyncQueue(FsyncQueue* queue) {
  FsyncNode* currentNode = queue->head;
  while (currentNode) {
    FsyncNode* nextNode = currentNode->next;
    free(currentNode->operation->aio_context); // Assuming aio_context was allocated separately
    free(currentNode->operation);
    free(currentNode);
    currentNode = nextNode;
  }

  sem_destroy(&queue->sem);
  pthread_spin_destroy(&queue->headLock);
  pthread_spin_destroy(&queue->tailLock);
  free(queue);
}