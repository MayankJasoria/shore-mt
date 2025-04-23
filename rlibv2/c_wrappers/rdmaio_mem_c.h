#ifndef RDMAIO_MEM_C_H
#define RDMAIO_MEM_C_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @brief Opaque type representing an RDMA memory region.
 */
typedef struct rdmaio_rmem_t {
  void* mem;
} rdmaio_rmem_t;

/*!
 * @brief Creates an RMem object.
 * @param size The size of the memory region in bytes.
 * @return A pointer to the newly created RMem object, or NULL on failure.
 */
rdmaio_rmem_t* rmem_create(uint64_t size);

/*!
 * @brief Checks if the RMem object is valid.
 * @param mem A pointer to the RMem object.
 * @return True if the memory pointer is valid, false otherwise.
 */
bool rmem_valid(rdmaio_rmem_t* mem);

/*!
 * @brief Destroys the RMem object and releases its allocated memory.
 * @param mem A pointer to the RMem object to destroy.
 */
void rmem_destroy(rdmaio_rmem_t* mem);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // RDMAIO_MEM_C_H