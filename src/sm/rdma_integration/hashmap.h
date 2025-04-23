#ifndef HASHMAP_H
#define HASHMAP_H

#include <stdlib.h>
#include <pthread.h>
#include <stdbool.h>
#include <semaphore.h>
#include <stdatomic.h>

#include "rdma_structs.h"

// Define your desired table size here
#ifndef TABLE_SIZE
#define TABLE_SIZE 1021
#endif

typedef struct chainNode {
    unsigned int reqId;
    sem_t response_ready;
    volatile RdmaSyscallResponse response;
    volatile atomic_bool response_set;
    struct chainNode* next;
    struct chainNode* prev;
} ChainNode;

struct mapElement {
    pthread_spinlock_t lock;
    ChainNode* head;
    ChainNode* tail;
};

typedef struct mapElement HashTable;

// Initialize the hash table
HashTable* initHashTable();

// Insert a new request and return the pointer to the chainNode
ChainNode* insert(HashTable* table, unsigned int reqId);

// Modify the response for a given request ID
int modify(HashTable* table, unsigned int reqId, RdmaSyscallResponse response);

// Delete a node from the hash table using the chainNode pointer
int deleteNode(HashTable* table, ChainNode* nodeToDelete);

// Find the first reachable entry in the hash table
ChainNode* findFirstEntry(HashTable* table);

// Destroy the hash table and free memory
void destroyHashTable(HashTable* table);

#endif //HASHMAP_H
