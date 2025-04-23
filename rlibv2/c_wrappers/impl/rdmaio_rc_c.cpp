#include "../rdmaio_rc_c.h"
#include "../rdmaio_nic_c.h" // Include for rdmaio_nic_t
#include "../../core/qps/rc.hh"
#include "../../core/rmem/handler.hh"
#include "../../core/result.hh"
#include "../../core/qps/mod.hh"
#include "../../core/qps/config.hh"
#include "../../core/common.hh" // Include for Arc

#include <optional>
#include <cstring>
#include <errno.h>
#include <new> // For std::bad_alloc

using namespace rdmaio;
using namespace rdmaio::qp;
using namespace rdmaio::rmem;

extern "C" {

rdmaio_rc_t* rdmaio_rc_create(rdmaio_nic_t* nic, const rdmaio_qpconfig_t* config, struct ibv_cq* recv_cq) {
    if (!nic || !config) return nullptr; // Ensure config is not NULL
    QPConfig qp_config = QPConfig();

    qp_config.set_access_flags(config->access_flags);
    qp_config.set_max_rd_ops(config->max_rd_atomic); // Assuming max_rd_atomic in C wrapper maps to this
    qp_config.set_psn(config->rq_psn); // Assuming rq_psn and sq_psn are the same for initial setup
    qp_config.set_timeout(config->timeout);
    qp_config.set_max_send(config->max_send_size);
    qp_config.set_max_recv(config->max_recv_size);
    qp_config.set_qkey(config->qkey);
    qp_config.set_dc_key(config->dc_key);

    Arc<RNic>* arc_nic_ptr = static_cast<Arc<RNic>*>(nic->nic);
    if (arc_nic_ptr) {
        Arc<RNic> arc_nic = *arc_nic_ptr; // Dereference to get the Arc<RNic> object
        auto rc_option = RC::create(arc_nic, qp_config, recv_cq);
        if (rc_option.has_value()) {
            // Allocate Arc<RC> on the heap
            Arc<RC>* arc_rc = new Arc<RC>(rc_option.value());
            rdmaio_rc_t* wrapper = new rdmaio_rc_t;
            wrapper->rc = static_cast<void*>(arc_rc); // Store the pointer to Arc<RC>
            return wrapper;
        }
    }
    return nullptr;
}

void rdmaio_rc_destroy(rdmaio_rc_t* rc_ptr) {
    if (rc_ptr) {
        if (rc_ptr->rc) {
            Arc<RC>* arc_rc = static_cast<Arc<RC>*>(rc_ptr->rc);
            delete arc_rc;
        }
        delete rc_ptr;
    }
}

void rdmaio_rc_my_attr(const rdmaio_rc_t* rc, rdmaio_qpattr_t* out_attr) {
    if (rc && out_attr && rc->rc) {
        Arc<RC>* arc_rc_ptr = static_cast<Arc<RC>*>(rc->rc);
        QPAttr qp_attr = (*arc_rc_ptr)->my_attr();
        out_attr->subnet_prefix = qp_attr.addr.subnet_prefix;
        out_attr->interface_id = qp_attr.addr.interface_id;
        out_attr->local_id = qp_attr.addr.local_id;
        out_attr->lid = qp_attr.lid;
        out_attr->psn = qp_attr.psn;
        out_attr->port_id = qp_attr.port_id;
        out_attr->qpn = qp_attr.qpn;
        out_attr->qkey = qp_attr.qkey;
    }
}

int rdmaio_rc_connect(rdmaio_rc_t* rc, const rdmaio_qpattr_t* attr, char* out_error_msg, size_t error_msg_size) {
    if (rc && attr && rc->rc) {
        Arc<RC>* arc_rc_ptr = static_cast<Arc<RC>*>(rc->rc);
        QPAttr qp_attr;
        qp_attr.addr.subnet_prefix = attr->subnet_prefix;
        qp_attr.addr.interface_id = attr->interface_id;
        qp_attr.addr.local_id = attr->local_id;
        qp_attr.lid = attr->lid;
        qp_attr.psn = attr->psn;
        qp_attr.port_id = attr->port_id;
        qp_attr.qpn = attr->qpn;
        qp_attr.qkey = attr->qkey;
        auto result = (*arc_rc_ptr)->connect(qp_attr);
        if (result == IOCode::Ok) {
            return 0; // Success
        } else {
            if (out_error_msg && error_msg_size > 0) {
                strncpy(out_error_msg, result.desc.c_str(), error_msg_size - 1);
            }
            return -1; // Error
        }
    }
    return -1; // Invalid input
}

int rdmaio_rc_send_normal(rdmaio_rc_t* rc, const rdmaio_reqdesc_t* req_desc, const rdmaio_reqpayload_t* req_payload, char* out_error_msg, size_t error_msg_size) {
    if (rc && rc->rc) {
        Arc<RC>* arc_rc_ptr = static_cast<Arc<RC>*>(rc->rc);
        RC::ReqDesc cpp_req_desc;
        if (req_desc) {
            cpp_req_desc.wr_id = req_desc->wr_id;
            cpp_req_desc.op = static_cast<ibv_wr_opcode>(req_desc->op);
            cpp_req_desc.flags = req_desc->flags;
            cpp_req_desc.len = req_desc->len;
        }
        RC::ReqPayload cpp_req_payload;
        if (req_payload) {
            cpp_req_payload.local_addr = reinterpret_cast<RMem::raw_ptr_t>(req_payload->local_addr);
            cpp_req_payload.remote_addr = req_payload->remote_addr;
            cpp_req_payload.imm_data = req_payload->imm_data;
        }
        auto result = (*arc_rc_ptr)->send_normal(cpp_req_desc, cpp_req_payload);
        if (result == IOCode::Ok) {
            return 0; // Success
        } else {
            if (out_error_msg && error_msg_size > 0) {
                strncpy(out_error_msg, result.desc.c_str(), error_msg_size - 1);
            }
            return -1; // Error
        }
    }
    return -1; // Invalid input
}

int rdmaio_rc_send_normal_mr(rdmaio_rc_t* rc, const rdmaio_reqdesc_t* req_desc, const rdmaio_reqpayload_t* req_payload, const rdmaio_regattr_t* reg_local_mr, const rdmaio_regattr_t* reg_remote_mr, char* out_error_msg, size_t error_msg_size) {
    if (rc && rc->rc) {
        Arc<RC>* arc_rc_ptr = static_cast<Arc<RC>*>(rc->rc);
        RC::ReqDesc cpp_req_desc;
        if (req_desc) {
            cpp_req_desc.wr_id = req_desc->wr_id;
            cpp_req_desc.op = static_cast<ibv_wr_opcode>(req_desc->op);
            cpp_req_desc.flags = req_desc->flags;
            cpp_req_desc.len = req_desc->len;
        }
        RC::ReqPayload cpp_req_payload;
        if (req_payload) {
            cpp_req_payload.local_addr = reinterpret_cast<RMem::raw_ptr_t>(req_payload->local_addr);
            cpp_req_payload.remote_addr = req_payload->remote_addr;
            cpp_req_payload.imm_data = req_payload->imm_data;
        }
        RegAttr cpp_reg_local_mr;
        if (reg_local_mr) {
            cpp_reg_local_mr.buf = reg_local_mr->addr;
            cpp_reg_local_mr.sz = reg_local_mr->length;
            cpp_reg_local_mr.lkey = reg_local_mr->lkey;
            cpp_reg_local_mr.key = reg_local_mr->rkey;
        }
        RegAttr cpp_reg_remote_mr;
        if (reg_remote_mr) {
            cpp_reg_remote_mr.buf = reg_remote_mr->addr;
            cpp_reg_remote_mr.sz = reg_remote_mr->length;
            cpp_reg_remote_mr.lkey = reg_remote_mr->lkey;
            cpp_reg_remote_mr.key = reg_remote_mr->rkey;
        }
        auto result = (*arc_rc_ptr)->send_normal(cpp_req_desc, cpp_req_payload, cpp_reg_local_mr, cpp_reg_remote_mr);
        if (result == IOCode::Ok) {
            return 0; // Success
        } else {
            if (out_error_msg && error_msg_size > 0) {
                strncpy(out_error_msg, result.desc.c_str(), error_msg_size - 1);
            }
            return -1; // Error
        }
    }
    return -1; // Invalid input
}

int rdmaio_rc_wait_rc_comp(rdmaio_rc_t* rc, uint64_t* out_user_wr, ibv_wc* out_wc) {
    if (rc && rc->rc) {
        Arc<RC>* arc_rc_ptr = static_cast<Arc<RC>*>(rc->rc);
        auto result = (*arc_rc_ptr)->wait_rc_comp();
        if (result == IOCode::Ok) {
            if (out_wc) {
                std::memcpy(out_wc, &result.desc.second, sizeof(ibv_wc));
                if (out_user_wr) *out_user_wr = result.desc.first;
            }
            return 0; // Success
        } else if (result == IOCode::Timeout) {
            return -2; // Timeout
        } else {
            return -1; // Error
        }
    }
    return -1; // Invalid input
}

int rdmaio_rc_poll_rc_comp(rdmaio_rc_t* rc, uint64_t* out_user_wr, ibv_wc* out_wc) {
    if (rc && rc->rc) {
        Arc<RC>* arc_rc_ptr = static_cast<Arc<RC>*>(rc->rc);
        auto result = (*arc_rc_ptr)->poll_rc_comp();
        if (result.has_value()) {
            if (out_user_wr) *out_user_wr = result.value().first;
            if (out_wc) std::memcpy(out_wc, &result.value().second, sizeof(ibv_wc));
            return 0; // Success
        } else {
            return -2; // No Completion
        }
    }
    return -1; // Invalid input
}

void rdmaio_rc_bind_remote_mr(rdmaio_rc_t* rc_ptr, const rdmaio_regattr_t* mr) {
    if (rc_ptr && rc_ptr->rc && mr) {
        Arc<RC>* arc_rc_ptr = static_cast<Arc<RC>*>(rc_ptr->rc);
        rmem::RegAttr remote_attr = {.buf = mr->addr, .sz = mr->length, .key = mr->rkey, .lkey = mr->lkey};
        (*arc_rc_ptr)->bind_remote_mr(remote_attr);
    }
}

void rdmaio_rc_bind_local_mr(rdmaio_rc_t* rc_ptr, const rdmaio_regattr_t* mr) {
    if (rc_ptr && rc_ptr->rc && mr) {
        Arc<RC>* arc_rc_ptr = static_cast<Arc<RC>*>(rc_ptr->rc);
        rmem::RegAttr local_attr = {.buf = mr->addr, .sz = mr->length, .key = mr->rkey, .lkey = mr->lkey};
        (*arc_rc_ptr)->bind_local_mr(local_attr);
    }
}

} // extern "C"