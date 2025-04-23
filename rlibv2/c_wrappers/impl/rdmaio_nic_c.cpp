#include "rdmaio_nic_c.h"
#include "../../core/nic.hh"
#include "../../core/nicinfo.hh"
#include "../../core/common.hh"
#include <vector>
#include <cstring>
#include <optional>

using namespace rdmaio;

extern "C" {

rdmaio_devidx_t* rnic_info_query_dev_names(size_t* count) {
    std::vector<DevIdx> dev_idxs = RNicInfo::query_dev_names();
    *count = dev_idxs.size();
    if (dev_idxs.empty()) {
        return nullptr;
    }
    rdmaio_devidx_t* c_dev_idxs = static_cast<rdmaio_devidx_t*>(malloc(sizeof(rdmaio_devidx_t) * dev_idxs.size()));
    if (!c_dev_idxs) {
        return nullptr;
    }
    for (size_t i = 0; i < dev_idxs.size(); ++i) {
        c_dev_idxs[i].dev_id = dev_idxs[i].dev_id;
        c_dev_idxs[i].port_id = dev_idxs[i].port_id;
    }
    return c_dev_idxs;
}

void rnic_info_free_dev_names(rdmaio_devidx_t* dev_idxs) {
    free(dev_idxs);
}

rdmaio_nic_t* rnic_create(rdmaio_devidx_t idx, uint8_t gid) {
    auto nic_option = RNic::create({.dev_id = static_cast<unsigned int>(idx.dev_id),
                              .port_id = static_cast<unsigned int>(idx.port_id)}, gid);
    if (nic_option.has_value()) {
       Arc<RNic>* arc_nic = new Arc<RNic>(nic_option.value());
       rdmaio_nic_t* nic_wrapper = static_cast<rdmaio_nic_t*>(malloc(sizeof(rdmaio_nic_t)));
       if (nic_wrapper) {
          nic_wrapper->nic = static_cast<void*>(arc_nic);
          return nic_wrapper;
       } else {
          delete arc_nic; // Clean up if wrapper allocation fails
       }
    }
    return nullptr;
}

bool rnic_valid(const rdmaio_nic_t* nic_ptr) {
    if (nic_ptr && nic_ptr->nic) {
        return (*static_cast<Arc<RNic>*>(nic_ptr->nic))->valid();
    }
    return false;
}

void* rnic_get_ctx(const rdmaio_nic_t* nic_ptr) {
    if (nic_ptr && nic_ptr->nic) {
        return (*static_cast<Arc<RNic>*>(nic_ptr->nic))->get_ctx();
    }
    return nullptr;
}

void* rnic_get_pd(const rdmaio_nic_t* nic_ptr) {
    if (nic_ptr && nic_ptr->nic) {
        return (*static_cast<Arc<RNic>*>(nic_ptr->nic))->get_pd();
    }
    return nullptr;
}

rdmaio_iocode_t rnic_is_active(const rdmaio_nic_t* nic_ptr, char* err_msg, size_t err_msg_size) {
    if (nic_ptr && nic_ptr->nic) {
        auto result = (*static_cast<Arc<RNic>*>(nic_ptr->nic))->is_active();
        if (result.code.c == IOCode::Ok) {
            return RDMAIO_OK;
        } else {
            if (err_msg && err_msg_size > 0) {
                strncpy(err_msg, result.desc.c_str(), err_msg_size - 1);
                err_msg[err_msg_size - 1] = '\0';
            }
            return RDMAIO_ERR;
        }
    }
    return RDMAIO_ERR;
}

void rnic_destroy(rdmaio_nic_t* nic_ptr) {
    if (nic_ptr && nic_ptr->nic) {
        delete static_cast<Arc<RNic>*>(nic_ptr->nic);
        free(nic_ptr);
    } else if (nic_ptr) {
        free(nic_ptr);
    }
}

} // extern "C"