#ifndef RDMA_STRUCTS_H
#define RDMA_STRUCTS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/stat.h>

/* RDMA IMPORTS */
#include "rdmaio_lib_c.h"
#include "rdmaio_rc_c.h"
#include "rdmaio_rctrl_c.h"
#include "rdmaio_recv_iter_c.h"
#include "rdmaio_regattr_c.h"
#include "rdmaio_result_c.h"
#include "rdmaio_rc_recv_manager_c.h"
#include "rdmaio_mem_c.h"
#include "rdmaio_reg_handler_c.h"
#include "rdmaio_impl_c.h"
#include "rdmaio_qp_c.h"
/* RDMA IMPORTS END */

#define MAX_FILE_NAME_SIZE 64 // WAL file names are likely to be "shore_log/" + 15 (log) - 53 (master) characters, + null-terminator
#define MAX_WRITE_MESSAGE_SIZE (RDMA_MAX_MSG_SIZE - sizeof(off_t) - sizeof(size_t) - 2 * sizeof(int))
#define MAX_READ_MESSAGE_SIZE 1024 - sizeof(unsigned int) - 2 * sizeof(int) - sizeof(size_t) - sizeof(off_t) - sizeof(bool)

/* flags relevant for RDMA setup */
#define RDMA_SERVER_ADDR "192.168.252.211:8888"
#define RDMA_PORT 8888
#define RDMA_USE_NIC_IDX 0
#define RDMA_REG_MEM_NAME 74
#define RDMA_REG_ACK_MEM_NAME 147
#define RDMA_REG_WRITE_ACK_MEM_NAME 219
#define RDMA_REG_READ_ACK_MEM_NAME 283
#define RDMA_CQ_NAME "msg_cq"
#define RDMA_ACK_CQ_NAME "ack_cq"
#define RDMA_WRITE_ACK_CQ_NAME "write_ack_cq"
#define RDMA_READ_ACK_CQ_NAME "read_ack_cq"
#define RDMA_BUFFER_SIZE 1024 * 1024 * 1024
#define RDMA_MAX_MSG_SIZE 1024 * 512
#define RDMA_ENTRY_SIZE 2048
#define RDMA_ACK_BUFFER_SIZE (RDMA_ENTRY_SIZE * (sizeof(int) * 2 + sizeof(struct stat)))
#define RDMA_WRITE_ACK_BUFFER_SIZE (RDMA_ENTRY_SIZE * (sizeof(uint64_t) + sizeof(int)))
#define RDMA_READ_ACK_BUFFER_SIZE (RDMA_ENTRY_SIZE * MAX_READ_MESSAGE_SIZE)
#define RDMA_CLIENT_QP_NAME "send_qp"
#define RDMA_SERVER_QP_NAME "recv_qp"
#define RDMA_WRITE_ACK_QP_NAME "write_ack_qp"
#define RDMA_READ_ACK_QP_NAME "read_ack_qp"
/* RDMA FLAGS END */

// Define the enum for message types
typedef enum {
    RDMA_INIT_OP,
    RDMA_OPEN_FILE,
    RDMA_LSEEK_FILE,
    RDMA_CLOSE_FILE,
    RDMA_UNLINK_FILE,
    RDMA_RENAME_FILE,
    RDMA_STAT_FILE,
    RDMA_FSTAT_FILE,
  	RDMA_WRITE_FILE,
    RDMA_READ_FILE,
    RDMA_TRUNCATE_FILE,
  	RDMA_FTRUNCATE_FILE,
    RDMA_TERMINATE_OP
} MessageType;

typedef enum {
    NORMAL,
    DURABLE
} OpType;

/* Packed structs for synchronous system calls */
typedef struct rdmaOpenFilePayload {
    unsigned int reqId;
    int flags;
    mode_t mode;
    char filename[MAX_FILE_NAME_SIZE];
} RdmaOpenFilePayload __attribute__((packed));

typedef struct rdmaLseekFilePayload {
    unsigned int reqId;
    unsigned int fd;
    off_t position;
    int whence;
} RdmaLseekFilePayload __attribute__((packed));

typedef struct rdmaCloseFilePayload {
    unsigned int reqId;
    unsigned int fd;
} RdmaCloseFilePayload __attribute__((packed));

typedef struct rdmaUnlinkFilePayload {
    unsigned int reqId;
    OpType opType;
    char filename[MAX_FILE_NAME_SIZE];
} RdmaUnlinkFilePayload __attribute__((packed));

typedef struct rdmaRenameFilePayload {
    unsigned int reqId;
    OpType opType;
    char oldFilename[MAX_FILE_NAME_SIZE];
    char newFilename[MAX_FILE_NAME_SIZE];
} RdmaRenameFilePayload __attribute__((packed));

typedef struct rdmaTruncateFilePayload {
  unsigned int reqId;
  off_t offset;
  char filename[MAX_FILE_NAME_SIZE];
} RdmaTruncateFilePayload __attribute__((packed));

typedef struct rdmaFtruncateFilePayload {
  unsigned int reqId;
  unsigned int fd;
  off_t offset;
} RdmaFtruncateFilePayload __attribute__((packed));

typedef struct rdmaStatPayload {
    unsigned int reqId;
    char filename[MAX_FILE_NAME_SIZE];
} RdmaStatPayload __attribute__((packed));

typedef struct rdmaFstatPayload {
    unsigned int reqId;
    unsigned int fd;
} RdmaFstatPayload __attribute__((packed));

typedef struct rdmaSyscallResponse {
    int status;
    int errnum;
    union { // not relevant for most operations, but too much effort to make a separate payload and RDMA setup for each
        struct stat statbuf; // used by statbuf
        off_t offset; // used by lseek
    };
} RdmaSyscallResponse __attribute__((packed));

typedef struct rdmaWriteFilePayload {
   	uint64_t lsn_offset;
    uint32_t lsn_partition;
    unsigned int fd;
    off_t offset;
    size_t size;
    bool start;
    bool end;
    char data[MAX_WRITE_MESSAGE_SIZE];
} RdmaWriteFilePayload __attribute__((packed));

typedef struct rdmaInitPayload {
    unsigned int reqId;
    char message[RDMA_MAX_MSG_SIZE - sizeof(int)];
} RdmaInitPayload __attribute__((packed));

typedef struct rdmaFlushResponse {
    uint64_t lsn_offset;
    uint32_t lsn_partition;
    int error;
} RdmaFlushResponse __attribute__((packed));

typedef struct rdmaReadFilePayload {
    unsigned int reqId;
    unsigned int fd;
    off_t offset;
    size_t size;
} RdmaReadFilePayload __attribute__((packed));

typedef struct rdmaReadFileResponse {
    unsigned int reqId;
    int status;
    int errnum;
    size_t size;
    off_t offset; // relative to the original offset
    bool end;
    char buffer[MAX_READ_MESSAGE_SIZE];
} RdmaReadFileResponse __attribute__((packed));

typedef struct rdmaDirContents {
    int namelen;
    char name[MAX_FILE_NAME_SIZE];
} RdmaDirContents __attribute__((packed));

#endif //RDMA_STRUCTS_H
