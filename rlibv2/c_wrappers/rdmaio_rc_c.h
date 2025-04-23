#ifndef RDMAIO_RC_C_H
#define RDMAIO_RC_C_H

#include <stddef.h>
#include <stdint.h>
#include <infiniband/verbs.h> // Include libibverbs header

#ifdef __cplusplus
extern "C" {
#endif

// Forward declaration for the main RC type
typedef struct rdmaio_rc_t rdmaio_rc_t;

// Include the wrapper headers for other structures
#include "rdmaio_config_c.h"
#include "rdmaio_regattr_c.h"
#include "rdmaio_qpattr_c.h"
#include "rdmaio_result_c.h"
#include "rdmaio_nic_c.h"

// --- RC Class ---

/*!
 * @brief Represents a reliable connected (RC) queue pair.
 */
struct rdmaio_rc_t {
    void* rc; // Opaque pointer to the RC object
};

/*!
 * @brief Creates a reliable connected (RC) queue pair.
 * @param nic A pointer to the network interface card (RNic) object.
 * @param config Configuration parameters for the queue pair. Must not be NULL.
 * @param recv_cq Completion queue to be used for receive operations. Pass NULL if not needed.
 * @return A pointer to the created RC queue pair object, or NULL on failure.
 */
rdmaio_rc_t* rdmaio_rc_create(rdmaio_nic_t* nic, const rdmaio_qpconfig_t* config, struct ibv_cq* recv_cq);

/*!
 * @brief Destroys an RC queue pair object.
 * @param rc The RC queue pair object to destroy.
 */
void rdmaio_rc_destroy(rdmaio_rc_t* rc);

/*!
 * @brief Retrieves the attributes of the local RC queue pair.
 * @param rc The RC queue pair object.
 * @param out_attr Pointer to a structure where the attributes will be stored.
 */
void rdmaio_rc_my_attr(const rdmaio_rc_t* rc, rdmaio_qpattr_t* out_attr);

/*!
 * @brief Retrieves the current status of the RC queue pair.
 * @param rc The RC queue pair object.
 * @return The status of the queue pair as a rdmaio_iocode_t value.
 */
rdmaio_iocode_t rdmaio_rc_my_status(const rdmaio_rc_t* rc);

/*!
 * @brief Binds a remote memory region to the RC queue pair for remote operations.
 * @param rc The RC queue pair object.
 * @param mr Pointer to the attributes of the remote memory region.
 */
void rdmaio_rc_bind_remote_mr(rdmaio_rc_t* rc, const rdmaio_regattr_t* mr);

/*!
 * @brief Binds a local memory region to the RC queue pair for local operations.
 * @param rc The RC queue pair object.
 * @param mr Pointer to the attributes of the local memory region.
 */
void rdmaio_rc_bind_local_mr(rdmaio_rc_t* rc, const rdmaio_regattr_t* mr);

/*!
 * @brief Connects the local RC queue pair to a remote queue pair based on its attributes.
 * @param rc The local RC queue pair object.
 * @param attr Pointer to the attributes of the remote queue pair.
 * @param out_error_msg Buffer to store an error message in case of failure. Can be NULL.
 * @param error_msg_size Size of the error message buffer.
 * @return 0 on success, -1 on failure.
 */
int rdmaio_rc_connect(rdmaio_rc_t* rc, const rdmaio_qpattr_t* attr, char* out_error_msg, size_t error_msg_size);

// --- Nested Structs for send_normal ---

/*!
 * @brief Describes a send request.
 */
typedef struct rdmaio_reqdesc_t {
    /*! @brief The operation code (e.g., IBV_WR_RDMA_READ). */
    uint32_t op;
    /*! @brief Send flags (e.g., IBV_SEND_SIGNALED). */
    int flags;
    /*! @brief Length of the data to send. */
    uint32_t len;
    /*! @brief User-defined work request ID. */
    uint64_t wr_id;
} rdmaio_reqdesc_t;

/*!
 * @brief Contains the payload information for a send request.
 */
typedef struct rdmaio_reqpayload_t {
    /*! @brief Local memory address of the data to send. */
    uintptr_t local_addr;
    /*! @brief Remote memory address for RDMA operations. */
    uint64_t remote_addr;
    /*! @brief Immediate data to be sent with the request. */
    uint64_t imm_data;
} rdmaio_reqpayload_t;

/*!
 * @brief Posts a send request to the RC queue pair using the default bound memory regions.
 * @param rc The RC queue pair object.
 * @param desc Description of the send request.
 * @param payload Payload information for the send request.
 * @param out_error_msg Buffer to store an error message in case of failure. Can be NULL.
 * @param error_msg_size Size of the error message buffer.
 * @return 0 on success, -1 on failure.
 */
int rdmaio_rc_send_normal(rdmaio_rc_t* rc, const rdmaio_reqdesc_t* desc, const rdmaio_reqpayload_t* payload, char* out_error_msg, size_t error_msg_size);

/*!
 * @brief Posts a send request to the RC queue pair with explicitly specified memory regions.
 * @param rc The RC queue pair object.
 * @param desc Description of the send request.
 * @param payload Payload information for the send request.
 * @param local_mr Attributes of the local memory region to use for this request.
 * @param remote_mr Attributes of the remote memory region to use for this request (for RDMA operations).
 * @param out_error_msg Buffer to store an error message in case of failure. Can be NULL.
 * @param error_msg_size Size of the error message buffer.
 * @return 0 on success, -1 on failure.
 */
int rdmaio_rc_send_normal_mr(rdmaio_rc_t* rc, const rdmaio_reqdesc_t* desc, const rdmaio_reqpayload_t* payload,
                             const rdmaio_regattr_t* local_mr, const rdmaio_regattr_t* remote_mr,
                             char* out_error_msg, size_t error_msg_size);

/*!
 * @brief Polls for a completion from the send completion queue of the RC queue pair.
 * @param rc The RC queue pair object.
 * @param out_user_wr Pointer to store the user-defined work request ID of the completed request. Can be NULL.
 * @param out_wc Pointer to a structure where the work completion details will be stored. Can be NULL.
 * @return 0 if a completion was found, -1 otherwise.
 */
int rdmaio_rc_poll_rc_comp(rdmaio_rc_t* rc, uint64_t* out_user_wr, struct ibv_wc* out_wc);

/*!
 * @brief Waits for a completion from the send completion queue of the RC queue pair with a specified timeout.
 * @param rc The RC queue pair object.
 * @param timeout The maximum time to wait for a completion, in seconds. Use a negative value for no timeout.
 * @param out_user_wr Pointer to store the user-defined work request ID of the completed request. Can be NULL.
 * @param out_wc Pointer to a structure where the work completion details will be stored. Can be NULL.
 * @return 0 if a completion was found, -1 on error, 1 on timeout.
 */
int rdmaio_rc_wait_rc_comp(rdmaio_rc_t* rc, uint64_t* out_user_wr, struct ibv_wc* out_wc);

/*!
 * @brief Gets the maximum send size supported by the RC queue pair.
 * @param rc The RC queue pair object.
 * @return The maximum send size in bytes, or -1 on error.
 */
int rdmaio_rc_max_send_sz(const rdmaio_rc_t* rc);

/*!
 * @brief Destroys the rdmaio_rc_t instance.
 * @param rc_ptr the RC queue pair object
 */
void rdmaio_rc_destroy(rdmaio_rc_t* rc_ptr);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // RDMAIO_RC_C_H