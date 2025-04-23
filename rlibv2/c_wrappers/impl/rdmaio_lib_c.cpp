#include "../rdmaio_lib_c.h"
#include "../../core/lib.hh"
#include "../../core/qps/rc.hh" // Include RC definition
#include "../../core/qps/config.hh" // Include QPConfig definition
#include "../../core/rmem/handler.hh" // Include RegAttr definition
#include "../../core/common.hh" // Include for Arc

using namespace rdmaio;
using namespace rdmaio::qp;

struct rdmaio_connect_manager_t {
	rdmaio::ConnectManager* cm;
};

extern "C" {

rdmaio_connect_manager_t* rdmaio_connect_manager_create(const char* addr) {
    if (!addr) return nullptr;
    return new rdmaio_connect_manager_t{new ConnectManager(addr)};
}

rdmaio_iocode_t rdmaio_connect_manager_wait_ready(rdmaio_connect_manager_t* cm, double timeout_sec, size_t retry) {
    if (!cm || !cm->cm) return RDMAIO_ERR;
    Result<std::string> result = static_cast<ConnectManager*>(cm->cm)->wait_ready(timeout_sec * 1000000, retry);
    return static_cast<rdmaio_iocode_t>(result.code.c);
}

rdmaio_iocode_t rdmaio_connect_manager_cc_rc_msg(rdmaio_connect_manager_t* cm,
                                                        const char* qp_name,
                                                        const char* channel_name,
                                                        size_t max_msg_size,
                                                        rdmaio_rc_t* rc_wrapper,
                                                        uint32_t remote_nic_id,
                                                        const rdmaio_qpconfig_t* config) {
	if (!cm || !cm->cm || !rc_wrapper || !qp_name || !channel_name) {
		return RDMAIO_ERR;
	}

	if (!rc_wrapper->rc) {
		return RDMAIO_ERR;
	}

	Arc<RC>* rc_arc = static_cast<Arc<RC>*>(rc_wrapper->rc);
	Arc<RC> rc_to_pass;
	if (rc_arc) {
		rc_to_pass = *rc_arc; // dereference to get a copy of the Arc<RC> instance
	} else {
		return RDMAIO_ERR;
	}

    QPConfig qp_config = QPConfig();
    if (config) {
        qp_config.set_access_flags(config->access_flags)
                 .set_max_rd_ops(config->max_rd_atomic)
                 .set_psn(config->rq_psn) // Assuming rq_psn is used for initial PSN
                 .set_timeout(config->timeout)
                 .set_max_send(config->max_send_size)
                 .set_max_recv(config->max_recv_size)
                 .set_qkey(config->qkey)
                 .set_dc_key(config->dc_key);
    }

	auto result = static_cast<ConnectManager*>(cm->cm)->cc_rc_msg(qp_name, channel_name, max_msg_size, rc_to_pass, remote_nic_id, qp_config);
    return static_cast<rdmaio_iocode_t>(result.code.c);
}

rdmaio_iocode_t rdmaio_connect_manager_fetch_remote_mr(rdmaio_connect_manager_t* cm,
															   uint64_t id,
															   rdmaio_regattr_t* out_attr) {
	if (!cm || !cm->cm || !out_attr) return RDMAIO_ERR;
	auto result = static_cast<ConnectManager*>(cm->cm)->fetch_remote_mr(id);
	if (result.code.c == IOCode::Ok) {
		rmem::RegAttr attr = std::get<1>(result.desc);
		out_attr->addr = attr.buf; // Assuming attr.buf corresponds to the address
		out_attr->length = attr.sz; // Assuming attr.sz corresponds to the length
		out_attr->lkey = attr.lkey;
		out_attr->rkey = attr.key; // Assuming attr.key corresponds to the remote key
		return RDMAIO_OK;
	} else {
		return static_cast<rdmaio_iocode_t>(result.code.c);
	}
}

void rdmaio_connect_manager_destroy(rdmaio_connect_manager_t* cm) {
    if (cm) {
        delete static_cast<ConnectManager*>(cm->cm);
        delete cm;
    }
}

} // extern "C"