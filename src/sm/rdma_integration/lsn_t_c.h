// lsn_t_c.h
#ifndef LSN_T_C_H
#define LSN_T_C_H

#ifdef __cplusplus
// Allows this header to be included in both C and C++ files
extern "C" {
#endif

#include <stdint.h> // For uint32_t, int64_t
#include <stdbool.h>

  // Define an opaque type for the C++ lsn_t class.
  // C code will only see pointers to this type.
  // This is how you represent an 'lsn_t*' pointer in C code.
  typedef struct lsn_t_opaque lsn_t_c;

  // --- C Functions for LSN Interop (Implemented in lsn_t_c.cpp) ---

  // Get the partition number component from an opaque lsn_t pointer.
  // Called from C code that has an lsn_t_c* handle.
  uint32_t lsn_get_partition_c(const lsn_t_c* lsn_ptr);

  // Get the offset component from an opaque lsn_t pointer.
  // Called from C code that has an lsn_t_c* handle.
  int64_t lsn_get_offset_c(const lsn_t_c* lsn_ptr);

  // Compare an lsn_t handle with LSN components (partition, offset).
  // Returns true if the LSN represented by lsn_ptr is >= the LSN represented by components.
  bool lsn_is_greater_equal_c(const lsn_t_c* lsn_ptr, uint32_t part_to_compare, int64_t offset_to_compare);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // LSN_T_C_H