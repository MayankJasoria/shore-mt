// rlibv2/c_wrappers/rdmaio_rc_recv_manager_c.h
#ifndef RDMAIO_RC_RECV_MANAGER_C_H
#define RDMAIO_RC_RECV_MANAGER_C_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <infiniband/verbs.h>
#include "rdmaio_mem_c.h"
#include "rdmaio_rctrl_c.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @brief Opaque type representing the RDMA receive manager.
 */
typedef struct rdmaio_recv_manager_t {
    void* manager;
} rdmaio_recv_manager_t;

/*!
 * @brief Opaque type representing a simple allocator for receive buffers.
 */
typedef struct simple_allocator_t {
    void* allocator;
} simple_allocator_t;

/*!
 * @brief Handle to the receive entries queried from the receive manager.
 * This handle encapsulates the underlying memory management for the receive entries.
 */
typedef struct recv_entries_handle_t {
    void* internal_ptr;
} recv_entries_handle_t;

/*!
 * @brief Creates an RDMA receive manager object.
 * @param ctrl_ptr A pointer to the RDMA control object.
 * @return A pointer to the newly created receive manager object, or NULL on failure.
 */
rdmaio_recv_manager_t* recv_manager_create(rdmaio_rctrl_t* ctrl_ptr);

/*!
 * @brief Creates a simple allocator for receive buffers.
 * @param mem_ptr A pointer to the RDMA memory region to allocate from.
 * @param key The memory region key.
 * @return A pointer to the newly created simple allocator object, or NULL on failure.
 */
simple_allocator_t* simple_allocator_create(rdmaio_rmem_t* mem_ptr, uint32_t key);

/*!
 * @brief Destroys the simple allocator object.
 * @param allocator_ptr A pointer to the simple allocator object to destroy.
 */
void simple_allocator_destroy(simple_allocator_t* allocator_ptr);

/*!
 * @brief Registers a receive completion queue with the receive manager.
 * @param manager_ptr A pointer to the receive manager object.
 * @param name The name to register the receive CQ under.
 * @param cq A pointer to the underlying IBV completion queue.
 * @param allocator_ptr A pointer to the simple allocator to use for this CQ.
 * @return True on successful registration, false otherwise.
 */
bool recv_manager_reg_recv_cq(rdmaio_recv_manager_t* manager_ptr, const char* name, struct ibv_cq* cq, simple_allocator_t* allocator_ptr);

/*!
 * @brief Queries the receive entries associated with a registered name.
 * @param manager_ptr A pointer to the receive manager object.
 * @param name The name of the registered receive entries.
 * @return A handle to the receive entries, or NULL if not found.
 * The caller is responsible for destroying this handle using recv_entries_handle_destroy.
 */
recv_entries_handle_t* recv_manager_query_recv_entries(rdmaio_recv_manager_t* manager_ptr, const char* name);

/*!
 * @brief Destroys the RDMA receive manager object.
 * @param manager_ptr A pointer to the receive manager object to destroy.
 */
void recv_manager_destroy(rdmaio_recv_manager_t* manager_ptr);

/*!
 * @brief Destroys the handle to the receive entries, releasing associated resources.
 * @param handle A pointer to the receive entries handle to destroy.
 */
void recv_entries_handle_destroy(recv_entries_handle_t* handle);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // RDMAIO_RC_RECV_MANAGER_C_H