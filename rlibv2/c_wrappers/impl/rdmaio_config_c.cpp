#include "../rdmaio_config_c.h"
#include <stdlib.h>
#include <infiniband/verbs.h> // Include for IBV_ACCESS flags

// Placeholder for gflags values (assuming they are defined externally)

// Default values from the C++ header
const int kDefaultPSN = 3185;
const int kDefaultQKey = 0x111111;
const int kRcMaxSendSz = 128;
const int kRcMaxRecvSz = 2048;
const int kDcKey = 1024;

extern "C" {

	rdmaio_qpconfig_t* rdmaio_qpconfig_create_default() {
		rdmaio_qpconfig_t* config = (rdmaio_qpconfig_t*)malloc(sizeof(rdmaio_qpconfig_t));
		if (config != NULL) {
			// Initialize with default values from the C++ header
			config->access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC;
			config->max_rd_atomic = 16;
			config->max_dest_rd_atomic = 16;
			config->rq_psn = kDefaultPSN;
			config->sq_psn = kDefaultPSN;
			config->timeout = 20;
			config->max_send_size = kRcMaxSendSz; // Using the C++ default
			config->max_recv_size = kRcMaxRecvSz; // Using the C++ default
			config->qkey = kDefaultQKey;
			config->dc_key = kDcKey;
		}
		return config;
	}

	void rdmaio_qpconfig_destroy(rdmaio_qpconfig_t* config) {
		free(config);
	}

} // extern "C"