#include "lsn_t_c.h" // Includes the C API declarations

// Include core Shore-MT base headers *before* higher-level headers like log_core.h and sm_base.h
#include "sm_int_0.h"
#include "w_defines.h"
#include "w_base.h"
#include "w.h"        // Provides definitions for types like rc_t, w_link_t, w_list_t, etc.
#include "sthread.h" // Provides definitions for types like sthread_t, fileoff_t, lock types, etc.

#include "lsn.h"     // Include the original C++ lsn_t definition
#include "log_core.h"// Assuming log_core is where the durable LSN update logic resides
#include "sm_base.h" // Needed for sm_diskaddr_t, w_base_t::uint4_t, and other base SM types

// Define the opaque struct here. In this C++ file, it's a placeholder that
// corresponds to the opaque type declared in the .h file.
// When a C++ lsn_t* is cast to an lsn_t_c*, they point to the same memory.
// struct lsn_t_opaque { /* No members needed here */ }; // No longer needed if defined in lsn_t_c.h

// Assume access to the global log_core instance (adjust based on your actual structure)
// extern log_core* THE_LOG; // Keep if you use this global

// Implementation of lsn_get_partition_c: Get partition from C++ lsn_t via opaque pointer
uint32_t lsn_get_partition_c(const lsn_t_c* lsn_ptr) {
    // Cast the opaque C pointer back to a pointer to the actual C++ lsn_t
    const lsn_t* actual_lsn_ptr = reinterpret_cast<const lsn_t*>(lsn_ptr);
    // Call the C++ member function and return the result as a C type
    return actual_lsn_ptr->hi(); // Or actual_lsn_ptr->file()
}

// Implementation of lsn_get_offset_c: Get offset from C++ lsn_t via opaque pointer
int64_t lsn_get_offset_c(const lsn_t_c* lsn_ptr) {
     // Cast the opaque C pointer back to a pointer to the actual C++ lsn_t
    const lsn_t* actual_lsn_ptr = reinterpret_cast<const lsn_t*>(lsn_ptr);
    // Call the C++ member function and return the result as a C type
    return actual_lsn_ptr->lo(); // Or actual_lsn_ptr->rba()
}

bool lsn_is_greater_equal_c(const lsn_t_c* lsn_ptr, uint32_t part_to_compare, int64_t offset_to_compare) {
    // Cast the opaque C pointer back to a pointer to the actual C++ lsn_t
    const lsn_t* actual_lsn_ptr = reinterpret_cast<const lsn_t*>(lsn_ptr);

    // Reconstruct a temporary lsn_t object from the components received from the server
    // Note: Cast to the expected types for the lsn_t constructor
    // Ensure sm_diskaddr_t and w_base_t::uint4_t are defined (provided by sm_base.h now)
    lsn_t lsn_to_compare((w_base_t::uint4_t)part_to_compare, (sm_diskaddr_t)offset_to_compare);

    // Use the C++ lsn_t::operator>= for comparison
    return (*actual_lsn_ptr) >= lsn_to_compare;
}