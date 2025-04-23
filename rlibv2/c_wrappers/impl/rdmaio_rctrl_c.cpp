// rlibv2/c_wrappers/rdmaio_rctrl_wrapper.cpp
#include "../rdmaio_rctrl_c.h"
#include "../../core/rctrl.hh"
#include "../../core/rmem/handler.hh"
#include "../../core/nic.hh"
#include "../../core/qps/mod.hh"
#include <string>
#include <memory>
#include <cstdio> // For fprintf
#include <optional> // Include for std::optional

using namespace rdmaio;
using namespace rdmaio::rmem;
using namespace rdmaio::qp;

extern "C" {

rdmaio_rctrl_t* rctrl_create(unsigned short port, const char* host) {
    std::string host_str = (host != nullptr) ? host : "localhost";
    RCtrl* rctrl_obj = new RCtrl(static_cast<usize>(port), host_str);
    rdmaio_rctrl_t* wrapper = new rdmaio_rctrl_t;
    wrapper->ctrl = rctrl_obj;
    return wrapper;
}

bool rctrl_start_daemon(rdmaio_rctrl_t* ctrl_ptr) {
    if (ctrl_ptr && ctrl_ptr->ctrl) {
        RCtrl* ctrl = static_cast<RCtrl*>(ctrl_ptr->ctrl);
        return ctrl->start_daemon();
    }
    return false;
}

void rctrl_stop_daemon(rdmaio_rctrl_t* ctrl_ptr) {
    if (ctrl_ptr && ctrl_ptr->ctrl) {
        RCtrl* ctrl = static_cast<RCtrl*>(ctrl_ptr->ctrl);
        ctrl->stop_daemon();
    }
}

bool rdmaio_rctrl_register_mr(rdmaio_rctrl_t* ctrl_ptr, int64_t key, rdmaio_reg_handler_t* handler_ptr) {
	if (ctrl_ptr && ctrl_ptr->ctrl && handler_ptr && handler_ptr->handler) {
		RCtrl* ctrl = static_cast<RCtrl*>(ctrl_ptr->ctrl);
		Arc<RegHandler>* handler_arc_ptr = static_cast<Arc<RegHandler>*>(handler_ptr->handler);
		if (handler_arc_ptr) {
			ctrl->registered_mrs.reg(static_cast<u64>(key), *handler_arc_ptr);
			return true;
		}
	}
	fprintf(stderr, "Error: Invalid arguments for rdmaio_rctrl_register_mr.\n");
	return false;
}

rdmaio_qp_t* rctrl_query_qp(rdmaio_rctrl_t* ctrl_ptr, const char* qp_name) {
	if (ctrl_ptr && ctrl_ptr->ctrl && qp_name) {
		RCtrl* ctrl = static_cast<RCtrl*>(ctrl_ptr->ctrl);
		auto qp_opt = ctrl->registered_qps.query(qp_name);
		if (qp_opt.has_value()) {
			// Wrap the Arc<Dummy> object
			rdmaio_qp_t* qp_wrapper = new rdmaio_qp_t;
			qp_wrapper->internal = new Arc<Dummy>(qp_opt.value()); // Store Arc<Dummy>
			return qp_wrapper;
		}
	}
	return nullptr;
}

bool rctrl_register_nic(rdmaio_rctrl_t* ctrl_ptr, uint64_t nic_id, rdmaio_nic_t* nic_ptr) {
    if (ctrl_ptr && ctrl_ptr->ctrl && nic_ptr && nic_ptr->nic) {
       RCtrl* ctrl = static_cast<RCtrl*>(ctrl_ptr->ctrl);
       Arc<RNic>* arc_nic_ptr = static_cast<Arc<RNic>*>(nic_ptr->nic);
       ctrl->opened_nics.reg(nic_id, *arc_nic_ptr);
       return true;
    }
    return false;
}

void rctrl_destroy(rdmaio_rctrl_t* ctrl_ptr) {
    if (ctrl_ptr) {
        RCtrl* ctrl = static_cast<RCtrl*>(ctrl_ptr->ctrl);
        delete ctrl;
        delete ctrl_ptr;
    }
}

} // extern "C"