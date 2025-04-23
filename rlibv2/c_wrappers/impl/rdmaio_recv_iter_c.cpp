#include "../rdmaio_recv_iter_c.h"
#include "../../core/qps/recv_iter.hh"
#include "../../core/qps/rc.hh"
#include "../../core/qps/recv_helper.hh"
#include <memory> // Include for std::unique_ptr
#include "../../core/qps/mod.hh" // Include for Dummy

using namespace rdmaio;
using namespace rdmaio::qp;

constexpr size_t entry_num = 2048;

// Internal structure to hold the C++ RecvIter instance
struct rdmaio_recv_iter_internal_t {
    std::unique_ptr<RecvIter<Dummy, entry_num>> iter; // Changed RC to Dummy
};

extern "C" {

rdmaio_recv_iter_t* rdmaio_recv_iter_create(rdmaio_qp_t* qp_ptr, recv_entries_handle_t* recv_entries_handle) {
    if (!qp_ptr || !recv_entries_handle || !qp_ptr->internal || !recv_entries_handle->internal_ptr) return nullptr;

    // rdmaio_qp_t::internal holds an Arc<Dummy>*
    auto dummy_arc_ptr = static_cast<Arc<Dummy>*>(qp_ptr->internal);
    if (!dummy_arc_ptr) return nullptr;
    Arc<Dummy>& qp_arc = *dummy_arc_ptr;

    // recv_entries_handle->internal_ptr holds an Arc<RecvEntries<entry_num>>*
    auto entries_arc_ptr = static_cast<Arc<RecvEntries<entry_num>>*>(recv_entries_handle->internal_ptr);
    if (!entries_arc_ptr) return nullptr;
    Arc<RecvEntries<entry_num>>& entries_arc = *entries_arc_ptr;

    rdmaio_recv_iter_t* iter_c = new rdmaio_recv_iter_t;
    iter_c->internal = new rdmaio_recv_iter_internal_t;
    auto internal_ptr = static_cast<rdmaio_recv_iter_internal_t*>(iter_c->internal);
    internal_ptr->iter.reset(new RecvIter<Dummy, entry_num>(qp_arc, entries_arc));

    return iter_c;
}

bool rdmaio_recv_iter_has_msgs(const rdmaio_recv_iter_t* iter) {
    if (!iter || !iter->internal) {
        return false;
    }
    auto internal_ptr = static_cast<rdmaio_recv_iter_internal_t*>(iter->internal);
    if (!internal_ptr || !internal_ptr->iter) {
        return false;
    }
    return internal_ptr->iter->has_msgs();
}

void rdmaio_recv_iter_next(rdmaio_recv_iter_t* iter) {
    if (iter && iter->internal) {
        auto internal_ptr = static_cast<rdmaio_recv_iter_internal_t*>(iter->internal);
        if (internal_ptr && internal_ptr->iter) {
            internal_ptr->iter->next();
        }
    }
}

bool rdmaio_recv_iter_cur_msg(const rdmaio_recv_iter_t* iter, uint32_t* out_imm_data, uintptr_t* out_buf_addr) {
    if (!iter || !iter->internal) return false;
    auto internal_ptr = static_cast<rdmaio_recv_iter_internal_t*>(iter->internal);
    if (!internal_ptr || !internal_ptr->iter) return false;
    auto msg_opt = internal_ptr->iter->cur_msg();
    if (msg_opt.has_value()) {
        auto msg = msg_opt.value();
        if (out_imm_data) {
            *out_imm_data = msg.first;
        }
        if (out_buf_addr) {
            *out_buf_addr = reinterpret_cast<uintptr_t>(msg.second);
        }
        return true;
    }
    return false;
}

void rdmaio_recv_iter_clear(rdmaio_recv_iter_t* iter) {
    if (iter && iter->internal) {
        auto internal_ptr = static_cast<rdmaio_recv_iter_internal_t*>(iter->internal);
        if (internal_ptr && internal_ptr->iter) {
            internal_ptr->iter->clear();
        }
    }
}

void rdmaio_recv_iter_destroy(rdmaio_recv_iter_t* iter) {
    if (iter && iter->internal) {
        delete static_cast<rdmaio_recv_iter_internal_t*>(iter->internal);
        delete iter;
    }
}

} // extern "C"