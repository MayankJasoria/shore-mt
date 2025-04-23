#ifndef RDMAIO_NIC_C_H
#define RDMAIO_NIC_C_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "rdmaio_result_c.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @brief Represents the index of an RDMA device.
 */
typedef struct rdmaio_devidx_t {
    int dev_id;
    int port_id;
} rdmaio_devidx_t;

/*!
 * @brief Opaque type representing an RDMA Network Interface Controller (NIC).
 */
typedef struct rdmaio_nic_t {
    void* nic;
} rdmaio_nic_t;

/*!
 * @brief Queries all available RDMA NICs on the host machine.
 * @param count Output parameter to store the number of found NICs.
 * @return An array of rdmaio_devidx_t representing the indices of the found NICs.
 * The caller is responsible for freeing this memory using rnic_info_free_dev_names().
 * Returns NULL on error.
 */
rdmaio_devidx_t* rnic_info_query_dev_names(size_t* count);

/*!
 * @brief Frees the memory allocated by rnic_info_free_dev_names().
 * @param dev_idxs The array of NIC device indices to free.
 */
void rnic_info_free_dev_names(rdmaio_devidx_t* dev_idxs);

/*!
 * @brief Creates an RDMA NIC handler.
 * @param idx The index of the NIC to open.
 * @param gid The GID index to use.
 * @return A pointer to the newly created NIC object, or NULL on failure.
 */
rdmaio_nic_t* rnic_create(rdmaio_devidx_t idx, uint8_t gid);

/*!
 * @brief Checks if the RDMA NIC handler is valid.
 * @param nic A pointer to the RDMA NIC object.
 * @return True if the NIC is valid, false otherwise.
 */
bool rnic_valid(const rdmaio_nic_t* nic);

/*!
 * @brief Gets the underlying ibv_context of the RDMA NIC.
 * @param nic A pointer to the RDMA NIC object.
 * @return A pointer to the ibv_context.
 */
void* rnic_get_ctx(const rdmaio_nic_t* nic);

/*!
 * @brief Gets the underlying ibv_pd (Protection Domain) of the RDMA NIC.
 * @param nic A pointer to the RDMA NIC object.
 * @return A pointer to the ibv_pd.
 */
void* rnic_get_pd(const rdmaio_nic_t* nic);

/*!
 * @brief Checks if the RDMA NIC port is active.
 * @param nic A pointer to the RDMA NIC object.
 * @param err_msg Buffer to store an error message if the port is not active. Can be NULL if no error message is needed.
 * @param err_msg_size Size of the error message buffer.
 * @return RDMAIO_OK if the port is active, RDMAIO_ERR otherwise.
 */
rdmaio_iocode_t rnic_is_active(const rdmaio_nic_t* nic, char* err_msg, size_t err_msg_size);

/*!
 * @brief Destroys the RDMA NIC object and releases its resources.
 * @param nic A pointer to the RDMA NIC object to destroy.
 */
void rnic_destroy(rdmaio_nic_t* nic);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // RDMAIO_NIC_C_H