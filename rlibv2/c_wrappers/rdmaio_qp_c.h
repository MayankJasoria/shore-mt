// rdmaio_qp_c.h
#ifndef RDMAIO_QP_C_H
#define RDMAIO_QP_C_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rdmaio_qp_t {
  void* internal; // Holds a pointer to Arc<Dummy>
} rdmaio_qp_t;

#ifdef __cplusplus
}
#endif

#endif // RDMAIO_QP_C_H