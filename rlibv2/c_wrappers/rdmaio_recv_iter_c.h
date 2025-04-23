// rdmaio_recv_iter_c.h
#ifndef RDMAIO_RECV_ITER_C_H
#define RDMAIO_RECV_ITER_C_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <infiniband/verbs.h>
#include "rdmaio_qp_c.h"
#include "rdmaio_rc_recv_manager_c.h"

#ifdef __cplusplus
extern "C" {
#endif

  // Definition for the RecvIter wrapper type
  typedef struct rdmaio_recv_iter_t {
    void* internal;
  } rdmaio_recv_iter_t;

  /*!
   * @brief Creates a RecvIter object for a given QP.
   * @param qp A pointer to the wrapped QP object (rdmaio_qp_t*).
   * @param recv_entries A pointer to the wrapped RecvEntries object.
   * @return A pointer to the created RecvIter object, or NULL on failure.
   */
  rdmaio_recv_iter_t* rdmaio_recv_iter_create(rdmaio_qp_t* qp, recv_entries_handle_t* recv_entries_handle);

  /*!
   * @brief Checks if there are more messages available in the iterator.
   * @param iter The RecvIter object.
   * @return True if there are more messages, false otherwise.
   */
  bool rdmaio_recv_iter_has_msgs(const rdmaio_recv_iter_t* iter);

  /*!
   * @brief Moves the iterator to the next message.
   * @param iter The RecvIter object.
   */
  void rdmaio_recv_iter_next(rdmaio_recv_iter_t* iter);

  /*!
   * @brief Gets the current message from the iterator.
   * @param iter The RecvIter object.
   * @param out_imm_data Pointer to store the immediate data of the message. Can be NULL.
   * @param out_buf_addr Pointer to store the buffer address of the message. Can be NULL.
   * @return True if a message was available, false otherwise.
   */
  bool rdmaio_recv_iter_cur_msg(const rdmaio_recv_iter_t* iter, uint32_t* out_imm_data, uintptr_t* out_buf_addr);

  /*!
   * @brief Clears the iterator and reposts the receive buffers.
   * @param iter The RecvIter object.
   */
  void rdmaio_recv_iter_clear(rdmaio_recv_iter_t* iter);

  /*!
   * @brief Destroys the RecvIter object.
   * @param iter The RecvIter object to destroy.
   */
  void rdmaio_recv_iter_destroy(rdmaio_recv_iter_t* iter);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // RDMAIO_RECV_ITER_C_H