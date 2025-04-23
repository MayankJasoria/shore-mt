/**
 * @file file_hash_table.h
 * @brief Header file for a concurrent chained hash table.
 *
 * This hash table stores a mapping of version number (unsigned int) to
 * file descriptor (int). Each bucket uses a read-write lock for concurrency.
 * The chains are implemented as doubly-linked lists.
 */

#ifndef FILE_HASH_TABLE_H
#define FILE_HASH_TABLE_H

#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <semaphore.h>
#include <stdatomic.h>

/**
 * @def TABLE_SIZE
 * @brief The fixed size of the hash table.
 */
#define TABLE_SIZE 1021

/**
 * @struct HashNode
 * @brief Represents a node in the doubly-linked list of the hash table.
 */
typedef struct HashNode {
    unsigned int version; ///< The version number (key).
    int fd;             ///< The file descriptor (value).
    atomic_int pending_aio_count; // Track number of ongoing async I/O requests (we must wait for their completion)
    sem_t completion_sem; // triggers the thread waiting to close the fd to start running when all operations are completed
    struct HashNode* next; ///< Pointer to the next node in the list.
    struct HashNode* prev; ///< Pointer to the previous node in the list.
} HashNode;

/**
 * @struct HashTableBucket
 * @brief Represents a bucket in the hash table.
 */
typedef struct HashTableBucket {
    pthread_rwlock_t lock; ///< Read-write lock for concurrent access.
    HashNode* head;        ///< Pointer to the head of the doubly-linked list.
} HashTableBucket;

/**
 * @struct HashTable
 * @brief Represents the hash table structure.
 */
typedef struct HashTable {
    HashTableBucket buckets[TABLE_SIZE]; ///< Array of hash table buckets.
} HashTable;

/**
 * @brief Creates and initializes a new hash table.
 * @return A pointer to the newly created hash table, or NULL on failure.
 */
HashTable* createHashTable();

/**
 * @brief Destroys a hash table, freeing all allocated memory.
 * @param table A pointer to the hash table to be destroyed.
 */
void destroyHashTable(HashTable* table);

/**
 * @brief Inserts a new version-file descriptor mapping into the hash table.
 * @param table A pointer to the hash table.
 * @param version The version number (key) to insert.
 * @param fd The file descriptor (value) to insert.
 * @return 0 on success, -1 on failure (e.g., memory allocation error, lock acquisition failure).
 */
int insert(HashTable* table, unsigned int version, int fd);

/**
 * @brief Retrieves the hash table node associated with a given file descriptor.
 * @param table A pointer to the hash table.
 * @param fd The file descriptor to search for.
 * @return A pointer to the HashNode containing the file descriptor, or NULL if not found.
 */
HashNode* get(HashTable* table, int fd);

/**
 * @brief Deletes a record from the hash table based on the version number (key).
 * @param table A pointer to the hash table.
 * @param version The version number to delete.
 * @return 0 on success (record found and deleted), -1 if the version is not found or lock acquisition fails.
 */
int deleteByVersion(HashTable* table, unsigned int version);

/**
 * @brief Deletes a specific record (node) from the hash table.
 * @param table A pointer to the hash table.
 * @param nodeToDelete A pointer to the HashNode to delete.
 * @return 0 on success (node found and deleted), -1 if the node is not found or lock acquisition fails.
 */
int deleteByRecord(HashTable* table, HashNode* nodeToDelete);

#endif // FILE_HASH_TABLE_H