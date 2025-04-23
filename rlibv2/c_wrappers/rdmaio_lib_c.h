// rlibv2/c_wrappers/rdmaio_lib_c.h
#ifndef RDMAIO_CONNECT_MANAGER_C_H
#define RDMAIO_CONNECT_MANAGER_C_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "rdmaio_result_c.h"
#include "rdmaio_config_c.h"
#include "rdmaio_regattr_c.h"
#include "rdmaio_rc_c.h"
#include "rdmaio_rctrl_c.h"
#include "rdmaio_nic_c.h"

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration for the opaque ConnectManager type
typedef struct rdmaio_connect_manager_t rdmaio_connect_manager_t;

/*!
 * @brief Creates a ConnectManager object.
 * @param addr The address of the remote server (e.g., "localhost:1111").
 * @return A pointer to the created ConnectManager object, or NULL on failure.
 */
rdmaio_connect_manager_t* rdmaio_connect_manager_create(const char* addr);

/*!
 * @brief Waits for the remote server to be ready.
 * @param cm The ConnectManager object.
 * @param timeout_sec The timeout in seconds.
 * @param retry The number of times to retry.
 * @return The status of the operation (RDMAIO_OK on success, RDMAIO_TIMEOUT if timed out, etc.).
 */
rdmaio_iocode_t rdmaio_connect_manager_wait_ready(rdmaio_connect_manager_t* cm, double timeout_sec, size_t retry);

/*!
 * @brief Creates and connects an RC queue pair with a message channel on the remote end.
 * @param cm The ConnectManager object.
 * @param qp_name The name for the remote queue pair.
 * @param channel_name The name for the remote receive channel.
 * @param max_msg_size The maximum size of messages for the channel.
 * @param rc A pointer to the local RC queue pair object.
 * @param remote_nic_id The ID of the remote NIC to use for connection.
 * @param config Configuration parameters for the remote queue pair. Pass NULL for default configuration.
 * @return The status of the operation.
 */
rdmaio_iocode_t rdmaio_connect_manager_cc_rc_msg(rdmaio_connect_manager_t* cm,
                                                        const char* qp_name,
                                                        const char* channel_name,
                                                        size_t max_msg_size,
                                                        rdmaio_rc_t* rc,
                                                        uint32_t remote_nic_id,
                                                        const rdmaio_qpconfig_t* config);

/*!
 * @brief Fetches the remote memory registration attributes.
 * @param cm The ConnectManager object.
 * @param id The identifier of the remote registered memory.
 * @param out_attr Pointer to a structure where the registration attributes will be stored.
 * @return The status of the operation.
 */
rdmaio_iocode_t rdmaio_connect_manager_fetch_remote_mr(rdmaio_connect_manager_t* cm,
                                                               uint64_t id,
                                                               rdmaio_regattr_t* out_attr);

/*!
 * @brief Destroys the ConnectManager object.
 * @param cm The ConnectManager object to destroy.
 */
void rdmaio_connect_manager_destroy(rdmaio_connect_manager_t* cm);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // RDMAIO_CONNECT_MANAGER_C_H