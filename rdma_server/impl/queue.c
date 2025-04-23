#include "../extras/queue.h"
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct node {
  OperationLog* log;
  struct node* next;
} Node;

struct queue {
  Node* head;
  Node* tail;
  sem_t sem;
  pthread_spinlock_t headLock;
  pthread_spinlock_t tailLock;
} __attribute__((aligned(128)));

Queue* queueInit() {
  Queue* queue = (Queue*)malloc(sizeof(Queue));
  if (queue == NULL) {
    perror("Queue allocation failed");
    exit(EXIT_FAILURE);
  }
  queue->head = NULL;
  queue->tail = NULL;
  if (sem_init(&queue->sem, 0, 0)) {
    perror("semaphote initialization failed");
    exit(EXIT_FAILURE);
  }

  if (!pthread_spin_init(&queue->headLock, PTHREAD_PROCESS_PRIVATE)) {
    perror("Spinlock init for head failed");
    exit(EXIT_FAILURE);
  }

  if (!pthread_spin_init(&queue->tailLock, PTHREAD_PROCESS_PRIVATE)) {
    perror("Spinlock init for tail failed");
    exit(EXIT_FAILURE);
  }

  return queue;
}

void enqueue(Queue* queue, OperationLog* log) {
  Node* newNode = (Node*) malloc(sizeof(Node));
  if (newNode == NULL) {
    perror("Node allocation failed");
    exit(EXIT_FAILURE);
  }
  newNode->log = log;
  newNode->next = NULL;

  pthread_spin_lock(&queue->tailLock);

  if (queue->tail) {
    queue->tail->next = newNode;
    queue->tail = newNode;
  } else {
    pthread_spin_lock(&queue->headLock);
    queue->head = newNode;
    pthread_spin_unlock(&queue->headLock);
    queue->tail = newNode;
  }

  pthread_spin_unlock(&queue->tailLock);
  sem_post(&queue->sem);
  return;
}

OperationLog* dequeue(Queue* queue) {
  sem_wait(&queue->sem);
  pthread_spin_lock(&queue->headLock);
retry:
  ;
  Node* dequeueNode = queue->head;
	Node* next = dequeueNode->next;

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
	OperationLog* log = dequeueNode->log;
	free(dequeueNode);
	pthread_spin_unlock(&queue->headLock);
	return log;
}

void destroyQueue(Queue* queue) {
	// TODO: Consider removing this and enforce an 'owner-side' guarantee that it will dequeue everything
	while (queue->head) {
		Node* delNode = queue->head;
		queue->head = queue->head->next;
		free(delNode->log);
		free(delNode);
	}

	sem_destroy(&queue->sem);
	free(queue);
}