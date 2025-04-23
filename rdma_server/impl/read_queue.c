#include "../extras/read_queue.h"
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

ReadQueue* readQueueInit() {
  ReadQueue* queue = (ReadQueue*)malloc(sizeof(ReadQueue));
  if (queue == NULL) {
    perror("ReadQueue allocation failed");
    exit(EXIT_FAILURE);
  }
  queue->head = NULL;
  queue->tail = NULL;
  if (sem_init(&queue->sem, 0, 0)) {
    perror("Semaphore initialization failed for ReadQueue");
    exit(EXIT_FAILURE);
  }

  if (!pthread_spin_init(&queue->headLock, PTHREAD_PROCESS_PRIVATE)) {
    perror("Spinlock init for head failed for ReadQueue");
    exit(EXIT_FAILURE);
  }

  if (!pthread_spin_init(&queue->tailLock, PTHREAD_PROCESS_PRIVATE)) {
    perror("Spinlock init for tail failed for ReadQueue");
    exit(EXIT_FAILURE);
  }

  return queue;
}

void readEnqueue(ReadQueue* queue, RdmaReadFilePayload* payload) {
  ReadOperation* newNode = (ReadOperation*) malloc(sizeof(ReadOperation));
  if (newNode == NULL) {
    perror("ReadOperation allocation failed");
    exit(EXIT_FAILURE);
  }
  newNode->payload = payload;
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

RdmaReadFilePayload* readDequeue(ReadQueue* queue) {
  sem_wait(&queue->sem);
  pthread_spin_lock(&queue->headLock);
retry:
  ;
  ReadOperation* dequeueNode = queue->head;
  if (!dequeueNode) {
    pthread_spin_unlock(&queue->headLock);
    return NULL; // Queue is empty, should not happen if sem_wait succeeded
  }
  ReadOperation* next = dequeueNode->next;

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
  RdmaReadFilePayload* payload = dequeueNode->payload;
  free(dequeueNode);
  pthread_spin_unlock(&queue->headLock);
  return payload;
}

void destroyReadQueue(ReadQueue* queue) {
  ReadOperation* currentNode = queue->head;
  while (currentNode) {
    ReadOperation* nextNode = currentNode->next;
    free(currentNode->payload); // Assuming payload was allocated separately, same as before
    free(currentNode);
    currentNode = nextNode;
  }

  sem_destroy(&queue->sem);
  pthread_spin_destroy(&queue->headLock);
  pthread_spin_destroy(&queue->tailLock);
  free(queue);
}