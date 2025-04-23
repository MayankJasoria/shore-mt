// rlibv2/c_wrappers/rdmaio_impl_c.h
#ifndef RDMAIO_IMPL_C_H
#define RDMAIO_IMPL_C_H

#include <stddef.h>
#include <infiniband/verbs.h> // Include for ibv_cq
#include "rdmaio_nic_c.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @brief Creates a Completion Queue.
 * @param nic A pointer to the RDMA NIC.
 * @param cq_sz The size of the Completion Queue.
 * @param err_msg Buffer to store an error message if the operation fails. Can be NULL.
 * @param err_msg_size Size of the error message buffer.
 * @return A pointer to the created ibv_cq object, or NULL on failure.
 */
struct ibv_cq* rdmaio_create_cq(rdmaio_nic_t* nic, int cq_sz, char* err_msg, size_t err_msg_size);

/*!
 * @brief Destroys a Completion Queue.
 * @param cq The ibv_cq object to destroy.
 */
void rdmaio_destroy_cq(struct ibv_cq* cq);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // RDMAIO_IMPL_C_H