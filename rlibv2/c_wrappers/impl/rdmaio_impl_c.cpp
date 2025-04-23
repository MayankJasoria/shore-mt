#include "../rdmaio_impl_c.h"
#include "../../core/lib.hh"
#include <infiniband/verbs.h> // Include for ibv_verbs
#include <cstring>

using namespace rdmaio;
using namespace rdmaio::qp;

extern "C" {

struct ibv_cq* rdmaio_create_cq(rdmaio_nic_t* nic_ptr, int cq_sz, char* err_msg, size_t err_msg_size) {
	if (!nic_ptr || !nic_ptr->nic) {
		return nullptr;
	}
	auto arc_nic_ptr = static_cast<Arc<RNic>*>(nic_ptr->nic);
	auto result = Impl::create_cq(*arc_nic_ptr, cq_sz);
	if (result.code.c == IOCode::Ok) {
		return result.desc.first;
	}
	if (err_msg && err_msg_size > 0) {
		strncpy(err_msg, result.desc.second.c_str(), err_msg_size - 1);
		err_msg[err_msg_size - 1] = '\0';
	}
	return nullptr;
}

void rdmaio_destroy_cq(struct ibv_cq* cq) {
	if (cq) {
		ibv_destroy_cq(cq);
	}
}

} // extern "C"