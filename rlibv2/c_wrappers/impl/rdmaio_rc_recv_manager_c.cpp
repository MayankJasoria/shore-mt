#include "../rdmaio_rc_recv_manager_c.h"
#include "../../core/qps/rc_recv_manager.hh"
#include "../../core/rctrl.hh"
#include "../../core/rmem/mem.hh"
#include <string>
#include <memory>

using namespace rdmaio;
using namespace rdmaio::qp;
using namespace rdmaio::rmem;

// Define a constexpr for the entry number
constexpr usize entry_num = 2048;

// Define a default value for the template parameter R
using DefaultRecvManager = RecvManager<entry_num>;

// Concrete implementation of AbsRecvAllocator
class SimpleAllocator : public AbsRecvAllocator {
    RMem::raw_ptr_t buf = nullptr;
    usize total_mem = 0;
    mr_key_t key;

public:
    virtual ~SimpleAllocator() = default;
    SimpleAllocator(const Arc<RMem>& mem, mr_key_t key)
      : buf(mem->raw_ptr), total_mem(mem->sz), key(key) {
       RDMA_LOG(4) << "simple allocator use key: " << key;
    }

    Option<std::pair<rmem::RMem::raw_ptr_t, rmem::mr_key_t>> alloc_one(
      const usize &sz) override {
       if (total_mem < sz) {
          return {};
       }
       auto ret = buf;
       buf = static_cast<char *>(buf) + sz;
       total_mem -= sz;
       return std::make_pair(ret, key);
    }

    Option<std::pair<rmem::RMem::raw_ptr_t, rmem::RegAttr>> alloc_one_for_remote(
       const usize &sz) override {
       return {};
    }
};

extern "C" {

rdmaio_recv_manager_t* recv_manager_create(rdmaio_rctrl_t* ctrl_ptr) {
    if (ctrl_ptr && ctrl_ptr->ctrl) {
        RCtrl* ctrl = static_cast<RCtrl*>(ctrl_ptr->ctrl);
        DefaultRecvManager* manager = new DefaultRecvManager(*ctrl);
        rdmaio_recv_manager_t* wrapper = new rdmaio_recv_manager_t;
        wrapper->manager = manager;
        return wrapper;
    }
    return nullptr;
}

simple_allocator_t* simple_allocator_create(rdmaio_rmem_t* mem_ptr, uint32_t key) {
    if (mem_ptr && mem_ptr->mem) {
        RMem* mem = static_cast<RMem*>(mem_ptr->mem);
        // Lambda fix: Using a lambda as a no-op deleter for Arc
        SimpleAllocator* allocator = new SimpleAllocator(Arc<RMem>(mem, [](RMem*){ /* Do not delete, managed elsewhere */ }), key);
        simple_allocator_t* wrapper = new simple_allocator_t;
        wrapper->allocator = allocator;
        return wrapper;
    }
    return nullptr;
}

void simple_allocator_destroy(simple_allocator_t* allocator_ptr) {
    if (allocator_ptr && allocator_ptr->allocator) {
        delete static_cast<SimpleAllocator*>(allocator_ptr->allocator);
        delete allocator_ptr;
    }
}

bool recv_manager_reg_recv_cq(rdmaio_recv_manager_t* manager_ptr, const char* name, struct ibv_cq* cq, simple_allocator_t* allocator_ptr) {
    if (manager_ptr && manager_ptr->manager && name && cq && allocator_ptr && allocator_ptr->allocator) {
        DefaultRecvManager* manager = static_cast<DefaultRecvManager*>(manager_ptr->manager);
        SimpleAllocator* allocator = static_cast<SimpleAllocator*>(allocator_ptr->allocator);
        // Lambda fix: Using a lambda as a no-op deleter for Arc
        auto recv_common = std::make_shared<RecvCommon>(cq, Arc<SimpleAllocator>(allocator, [](SimpleAllocator*){ /* Do not delete, managed elsewhere */ }));
        return manager->reg_recv_cqs.reg(name, recv_common).has_value();
    }
    return false;
}

recv_entries_handle_t* recv_manager_query_recv_entries(rdmaio_recv_manager_t* manager_ptr, const char* name) {
    if (manager_ptr && manager_ptr->manager && name) {
       DefaultRecvManager* manager = static_cast<DefaultRecvManager*>(manager_ptr->manager);
       auto recv_entries_opt = manager->reg_recv_entries.query(name);
       if (recv_entries_opt.has_value()) {
          // Using the constexpr variable 'entry_num'
          auto arc_ptr = new Arc<RecvEntries<entry_num>>(recv_entries_opt.value());
          recv_entries_handle_t* handle = new recv_entries_handle_t;
          handle->internal_ptr = static_cast<void*>(arc_ptr);
          return handle;
       }
    }
    return nullptr;
}

void recv_manager_destroy(rdmaio_recv_manager_t* manager_ptr) {
    if (manager_ptr && manager_ptr->manager) {
       delete static_cast<DefaultRecvManager*>(manager_ptr->manager);
       delete manager_ptr;
    }
}

void recv_entries_handle_destroy(recv_entries_handle_t* handle) {
    if (handle && handle->internal_ptr) {
       // Using the constexpr variable 'entry_num'
       delete static_cast<Arc<RecvEntries<entry_num>>*>(handle->internal_ptr);
       delete handle;
    } else if (handle) {
       delete handle;
    }
}


} // extern "C"