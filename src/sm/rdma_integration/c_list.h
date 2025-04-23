#ifndef C_LIST_H
#define C_LIST_H

#include "rdma_structs.h"

#ifdef __cplusplus
extern "C" { // Protect C++ name mangling
#endif

  // Forward declarations of incomplete struct types with c_list naming.
  struct c_list_handle_s;
  struct c_list_iterator_handle_s;

  // Typedefs for the opaque pointer handles using c_list naming.
  typedef struct c_list_handle_s* c_list_handle;
  typedef struct c_list_iterator_handle_s* c_list_iterator_handle;

  // Function declarations using the new handle types
  c_list_handle c_list_create();
  void c_list_destroy(c_list_handle list_handle);
  int c_list_add_back(c_list_handle list_handle, const RdmaDirContents* data);
  c_list_iterator_handle c_list_begin(c_list_handle list_handle);
  c_list_iterator_handle c_list_end(c_list_handle list_handle);
  int c_list_iterator_is_equal(const c_list_iterator_handle iter1_handle, const c_list_iterator_handle iter2_handle);
  int c_list_iterator_get_data(c_list_iterator_handle iterator_handle, RdmaDirContents* data_out);
  c_list_iterator_handle c_list_iterator_increment(c_list_iterator_handle iterator_handle);
  c_list_iterator_handle c_list_erase(c_list_handle list_handle, c_list_iterator_handle iterator_handle);
  void c_list_destroy_iterator(c_list_iterator_handle iter_handle); // Still needed if iterators are heap allocated

#ifdef __cplusplus
} // End extern "C"
#endif

#endif //C_LIST_H
