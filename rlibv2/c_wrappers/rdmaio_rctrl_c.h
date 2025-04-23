#ifndef RDMAIO_RCTRL_C_H
#define RDMAIO_RCTRL_C_H

#include <stdbool.h>
#include <stdint.h> // Include for int64_t
#include "rdmaio_nic_c.h"
#include "rdmaio_reg_handler_c.h"
#include "rdmaio_qp_c.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @brief Opaque type representing the RDMA Control Path Daemon (RCtrl).
 */
typedef struct rdmaio_rctrl_t {
    void* ctrl;
} rdmaio_rctrl_t;

/*!
 * @brief Creates an RCtrl object.
 * @param port The port number for the RCtrl daemon to listen on.
 * @param host The hostname or IP address to bind to (default: "localhost").
 * @return A pointer to the newly created RCtrl object, or NULL on failure.
 */
rdmaio_rctrl_t* rctrl_create(unsigned short port, const char* host);

/*!
 * @brief Starts the daemon thread for handling RDMA connection requests.
 * @param ctrl A pointer to the RCtrl object.
 * @return True if the daemon thread was successfully started, false otherwise.
 */
bool rctrl_start_daemon(rdmaio_rctrl_t* ctrl);

/*!
 * @brief Stops the daemon thread for handling RDMA connection requests.
 * @param ctrl A pointer to the RCtrl object.
 */
void rctrl_stop_daemon(rdmaio_rctrl_t* ctrl);

/*!
 * @brief Registers a memory region handler with the RCtrl daemon.
 * @param ctrl A pointer to the RCtrl object.
 * @param key The key to register the memory region handler under.
 * @param handler A pointer to the memory region handler object.
 * @return True if the registration was successful, false otherwise.
 */
bool rdmaio_rctrl_register_mr(rdmaio_rctrl_t* ctrl, int64_t key, rdmaio_reg_handler_t* handler);

/*!
 * @brief Queries a registered queue pair from the RCtrl daemon.
 * @param ctrl A pointer to the RCtrl object.
 * @param name The name of the queue pair to query.
 * @return A pointer to the registered RC queue pair object, or NULL if not found.
 */
rdmaio_qp_t* rctrl_query_qp(rdmaio_rctrl_t* ctrl, const char* name);

/*!
 * @brief Registers an opened NIC with the RCtrl daemon.
 * @param ctrl A pointer to the RCtrl object.
 * @param name The name (or ID) to register the NIC under.
 * @param nic A pointer to the opened NIC object.
 * @return True if the registration was successful, false otherwise.
 */
bool rctrl_register_nic(rdmaio_rctrl_t* ctrl_ptr, uint64_t nic_id, rdmaio_nic_t* nic_ptr);

/*!
 * @brief Destroys the RCtrl object and releases its resources.
 * @param ctrl A pointer to the RCtrl object to destroy.
 */
void rctrl_destroy(rdmaio_rctrl_t* ctrl);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // RDMAIO_RCTRL_C_H