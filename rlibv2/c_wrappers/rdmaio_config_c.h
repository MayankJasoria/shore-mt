#ifndef RDMAIO_CONFIG_C_H
#define RDMAIO_CONFIG_C_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rdmaio_qpconfig_t {
	int access_flags;
	int max_rd_atomic;
	int max_dest_rd_atomic;
	int rq_psn;
	int sq_psn;
	int timeout;
	int max_send_size;
	int max_recv_size;
	int qkey;
	int dc_key;
} rdmaio_qpconfig_t;

/*!
 * @brief Creates a default rdmaio_qpconfig_t object.
 * @return A pointer to the newly created configuration object, or NULL on failure.
 */
rdmaio_qpconfig_t* rdmaio_qpconfig_create_default();

/*!
 * @brief Destroys a rdmaio_qpconfig_t object.
 * @param config The configuration object to destroy.
 */
void rdmaio_qpconfig_destroy(rdmaio_qpconfig_t* config);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // RDMAIO_CONFIG_C_H