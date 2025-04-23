// c_list.cpp
#include "c_list.h" // Include the C-callable header (declares c_list_handle_s, c_list_iterator_handle_s)

#include <list>      // For std::list
#include <iterator>  // For std::list iterators
#include <cstring>   // For memcpy, strlen
#include <stdexcept> // For exceptions
#include <cerrno>    // For errno

// --- Define the incomplete struct types ---
// These definitions are needed in the C++ implementation.
struct c_list_handle_s {};
struct c_list_iterator_handle_s {};

// Internal type for the C++ list
using namespace std;
using RdmaDirContentsList = list<RdmaDirContents>;
using RdmaDirContentsListIterator = RdmaDirContentsList::iterator;

// --- Implement wrapper functions using reinterpret_cast and the new handle types ---

c_list_handle c_list_create() {
    // Allocate the C++ list object on the heap
    return reinterpret_cast<c_list_handle>(new RdmaDirContentsList());
}

void c_list_destroy(c_list_handle list_handle) {
    if (list_handle) {
        // Cast the opaque handle back to the C++ list pointer and delete it
        delete reinterpret_cast<RdmaDirContentsList*>(list_handle);
    }
}

int c_list_add_back(c_list_handle list_handle, const RdmaDirContents* data) {
    // Cast handle back
    RdmaDirContentsList* list = reinterpret_cast<RdmaDirContentsList*>(list_handle);
    if (!list || !data) { return -1; } // Error check
    list->push_back(*data); // Add element (copies data)
    return 0;
}

c_list_iterator_handle c_list_begin(c_list_handle list_handle) {
     RdmaDirContentsList* list = reinterpret_cast<RdmaDirContentsList*>(list_handle);
     if (!list) { return nullptr; }
    // Get the iterator, allocate on heap, cast its pointer address to opaque handle
     return reinterpret_cast<c_list_iterator_handle>(
        new RdmaDirContentsListIterator(list->begin())
    );
}

c_list_iterator_handle c_list_end(c_list_handle list_handle) {
     RdmaDirContentsList* list = reinterpret_cast<RdmaDirContentsList*>(list_handle);
     if (!list) { return nullptr; }
    // Get the iterator, allocate on heap, cast its pointer address to opaque handle
     return reinterpret_cast<c_list_iterator_handle>(
        new RdmaDirContentsListIterator(list->end())
    );
}

void c_list_destroy_iterator(c_list_iterator_handle iter_handle) {
    if (iter_handle) {
        // Cast handle back to iterator pointer and delete it
        delete reinterpret_cast<RdmaDirContentsListIterator*>(iter_handle);
    }
}

int c_list_iterator_is_equal(const c_list_iterator_handle iter1_handle, const c_list_iterator_handle iter2_handle) {
    // Cast handles back to iterator pointers and compare the iterators they point to
     return (*reinterpret_cast<const RdmaDirContentsListIterator*>(iter1_handle) ==
            *reinterpret_cast<const RdmaDirContentsListIterator*>(iter2_handle));
}

int c_list_iterator_get_data(c_list_iterator_handle iterator_handle, RdmaDirContents* data_out) {
     if (!iterator_handle || !data_out) { return -1; }
    // Cast handle, dereference iterator to get element, copy data
    *data_out = **reinterpret_cast<RdmaDirContentsListIterator*>(iterator_handle);
    return 0;
}

c_list_iterator_handle c_list_iterator_increment(c_list_iterator_handle iterator_handle) {
     if (!iterator_handle) { return NULL; }
    // Cast handle, increment the iterator it points to, return the *same* handle (now pointing to next)
    (++*reinterpret_cast<RdmaDirContentsListIterator*>(iterator_handle));
    return iterator_handle; // Return the updated handle
}

c_list_iterator_handle c_list_erase(c_list_handle list_handle, c_list_iterator_handle iterator_handle) {
     if (!list_handle || !iterator_handle) { return NULL; }
    // Cast handles
    RdmaDirContentsList* list = reinterpret_cast<RdmaDirContentsList*>(list_handle);
    RdmaDirContentsListIterator* old_iter_ptr = reinterpret_cast<RdmaDirContentsListIterator*>(iterator_handle);

    // Erase the element, get the iterator to the next element
    RdmaDirContentsListIterator next_iter = list->erase(*old_iter_ptr);

    // Delete the old iterator handle's memory
    delete old_iter_ptr;

    // Create a new iterator handle pointing to the next iterator position
    return reinterpret_cast<c_list_iterator_handle>(new RdmaDirContentsListIterator(next_iter));
}