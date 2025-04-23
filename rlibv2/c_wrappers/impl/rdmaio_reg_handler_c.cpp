#include "../rdmaio_reg_handler_c.h"
#include "../../core/rmem/handler.hh"
#include <memory>

using namespace rdmaio;
using namespace rdmaio::rmem;

extern "C" {

    rdmaio_reg_handler_t* rdmaio_reg_handler_create(rdmaio_rmem_t* mem_ptr, rdmaio_nic_t* nic_ptr) {
       if (mem_ptr && nic_ptr && mem_ptr->mem) {
          auto mem_arc = Arc<RMem>(static_cast<RMem*>(mem_ptr->mem), [](RMem*){ /* Do not delete */ });
          auto nic_arc_ptr = static_cast<Arc<RNic>*>(nic_ptr->nic);
          if (nic_arc_ptr) {
             auto handler_res = RegHandler::create(mem_arc, *nic_arc_ptr);
             if (handler_res.has_value()) {
                return new rdmaio_reg_handler_t{new Arc<RegHandler>(handler_res.value())};
             }
          }
       }
       return nullptr;
    }

    bool rdmaio_reg_handler_valid(rdmaio_reg_handler_t* handler_ptr) {
       if (handler_ptr && handler_ptr->handler) {
          auto handler_arc = static_cast<Arc<RegHandler>*>(handler_ptr->handler);
          return (*handler_arc)->valid();
       }
       return false;
    }

	rdmaio_regattr_t rdmaio_reg_handler_get_attr(rdmaio_reg_handler_t* handler_ptr) {
    	rdmaio_regattr_t attr = {};
    	if (handler_ptr && handler_ptr->handler) {
    		auto handler_arc = static_cast<Arc<RegHandler>*>(handler_ptr->handler);
    		auto reg_attr_opt = (*handler_arc)->get_reg_attr();
    		if (reg_attr_opt.has_value()) {
    			auto reg_attr = reg_attr_opt.value();
    			attr.addr = reg_attr.buf;
    			attr.length = reg_attr.sz;
    			attr.lkey = reg_attr.lkey;
    			attr.rkey = reg_attr.key;
    		}
    	}
    	return attr;
    }

    void rdmaio_reg_handler_destroy(rdmaio_reg_handler_t* handler_ptr) {
       if (handler_ptr && handler_ptr->handler) {
          auto handler_arc = static_cast<Arc<RegHandler>*>(handler_ptr->handler);
          delete handler_arc;
       }
       delete handler_ptr;
    }

} // extern "C"