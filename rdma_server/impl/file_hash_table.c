// file_hash_table.c
#include "../extras/file_hash_table.h"

/*
 * The keys are guaranteed to be monotonically increasing for the most part
 * so use of key % size pretty much guarantees uniform distribution by itself
 */
static inline int hashFunction(unsigned int version) {
    return (int) (version % (unsigned int) TABLE_SIZE);
}

HashTable* createHashTable() {
    HashTable* table = (HashTable*) malloc(sizeof(HashTable));
    if (!table) {
        perror("Failed to allocate memory for hash table");
        return NULL;
    }
    // Initialize each bucket
    for (int i = 0; i < TABLE_SIZE; ++i) {
        // Initialize the read-write lock
        if (pthread_rwlock_init(&table->buckets[i].lock, NULL) != 0) {
            perror("Failed to initialize read-write lock");
            // Handle cleanup if initialization fails
            for (int j = 0; j < i; ++j) {
                pthread_rwlock_destroy(&table->buckets[j].lock);
            }
            free(table);
            return NULL;
        }
        // Initialize the head of the linked list
        table->buckets[i].head = NULL;
    }
    return table;
}

void destroyHashTable(HashTable* table) {
    if (!table) return;
    // Iterate through each bucket
    for (int i = 0; i < TABLE_SIZE; ++i) {
        // Destroy the read-write lock
        pthread_rwlock_destroy(&table->buckets[i].lock);
        // Free all nodes in the linked list
        HashNode* current = table->buckets[i].head;
        while (current) {
            HashNode* next = current->next;
            if (sem_destroy(&current->completion_sem) != 0) {
                perror("Failed to destroy semaphore during destroyHashTable");
            }
            free(current);
            current = next;
        }
    }
    free(table);
}

int insert(HashTable* table, unsigned int version, int fd) {
    if (!table) return -1;
    int index = hashFunction(version);
    // Allocate memory for the new node
    HashNode* newNode = (HashNode*)malloc(sizeof(HashNode));
    if (!newNode) {
        perror("Failed to allocate memory for new node");
        return -1;
    }
    newNode->version = version;
    newNode->fd = fd;
    newNode->pending_aio_count = 0; // Initialize the pending AIO counter
    if (sem_init(&newNode->completion_sem, 0, 0) != 0) { // Initialize the semaphore
        perror("Failed to initialize semaphore");
        free(newNode);
        return -1;
    }
    newNode->next = NULL;
    newNode->prev = NULL;

    // Acquire write lock for the bucket
    if (pthread_rwlock_wrlock(&table->buckets[index].lock) == 0) {
        // Insert at the beginning of the linked list
        if (table->buckets[index].head == NULL) {
            table->buckets[index].head = newNode;
        } else {
            newNode->next = table->buckets[index].head;
            table->buckets[index].head->prev = newNode;
            table->buckets[index].head = newNode;
        }
        // Release write lock
        pthread_rwlock_unlock(&table->buckets[index].lock);
        return 0;
    } else {
        perror("Failed to acquire write lock");
        sem_destroy(&newNode->completion_sem); // Clean up semaphore if lock acquisition fails
        free(newNode);
        return -1;
    }
}


HashNode* get(HashTable* table, int fd) {
    if (!table) return NULL;
    // Iterate through each bucket
    for (int i = 0; i < TABLE_SIZE; ++i) {
        // Acquire read lock for the bucket
        if (pthread_rwlock_rdlock(&table->buckets[i].lock) == 0) {
            // Traverse the linked list
            HashNode* current = table->buckets[i].head;
            while (current) {
                if (current->fd == fd) {
                    // Release read lock and return the node
                    pthread_rwlock_unlock(&table->buckets[i].lock);
                    return current;
                }
                current = current->next;
            }
            // Release read lock if not found in this bucket
            pthread_rwlock_unlock(&table->buckets[i].lock);
        } else {
            perror("Failed to acquire read lock");
            // Continue to the next bucket, but ideally handle this error more robustly
        }
    }
    return NULL; // File descriptor not found
}

int deleteByVersion(HashTable* table, unsigned int version) {
    if (!table) return -1;
    int index = hashFunction(version);

    // Acquire write lock for the bucket
    if (pthread_rwlock_wrlock(&table->buckets[index].lock) == 0) {
        HashNode* current = table->buckets[index].head;
        // Traverse the linked list
        while (current) {
            if (current->version == version) {
                // Handle deletion based on node position
                if (current->prev == NULL) {
                    table->buckets[index].head = current->next;
                    if (table->buckets[index].head != NULL) {
                        table->buckets[index].head->prev = NULL;
                    }
                } else {
                    current->prev->next = current->next;
                    if (current->next != NULL) {
                        current->next->prev = current->prev;
                    }
                }
                if (sem_destroy(&current->completion_sem) != 0) {
                    perror("Failed to destroy semaphore during deleteByVersion");
                }
                free(current);
                // Release write lock and return success
                pthread_rwlock_unlock(&table->buckets[index].lock);
                return 0;
            }
            current = current->next;
        }
        // Release write lock if version not found
        pthread_rwlock_unlock(&table->buckets[index].lock);
        return -1; // Version not found
    } else {
        perror("Failed to acquire write lock");
        return -1;
    }
}

int deleteByRecord(HashTable* table, HashNode* nodeToDelete) {
    if (!table || !nodeToDelete) return -1;
    int index = hashFunction(nodeToDelete->version); // Assuming version is a reliable way to find the bucket

    // Acquire write lock for the bucket
    if (pthread_rwlock_wrlock(&table->buckets[index].lock) == 0) {
        HashNode* current = table->buckets[index].head;
        // Traverse the linked list
        while (current) {
            if (current == nodeToDelete) {
                // Handle deletion based on node position
                if (current->prev == NULL) {
                    table->buckets[index].head = current->next;
                    if (table->buckets[index].head != NULL) {
                        table->buckets[index].head->prev = NULL;
                    }
                } else {
                    current->prev->next = current->next;
                    if (current->next != NULL) {
                        current->next->prev = current->prev;
                    }
                }
                if (sem_destroy(&current->completion_sem) != 0) {
                    perror("Failed to destroy semaphore during deleteByRecord");
                }
                free(current);
                // Release write lock and return success
                pthread_rwlock_unlock(&table->buckets[index].lock);
                return 0;
            }
            current = current->next;
        }
        // Release write lock if node not found
        pthread_rwlock_unlock(&table->buckets[index].lock);
        return -1; // Node not found (shouldn't happen if the caller has the correct node)
    } else {
        perror("Failed to acquire write lock");
        return -1;
    }
}