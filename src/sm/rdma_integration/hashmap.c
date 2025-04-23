#include "rdma_integration/hashmap.h"
#include <stdio.h>

HashTable* initHashTable() {
    HashTable* table = (HashTable*) malloc(sizeof(HashTable) * TABLE_SIZE);
    if (!table) {
        perror("Failed to allocate hash table");
        return NULL;
    }
    for (int i = 0; i < TABLE_SIZE; ++i) {
        if (pthread_spin_init(&table[i].lock, PTHREAD_PROCESS_PRIVATE) != 0) {
            perror("Failed to initialize spinlock");
            // Consider cleanup here if allocation succeeded for some buckets
            free(table);
            return NULL;
        }
        table[i].head = NULL;
        table[i].tail = NULL;
    }
    return table;
}

/*
 * The keys are guaranteed to be monotonically increasing for the most part
 * so use of key % size pretty much guarantees uniform distribution by itself
 */
static inline int hashFunction(unsigned int reqId) {
    return (int) (reqId % (unsigned int) TABLE_SIZE);
}

ChainNode* insert(HashTable* table, unsigned int reqId) {
    int index = hashFunction(reqId);
    ChainNode* newNode = (ChainNode*) malloc(sizeof(ChainNode));
    if (newNode == NULL) {
        perror("Failed to allocate chain node");
        return NULL; // Indicate failure
    }
    newNode->reqId = reqId;
    if (sem_init(&newNode->response_ready, 0, 0) != 0) {
        perror("Failed to initialize semaphore");
        free(newNode);
        return NULL;
    }
    newNode->response_set = false;
    newNode->next = NULL;
    newNode->prev = NULL;

    if (pthread_spin_lock(&table[index].lock) != 0) {
        perror("Failed to acquire spinlock");
        sem_destroy(&newNode->response_ready);
        free(newNode);
        return NULL;
    }

    if (table[index].tail == NULL) {
        table[index].head = newNode;
        table[index].tail = newNode;
    } else {
        table[index].tail->next = newNode;
        newNode->prev = table[index].tail;
        table[index].tail = newNode;
    }

    if (pthread_spin_unlock(&table[index].lock) != 0) {
        perror("Failed to release spinlock");
        // Handle error - might lead to deadlock if not careful
    }
    return newNode; // Return the newly created node
}

int modify(HashTable* table, unsigned int reqId, RdmaSyscallResponse response_data) {
    int index = hashFunction(reqId);
    ChainNode* current;

    if (pthread_spin_lock(&table[index].lock) != 0) {
        perror("Failed to acquire spinlock");
        return -1;
    }

    current = table[index].head;
    while (current != NULL) {
        if (current->reqId == reqId) {
            current->response = response_data;
            atomic_store(&current->response_set, true);
            sem_post(&current->response_ready);
            pthread_spin_unlock(&table[index].lock);
            return 0; // Found and modified
        }
        current = current->next;
    }

    pthread_spin_unlock(&table[index].lock);
    return -1; // Not found
}

int deleteNode(HashTable* table, ChainNode* nodeToDelete) {
    if (nodeToDelete == NULL) {
        return -1; // Or some other error code for null node
    }

    int index = hashFunction(nodeToDelete->reqId);
    int result = -1;

    if (pthread_spin_lock(&table[index].lock) != 0) {
        perror("Failed to acquire spinlock for delete");
        return -1;
    }

    // Now, perform the deletion from the doubly linked list
    if (nodeToDelete->prev == NULL) {
        table[index].head = nodeToDelete->next;
    } else {
        nodeToDelete->prev->next = nodeToDelete->next;
    }

    if (nodeToDelete->next == NULL) {
        table[index].tail = nodeToDelete->prev;
    } else {
        nodeToDelete->next->prev = nodeToDelete->prev;
    }

    if (pthread_spin_unlock(&table[index].lock) != 0) {
        perror("Failed to release spinlock for delete");
    }

    sem_destroy(&nodeToDelete->response_ready);
    free(nodeToDelete);
    result = 0;

    return result;
}

ChainNode* findFirstEntry(HashTable* table) {
    for (int i = 0; i < TABLE_SIZE; ++i) {
        if (pthread_spin_lock(&table[i].lock) == 0) {
            ChainNode* current = table[i].head;
            while (current != NULL) {
                bool response_is_set = atomic_load(&current->response_set);
                if (!response_is_set) {
                    pthread_spin_unlock(&table[i].lock);
                    return current;
                }
                current = current->next;
            }
            pthread_spin_unlock(&table[i].lock);
        }
    }
    return NULL; // No incomplete request found
}

void destroyHashTable(HashTable* table) {
    if (table == NULL) {
        return;
    }
    for (int i = 0; i < TABLE_SIZE; ++i) {
        ChainNode* current = table[i].head;
        while (current != NULL) {
            ChainNode* next = current->next;
            sem_destroy(&current->response_ready);
            free(current);
            current = next;
        }
        pthread_spin_destroy(&table[i].lock);
    }
    free(table);
}