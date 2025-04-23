#ifndef RDMAIO_QPATTR_C_H
#define RDMAIO_QPATTR_C_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rdmaio_qpattr_t {
	uint64_t subnet_prefix;
	uint64_t interface_id;
	uint32_t local_id;
	uint64_t lid;
	uint64_t psn;
	uint64_t port_id;
	uint64_t qpn;
	uint64_t qkey;
} rdmaio_qpattr_t;

#ifdef __cplusplus
} // extern "C"
#endif

#endif // RDMAIO_QPATTR_C_H