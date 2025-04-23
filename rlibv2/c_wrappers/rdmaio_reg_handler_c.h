#ifndef RDMAIO_REG_HANDLER_C_H
#define RDMAIO_REG_HANDLER_C_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "rdmaio_mem_c.h" // Include for rdmaio_rmem_t
#include "rdmaio_nic_c.h"
#include "rdmaio_regattr_c.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @brief Opaque type representing a memory registration handler.
 */
typedef struct rdmaio_reg_handler_t {
  void* handler;
} rdmaio_reg_handler_t;

/*!
 * @brief Creates a memory registration handler.
 * @param mem A pointer to the registered memory object.
 * @param nic A pointer to the RDMA NIC.
 * @return A pointer to the newly created registration handler, or NULL if creation fails.
 */
rdmaio_reg_handler_t* rdmaio_reg_handler_create(rdmaio_rmem_t* mem, rdmaio_nic_t* nic);

/*!
 * @brief Checks if the registration handler is valid.
 * @param handler A pointer to the registration handler.
 * @return True if the handler is valid, false otherwise.
 */
bool rdmaio_reg_handler_valid(rdmaio_reg_handler_t* handler);

/*!
 * @brief Gets the registration attributes of the memory region.
 * @param handler A pointer to the registration handler.
 * @return The registration attributes.
 */
rdmaio_regattr_t rdmaio_reg_handler_get_attr(rdmaio_reg_handler_t* handler);

/*!
 * @brief Destroys the registration handler.
 * @param handler A pointer to the registration handler.
 */
void rdmaio_reg_handler_destroy(rdmaio_reg_handler_t* handler);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // RDMAIO_REG_HANDLER_C_H