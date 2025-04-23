#include "../rdmaio_mem_c.h"
#include "../../core/rmem/mem.hh"

using namespace rdmaio::rmem;

extern "C" {

rdmaio_rmem_t* rmem_create(uint64_t size) {
	return new rdmaio_rmem_t{new RMem(size)};
}

bool rmem_valid(rdmaio_rmem_t* mem_ptr) {
	if (mem_ptr && mem_ptr->mem) {
		return static_cast<RMem*>(mem_ptr->mem)->valid();
	}
	return false;
}

void rmem_destroy(rdmaio_rmem_t* mem_ptr) {
	if (mem_ptr && mem_ptr->mem) {
		delete static_cast<RMem*>(mem_ptr->mem);
		delete mem_ptr;
	} else if (mem_ptr) {
		delete mem_ptr;
	}
}

} // extern "C"