#include "../extras/write_queue.h"
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

WriteQueue* writeQueueInit() {
  WriteQueue* queue = (WriteQueue*)malloc(sizeof(WriteQueue));
  if (queue == NULL) {
    perror("WriteQueue allocation failed");
    exit(EXIT_FAILURE);
  }
  queue->head = NULL;
  queue->tail = NULL;
  if (sem_init(&queue->sem, 0, 0)) {
    perror("Semaphore initialization failed for WriteQueue");
    exit(EXIT_FAILURE);
  }

  if (!pthread_spin_init(&queue->headLock, PTHREAD_PROCESS_PRIVATE)) {
    perror("Spinlock init for head failed for WriteQueue");
    exit(EXIT_FAILURE);
  }

  if (!pthread_spin_init(&queue->tailLock, PTHREAD_PROCESS_PRIVATE)) {
    perror("Spinlock init for tail failed for WriteQueue");
    exit(EXIT_FAILURE);
  }

  return queue;
}

void writeEnqueue(WriteQueue* queue, RdmaWriteFilePayload* payload) {
  WriteOperation* newNode = (WriteOperation*) malloc(sizeof(WriteOperation));
  if (newNode == NULL) {
    perror("WriteOperation allocation failed");
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

RdmaWriteFilePayload* writeDequeue(WriteQueue* queue) {
  sem_wait(&queue->sem);
  pthread_spin_lock(&queue->headLock);
retry:
  ;
  WriteOperation* dequeueNode = queue->head;
  if (!dequeueNode) {
    pthread_spin_unlock(&queue->headLock);
    return NULL; // Queue is empty, should not happen if sem_wait succeeded
  }
  WriteOperation* next = dequeueNode->next;

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
  RdmaWriteFilePayload* payload = dequeueNode->payload;
  free(dequeueNode);
  pthread_spin_unlock(&queue->headLock);
  return payload;
}

void destroyWriteQueue(WriteQueue* queue) {
  WriteOperation* currentNode = queue->head;
  while (currentNode) {
    WriteOperation* nextNode = currentNode->next;
    free(currentNode->payload); // Assuming payload was allocated separately
    free(currentNode);
    currentNode = nextNode;
  }

  sem_destroy(&queue->sem);
  pthread_spin_destroy(&queue->headLock);
  pthread_spin_destroy(&queue->tailLock);
  free(queue);
}