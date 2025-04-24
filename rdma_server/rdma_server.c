#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <stdint.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <aio.h>
#include <stdatomic.h>
#include <dirent.h>

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

#include "extras/queue.h"
#include "extras/fsync_queue.h"
#include "extras/write_queue.h"
#include "extras/file_hash_table.h"
#include "extras/common.h"
#include "extras/read_queue.h"

typedef struct rdma {
  rdmaio_rctrl_t* rctrl;
  rdmaio_recv_manager_t* manager;
  rdmaio_devidx_t* dev_idx_array;
  rdmaio_nic_t* nic;
  rdmaio_rmem_t* mem;
  rdmaio_reg_handler_t* handler; // remote
  simple_allocator_t* allocator;
  rdmaio_qp_t* recv_qp;
  recv_entries_handle_t* recv_rs;
  rdmaio_rc_t* send_qp;
  rdmaio_reg_handler_t* local_mr;
  rdmaio_connect_manager_t* cm;
  rdmaio_rc_t* write_send_qp;
  rdmaio_reg_handler_t* write_local_mr;
  rdmaio_connect_manager_t* write_cm;
  rdmaio_rc_t* read_send_qp;
  rdmaio_reg_handler_t* read_local_mr;
  rdmaio_connect_manager_t* read_cm;
} Rdma;

Rdma rdma;
Queue* commandsQueue;
WriteQueue* writeQueue;
FsyncQueue* fsyncQueue;
ReadQueue* readQueue;
HashTable* fileHashTable;

atomic_uint counter = ATOMIC_VAR_INIT(0);

// control messages
char* base_buf = NULL;
size_t current_offset = 0;
size_t total_buffer_size = 0;

// write/flush acks
char* write_base_buf = NULL;
size_t write_current_offset = 0;
size_t write_total_buffer_size = 0;

// read acks
char* read_base_buf = NULL;
size_t read_current_offset = 0;
size_t read_total_buffer_size = 0;

static void init_rdma_struct(Rdma* rdma) {
  rdma->rctrl = NULL;
  rdma->manager = NULL;
  rdma->dev_idx_array = NULL;
  rdma->nic = NULL;
  rdma->mem = NULL;
  rdma->handler = NULL;
  rdma->allocator = NULL;
  rdma->recv_qp = NULL;
  rdma->recv_rs = NULL;
  rdma->send_qp = NULL;
  rdma->local_mr = NULL;
  rdma->cm = NULL;
  rdma->write_send_qp = NULL;
  rdma->write_local_mr = NULL;
  rdma->write_cm = NULL;
  rdma->read_send_qp = NULL;
  rdma->read_local_mr = NULL;
  rdma->read_cm = NULL;
}

static void destroy_rdma_struct(Rdma* rdma) {
  if (rdma->read_cm) {
    rdmaio_connect_manager_destroy(rdma->read_cm);
  }

  if (rdma->read_local_mr) {
    rdmaio_reg_handler_destroy(rdma->read_local_mr);
  }

  if (rdma->read_send_qp) {
    rdmaio_rc_destroy(rdma->read_send_qp);
  }

  if (rdma->write_cm) {
    rdmaio_connect_manager_destroy(rdma->write_cm);
  }

  if (rdma->write_local_mr) {
    rdmaio_reg_handler_destroy(rdma->write_local_mr);
  }

  if (rdma->write_send_qp) {
    rdmaio_rc_destroy(rdma->write_send_qp);
  }

  if (rdma->cm) {
    rdmaio_connect_manager_destroy(rdma->cm);
  }

  if (rdma->local_mr) {
    rdmaio_reg_handler_destroy(rdma->local_mr);
  }

  if (rdma->send_qp) {
    rdmaio_rc_destroy(rdma->send_qp);
  }

  if (rdma->allocator) {
    simple_allocator_destroy(rdma->allocator);
  }

  if (rdma->handler) {
    rdmaio_reg_handler_destroy(rdma->handler);
  }

  if (rdma->mem) {
    rmem_destroy(rdma->mem);
  }

  if (rdma->dev_idx_array) {
    rnic_info_free_dev_names(rdma->dev_idx_array);
  }

  if (rdma->nic) {
    rnic_destroy(rdma->nic);
  }

  if (rdma->manager) {
    recv_manager_destroy(rdma->manager);
  }

  if (rdma->rctrl) {
    rctrl_destroy(rdma->rctrl);
  }

  // sets all values to NULL
  init_rdma_struct(rdma);
}

static void getParentDirectory(char *full_path) {
  if (full_path == NULL || full_path[0] == '\0') {
    return; // Nothing to do
  }

  char *last_slash = strrchr(full_path, '/');

  if (last_slash == NULL) {
    // No slash, parent is current directory. Modify the buffer to ".\0"
    full_path[0] = '.';
    full_path[1] = '\0';
  } else if (last_slash == full_path) {
    // Only slash at the beginning, parent is root. Modify the buffer to "/\0"
    full_path[1] = '\0'; // Keep the first '/', terminate after it
  } else {
    // Slash found elsewhere, replace the last slash with null terminator
    *last_slash = '\0';
  }
}

static void initSendBuf() {
  rdmaio_regattr_t attr = rdmaio_reg_handler_get_attr(rdma.local_mr);
  base_buf = (char *) attr.addr;
  total_buffer_size = RDMA_ACK_BUFFER_SIZE;
  printf("RDMA send ack buffer initialized (size: %zu).\n", total_buffer_size);
}

static void initWriteSendBuf() {
  rdmaio_regattr_t attr = rdmaio_reg_handler_get_attr(rdma.write_local_mr);
  write_base_buf = (char *) attr.addr;
  write_total_buffer_size = RDMA_WRITE_ACK_BUFFER_SIZE;
  printf("RDMA write send ack buffer initialized (size: %zu).\n", write_total_buffer_size);
}

static void initReadSendBuf() {
  rdmaio_regattr_t attr = rdmaio_reg_handler_get_attr(rdma.read_local_mr);
  read_base_buf = (char *) attr.addr;
  read_total_buffer_size = RDMA_READ_ACK_BUFFER_SIZE;
  printf("RDMA read send ack buffer initialized (size: %zu).\n", read_total_buffer_size);
}

static uintptr_t writeResponseToSendBuffer(int status, int err, struct stat * buf) {
  RdmaSyscallResponse response;
  response.status = status;
  response.errnum = err;
  if (buf) {
    response.statbuf = *buf;
  }

  if (!base_buf) {
    initSendBuf();
  }

  int size = sizeof(RdmaSyscallResponse);

  if (current_offset + size > total_buffer_size) {
    // cycle back to start
    current_offset = 0;
  }

  char* new_buf = base_buf;
  new_buf = base_buf + current_offset;
  memcpy(new_buf, (char*) (&response), size);
  current_offset += size;

  return (uintptr_t) new_buf;
}

static inline uintptr_t writeControlResponseToSendBuffer(int status, int err) {
  return writeResponseToSendBuffer(status, err, NULL);
}

static void sendControlAcknowledgement(int reqId, uintptr_t buf) {
  // send ack
  rdmaio_reqdesc_t send_desc;
  send_desc.op = IBV_WR_SEND_WITH_IMM;
  send_desc.flags = IBV_SEND_SIGNALED;
  send_desc.len = sizeof(RdmaSyscallResponse);
  send_desc.wr_id = reqId;

  rdmaio_reqpayload_t send_payload;
  send_payload.local_addr = buf;
  send_payload.remote_addr = 0;
  send_payload.imm_data = reqId;

  char error_msg[256];
  int res_s = rdmaio_rc_send_normal(rdma.send_qp, &send_desc, &send_payload, error_msg, sizeof(error_msg));
  if (res_s != 0) {
    fprintf(stderr, "Error sending ack: %s\n", error_msg);
  }
  struct ibv_wc wc;
  int res_p = rdmaio_rc_wait_rc_comp(rdma.send_qp, NULL, &wc);
  if (res_p != 0) {
    fprintf(stderr, "Error waiting for ack send completion: %d\n", res_p);
  }
}

static uintptr_t writeResponseToWriteSendBuffer(uint32_t lsn_partition, uint64_t lsn_offset, int err) {
  RdmaFlushResponse response;
  response.lsn_partition = lsn_partition;
  response.lsn_offset = lsn_offset;
  response.error = err;

  if (!write_base_buf) {
    initWriteSendBuf();
  }

  int size = sizeof(RdmaFlushResponse);

  if (write_current_offset + size > write_total_buffer_size) {
    write_current_offset = 0;
  }

  char* new_buf = write_base_buf;
  new_buf = write_base_buf + write_current_offset;
  memcpy(new_buf, (char*) (&response), size);
  write_current_offset += size;

  return (uintptr_t) new_buf;
}

static void sendFlushAcknowledgement(int status, uint32_t lsn_partition, uint64_t lsn_offset, int error) {
  // send ack
  rdmaio_reqdesc_t send_desc;
  send_desc.op = IBV_WR_SEND_WITH_IMM;
  send_desc.flags = IBV_SEND_SIGNALED;
  send_desc.len = sizeof(RdmaFlushResponse);
  send_desc.wr_id = 0;

  rdmaio_reqpayload_t send_payload;
  send_payload.local_addr = writeResponseToWriteSendBuffer(lsn_partition, lsn_offset, error);
  send_payload.remote_addr = 0;
  send_payload.imm_data = status;

  char error_msg[256];
  int res_s = rdmaio_rc_send_normal(rdma.write_send_qp, &send_desc, &send_payload, error_msg, sizeof(error_msg));
  if (res_s != 0) {
    fprintf(stderr, "Error sending write ack: %s\n", error_msg);
  }
  struct ibv_wc wc;
  int res_p = rdmaio_rc_wait_rc_comp(rdma.write_send_qp, NULL, &wc);
  if (res_p != 0) {
    fprintf(stderr, "Error waiting for write ack send completion: %d\n", res_p);
  }
}

static uintptr_t writeResponseToReadSendBuffer(unsigned int reqId, int status, int error, off_t offset, size_t size, bool end, char* buf) {
  RdmaReadFileResponse response;
  response.reqId = reqId;
  response.status = status;
  response.errnum = error;
  response.offset = offset;
  response.end = end;
  if(buf) {
    response.size = size;
    memcpy(response.buffer, buf, size);
  } else {
    response.size = 0;
  }

  if (!read_base_buf) {
    initReadSendBuf();
  }

  int buf_size = sizeof(RdmaReadFileResponse);

  if (read_current_offset + buf_size > read_total_buffer_size) {
    read_current_offset = 0;
  }

  char* new_buf = read_base_buf;
  new_buf = read_base_buf + read_current_offset;
  memcpy(new_buf, (char*) (&response), buf_size);
  read_current_offset += buf_size;

  return (uintptr_t) new_buf;
}

static void sendReadAcknowledgement(unsigned int reqId, int status, int error, off_t offset, size_t size, bool end, char* buf) {
  // send ack
  rdmaio_reqdesc_t send_desc;
  send_desc.op = IBV_WR_SEND_WITH_IMM;
  send_desc.flags = IBV_SEND_SIGNALED;
  send_desc.len = sizeof(RdmaReadFileResponse);
  send_desc.wr_id = reqId;

  rdmaio_reqpayload_t send_payload;
  send_payload.local_addr = writeResponseToReadSendBuffer(reqId, status, error, offset, size, end, buf);
  send_payload.remote_addr = 0;
  send_payload.imm_data = status;

  char error_msg[256];
  int res_s = rdmaio_rc_send_normal(rdma.read_send_qp, &send_desc, &send_payload, error_msg, sizeof(error_msg));
  if (res_s != 0) {
    fprintf(stderr, "Error sending read ack: %s\n", error_msg);
  }
}

static void validateAckCompletion(rdmaio_rc_t* queue, int count) {
  for (int i = 0; i < count; i++) {
    struct ibv_wc wc;
    int res_p = rdmaio_rc_wait_rc_comp(queue, NULL, &wc);
    if (res_p != 0) {
      fprintf(stderr, "Error waiting for read ack send completion: %d\n", res_p);
    }
  }
}

/**
 * Lists the names of files in a given directory.
 * @param dir_path The path to the directory.
 * @return 0 on success, -1 on error.
 */
int list_directory_contents(const char *dir_path) {
  DIR *dirp;         // Directory stream pointer
  struct dirent *dp; // Pointer to directory entry structure

  // 1. Open the directory
  dirp = opendir(dir_path);

  // Check if opendir failed
  if (dirp == NULL) {
    uintptr_t response = writeControlResponseToSendBuffer(-1, errno);
    sendControlAcknowledgement(-1, response); // -1 imm_msg will mean error
    fprintf(stderr, "Could not open directory: %s\n", dir_path);
    return -1; // Indicate failure
  }

  printf("Contents of directory: %s\n", dir_path);

  // 2. Read directory entries in a loop
  // Set errno to 0 before the loop to distinguish end-of-directory from error
  errno = 0;
  int size = sizeof(RdmaDirContents);
  if (!base_buf) {
    initSendBuf();
  }
  int count = 0;
  while ((dp = readdir(dirp)) != NULL) {
    // dp->d_name is the null-terminated filename

    // Optional: Skip the "." and ".." entries
    if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0) {
      continue;
    }

    // Print the filename
    printf("  %s\n", dp->d_name);

    // Collect dp->d_name in a buffer and send back to the client.
    RdmaDirContents contents;
    contents.namelen = strlen(dp->d_name);
    snprintf(contents.name, MAX_FILE_NAME_SIZE, "%s", dp->d_name);

  	if (current_offset + size > total_buffer_size) {
  	  // cycle back to start
  	  current_offset = 0;
  	}

  	char* new_buf = base_buf;
  	new_buf = base_buf + current_offset;
  	memcpy(new_buf, (char*) (&contents), size);
  	current_offset += size;

    // send the name to client
  	rdmaio_reqdesc_t send_desc;
  	send_desc.op = IBV_WR_SEND_WITH_IMM;
  	send_desc.flags = IBV_SEND_SIGNALED;
  	send_desc.len = size;
  	send_desc.wr_id = 0; // don't care;

  	rdmaio_reqpayload_t send_payload;
  	send_payload.local_addr = (uintptr_t) new_buf;
  	send_payload.remote_addr = 0;
  	send_payload.imm_data = 1; // indicates that this is a 'message with a value'

  	char error_msg[256];
  	int res_s = rdmaio_rc_send_normal(rdma.send_qp, &send_desc, &send_payload, error_msg, sizeof(error_msg));
  	if (res_s != 0) {
  	  fprintf(stderr, "Error sending ack: %s\n", error_msg);
      uintptr_t response = writeControlResponseToSendBuffer(-1, errno); // signal client about failure
      sendControlAcknowledgement(-1, response); // -1 imm_msg will mean error
      break;
  	}

    ++count;
    if (count == 2040) {
      validateAckCompletion(rdma.send_qp, count);
      count = 0;
    }
  }

  // Check if the loop terminated due to an error (errno will be non-zero)
  if (errno != 0) {
    uintptr_t response = writeControlResponseToSendBuffer(-1, errno); // signal client about failure
    sendControlAcknowledgement(-1, response); // -1 imm_msg will mean error
    fprintf(stderr, "An error occurred while reading directory: %s\n", dir_path);
  }

  // send message with imm_msg = 0 (indicate completion)
  rdmaio_reqdesc_t send_desc;
  send_desc.op = IBV_WR_SEND_WITH_IMM;
  send_desc.flags = IBV_SEND_SIGNALED;
  send_desc.len = 0;
  send_desc.wr_id = 0; // don't care;

  rdmaio_reqpayload_t send_payload;
  send_payload.local_addr = (uintptr_t) base_buf;
  send_payload.remote_addr = 0;
  send_payload.imm_data = 0; // notifies client that all messages are completed

  char error_msg[256];
  int res_s = rdmaio_rc_send_normal(rdma.send_qp, &send_desc, &send_payload, error_msg, sizeof(error_msg));
  if (res_s != 0) {
    fprintf(stderr, "Error sending ack: %s\n", error_msg);
  }

  // validate send completion
  validateAckCompletion(rdma.send_qp, count + 1); // ack for the final message

  // 3. Close the directory
  if (closedir(dirp) == -1) {
    perror("Error closing directory"); // Prints system error based on errno
    fprintf(stderr, "Could not close directory: %s\n", dir_path);
    return -1; // Indicate failure (could return 0 if reading was main goal)
  }

  return 0; // Indicate success
}

static void handleFileOpen(unsigned int reqId, int flags, mode_t mode, char* filename) {
  char full_path[MAX_FILE_NAME_SIZE + LOG_PATH_SIZE];
  int res = snprintf(full_path, MAX_FILE_NAME_SIZE + LOG_PATH_SIZE, "%s%s", LOG_PATH, filename);
  if (res < 0) {
    fprintf(stderr, "Could not create path length\n");
    uintptr_t response = writeControlResponseToSendBuffer(-1, EAGAIN);
    sendControlAcknowledgement(reqId, response);
    return;
  }
  int fd = open(full_path, flags, mode);
  if (fd < 0) {
    fprintf(stderr, "Could not open file %s\n", full_path);
    uintptr_t response = writeControlResponseToSendBuffer(-1, errno);
    sendControlAcknowledgement(reqId, response);
    return;
  }

  unsigned int version = atomic_fetch_add(&counter, 1); // Using atomic_fetch_add
  if(insert(fileHashTable, version, fd) != 0) {
    uintptr_t response = writeControlResponseToSendBuffer(-1, errno);
    close(fd);
    sendControlAcknowledgement(reqId, response);
    return;
  }

  // report success (fd will be non-negative)
  uintptr_t response = writeControlResponseToSendBuffer(fd, 0);
  sendControlAcknowledgement(reqId, response);
  return;
}

static void handleFileLseek(unsigned int reqId, unsigned int version, off_t position, int whence) {
  HashNode* node = get(fileHashTable, version);
  if (!node) {
    fprintf(stderr, "Could not find file node for version %u to lseek.\n", version);
    uintptr_t response = writeControlResponseToSendBuffer(-1, EBADF);
    sendControlAcknowledgement(reqId, response);
    return;
  }

  int server_local_fd = node->fd;
  off_t res = lseek(server_local_fd, position, whence);
  if (res == (off_t)-1) {
    fprintf(stderr, "Could not lseek file (fd: %d): %s\n", server_local_fd, strerror(errno));
    uintptr_t response = writeControlResponseToSendBuffer(-1, errno);
    sendControlAcknowledgement(reqId, response);
    return;
  }

  uintptr_t response = writeControlResponseToSendBuffer(0, 0);
  ((RdmaSyscallResponse*) response)->offset = res;
  sendControlAcknowledgement(reqId, response);
  return;
}

static void handleFileClose(unsigned int reqId, unsigned int version) {
  HashNode* node = get(fileHashTable, version);
  if (!node) {
    fprintf(stderr, "Could not find file node for version %u to close.\n", version);
    uintptr_t response = writeControlResponseToSendBuffer(-1, EBADF);
    sendControlAcknowledgement(reqId, response);
    return;
  }

  // Wait for all pending AIO operations to complete
  while (atomic_load(&node->pending_aio_count) > 0) { // Using atomic_load
    sem_wait(&node->completion_sem);
    // Re-check the count with acquire ordering
  }

  if (close(node->fd) != 0) {
    fprintf(stderr, "Could not close file (fd: %d): %s\n", node->fd, strerror(errno));
    uintptr_t response = writeControlResponseToSendBuffer(-1, errno);
    sendControlAcknowledgement(reqId, response);
    return;
  }

  uintptr_t response = writeControlResponseToSendBuffer(0, 0);
  deleteByRecord(fileHashTable, node);
  sendControlAcknowledgement(reqId, response);
}

static void handleFileUnlink(unsigned int reqId, OpType opType, char* filename) {
  char full_path[MAX_FILE_NAME_SIZE + LOG_PATH_SIZE];
  int res = snprintf(full_path, MAX_FILE_NAME_SIZE + LOG_PATH_SIZE, "%s%s", LOG_PATH, filename);
  if (res < 0) {
    fprintf(stderr, "Could not create path length\n");
    uintptr_t response = writeControlResponseToSendBuffer(-1, EAGAIN);
    sendControlAcknowledgement(reqId, response);
    return;
  }

  if (unlink(filename) != 0) {
    fprintf(stderr, "Could not unlink file %s\n", filename);
    uintptr_t response = writeControlResponseToSendBuffer(-1, errno);
    sendControlAcknowledgement(reqId, response);
    return;
  }

  if (opType == DURABLE) {
    // inplace modification
    getParentDirectory(full_path);

    int dir_fd = open(full_path, O_RDONLY | O_DIRECTORY);
    if (dir_fd == -1) {
      fprintf(stderr, "Could not open directory %s\n", full_path);
      uintptr_t response = writeControlResponseToSendBuffer(-1, EAGAIN);
      sendControlAcknowledgement(reqId, response);
      return;
    }

    if(fsync(dir_fd) != 0) {
      fprintf(stderr, "Could not sync directory %s\n", full_path);
      uintptr_t response = writeControlResponseToSendBuffer(-1, errno);
      sendControlAcknowledgement(reqId, response);
      return;
    }

    if (close(dir_fd) != 0) {
      fprintf(stderr, "Could not close directory %s\n", full_path);
      uintptr_t response = writeControlResponseToSendBuffer(-1, errno);
      sendControlAcknowledgement(reqId, response);
      return;
    }
  }

  uintptr_t response = writeControlResponseToSendBuffer(0, 0);
  sendControlAcknowledgement(reqId, response);
  return;
}

static void handleFileRename(unsigned int reqId, OpType opType, char* oldFilename, char* newFilename) {
  char full_old_path[MAX_FILE_NAME_SIZE + LOG_PATH_SIZE];
  int res = snprintf(full_old_path, MAX_FILE_NAME_SIZE + LOG_PATH_SIZE, "%s%s", LOG_PATH, oldFilename);
  if (res < 0) {
    fprintf(stderr, "Could not create path length\n");
    uintptr_t response = writeControlResponseToSendBuffer(-1, EAGAIN);
    sendControlAcknowledgement(reqId, response);
    return;
  }

  char full_new_path[MAX_FILE_NAME_SIZE + LOG_PATH_SIZE];
  res = snprintf(full_new_path, MAX_FILE_NAME_SIZE + LOG_PATH_SIZE, "%s%s", LOG_PATH, newFilename);
  if (res < 0) {
    fprintf(stderr, "Could not create path length\n");
    uintptr_t response = writeControlResponseToSendBuffer(-1, EAGAIN);
    sendControlAcknowledgement(reqId, response);
    return;
  }

  if (opType == DURABLE) {
    // open old file
    int oldFd = open(full_old_path, O_RDWR, S_IRUSR | S_IWUSR);
    if (oldFd < 0) {
      fprintf(stderr, "Could not open file %s\n", full_old_path);
      uintptr_t response = writeControlResponseToSendBuffer(-1, errno);
      sendControlAcknowledgement(reqId, response);
      return;
    }

    // fsync old file
    if (fsync(oldFd) != 0) {
      fprintf(stderr, "Could not sync file %s\n", full_old_path);
      uintptr_t response = writeControlResponseToSendBuffer(-1, errno);
      close(oldFd);
      sendControlAcknowledgement(reqId, response);
      return;
    }

    // close old file
    if (close(oldFd) != 0) {
      fprintf(stderr, "Could not close file %s\n", full_old_path);
      uintptr_t response = writeControlResponseToSendBuffer(-1, errno);
      sendControlAcknowledgement(reqId, response);
      return;
    }

    // open new file
    int newFd = open(full_new_path, O_RDWR, S_IRUSR | S_IWUSR);
    if (newFd < 0) {
      // fd is negative -- if enoent, great - we can directly attempt to rename! otherwise report error and exit
      if (errno != ENOENT) {
	fprintf(stderr, "Could not open file %s\n", full_new_path);
	uintptr_t response = writeControlResponseToSendBuffer(-1, errno);
	sendControlAcknowledgement(reqId, response);
	return;
      }
    } else {
      // otherwise fsync and close new file
      if (fsync(newFd) != 0) {
	fprintf(stderr, "Could not sync file %s\n", full_new_path);
	uintptr_t response = writeControlResponseToSendBuffer(-1, errno);
	close(newFd);
	sendControlAcknowledgement(reqId, response);
	return;
      }

      if (close(newFd) != 0) {
	fprintf(stderr, "Could not close file %s\n", full_new_path);
	uintptr_t response = writeControlResponseToSendBuffer(-1, errno);
	sendControlAcknowledgement(reqId, response);
	return;
      }
    }
  }

  // call rename
  if (rename(full_old_path, full_new_path) < 0) {
    fprintf(stderr, "Could not rename file %s\n", full_old_path);
    uintptr_t response = writeControlResponseToSendBuffer(-1, errno);
    sendControlAcknowledgement(reqId, response);
    return;
  }

  if (opType == DURABLE) {
    // open new file
    int newFd = open(full_new_path, O_RDWR, S_IRUSR | S_IWUSR);
    if (newFd < 0) {
      fprintf(stderr, "Could not open file %s\n", full_new_path);
      uintptr_t response = writeControlResponseToSendBuffer(-1, errno);
      sendControlAcknowledgement(reqId, response);
      return;
    }
    // fsync new file
    if (fsync(newFd) != 0) {
      fprintf(stderr, "Could not sync file %s\n", full_new_path);
      uintptr_t response = writeControlResponseToSendBuffer(-1, errno);
      close(newFd);
      sendControlAcknowledgement(reqId, response);
      return;
    }

    // close new file
    if (close(newFd) != 0) {
      fprintf(stderr, "Could not close file %s\n", full_new_path);
      uintptr_t response = writeControlResponseToSendBuffer(-1, errno);
      sendControlAcknowledgement(reqId, response);
      return;
    }

    // get file parent
    getParentDirectory(full_new_path);
    int dir_fd = open(full_new_path, O_RDONLY | O_DIRECTORY);
    if (dir_fd == -1) {
      fprintf(stderr, "Could not open directory %s\n", full_new_path);
      uintptr_t response = writeControlResponseToSendBuffer(-1, errno);
      sendControlAcknowledgement(reqId, response);
      return;
    }

    if(fsync(dir_fd) != 0) {
      fprintf(stderr, "Could not sync directory %s\n", full_new_path);
      uintptr_t response = writeControlResponseToSendBuffer(-1, errno);
      sendControlAcknowledgement(reqId, response);
      return;
    }

    if (close(dir_fd) != 0) {
      fprintf(stderr, "Could not close directory %s\n", full_new_path);
      uintptr_t response = writeControlResponseToSendBuffer(-1, errno);
      sendControlAcknowledgement(reqId, response);
      return;
    }
  }

  uintptr_t response = writeControlResponseToSendBuffer(0, 0); // success!
  sendControlAcknowledgement(reqId, response);
  return;
}

static void handleFileTruncate(unsigned int reqId, char* filename, off_t offset) {
  uintptr_t response;
  if (truncate(filename, offset) != 0) {
    fprintf(stderr, "Could not truncate file %s\n", filename);
   	response = writeControlResponseToSendBuffer(-1, errno);
  } else {
    response = writeControlResponseToSendBuffer(0, 0);
  }
  sendControlAcknowledgement(reqId, response);
  return;
}

static void handleFileFtruncate(unsigned int reqId, unsigned int version, off_t size) {
  HashNode* node = get(fileHashTable, version);
  uintptr_t response;
  if (!node) {
    fprintf(stderr, "Could not find file node for version %u to truncate.\n", version);
    response = writeControlResponseToSendBuffer(-1, EBADF);
    sendControlAcknowledgement(reqId, response);
    return;
  }

  if (ftruncate(node->fd, size) != 0) {
    fprintf(stderr, "Could not truncate file (fd: %d): %s\n", node->fd, strerror(errno));
    response = writeControlResponseToSendBuffer(-1, errno);
    sendControlAcknowledgement(reqId, response);
    return;
  }

  response = writeControlResponseToSendBuffer(0, 0);
  sendControlAcknowledgement(reqId, response);
}


static void handleFileStat(unsigned int reqId, char* filename) {
  struct stat statbuf;
  uintptr_t response;
  if (stat(filename, &statbuf) != 0) {
    fprintf(stderr, "Could not stat file %s\n", filename);
    response = writeControlResponseToSendBuffer(-1, errno);
  } else {
    response = writeResponseToSendBuffer(0, 0, &statbuf);
  }
  sendControlAcknowledgement(reqId, response);
  return;
}

static void handleFileFstat(unsigned int reqId, unsigned int version) {
  HashNode* node = get(fileHashTable, version);
  uintptr_t response;
  if (!node) {
    fprintf(stderr, "Could not find file node for version %u to stat.\n", version);
    response = writeControlResponseToSendBuffer(-1, EBADF);
    sendControlAcknowledgement(reqId, response);
    return;
  }

  struct stat statbuf;
  if (fstat(node->fd, &statbuf) != 0) {
    fprintf(stderr, "Could not stat file (fd: %d): %s\n", node->fd, strerror(errno));
    response = writeControlResponseToSendBuffer(-1, errno);
    sendControlAcknowledgement(reqId, response);
    return;
  }

  response = writeResponseToSendBuffer(0, 0, &statbuf);
  sendControlAcknowledgement(reqId, response);
}

static void* processOperations(void* arg) {
  int terminate = 0;
  while(!terminate) {
    // thread blocks here till there is some data to process
    OperationLog* op = dequeue(commandsQueue);
    switch (op->messageType) {
      case RDMA_INIT_OP:
      {
	    RdmaInitPayload payload = op->initPayload;
	    printf("Received message: %s\n", (char* ) payload.message);
	    uintptr_t response = writeControlResponseToSendBuffer(0, 0); // success
	    sendControlAcknowledgement(payload.reqId, response);
	    break;
      }
      case RDMA_OPEN_FILE:
      {
	    RdmaOpenFilePayload payload = op->openFilePayload;
	    handleFileOpen(payload.reqId, payload.flags, payload.mode, payload.filename);
	    break;
      }
      case RDMA_LSEEK_FILE:
      {
	    RdmaLseekFilePayload payload = op->lseekFilePayload;
	    handleFileLseek(payload.reqId, payload.fd, payload.position, payload.whence);
	    break;
      }
      case RDMA_CLOSE_FILE:
      {
        RdmaCloseFilePayload payload = op->closeFilePayload;
	    handleFileClose(payload.reqId, payload.fd);
	    break;
      }
      case RDMA_UNLINK_FILE:
      {
	    RdmaUnlinkFilePayload payload = op->unlinkFilePayload;
	    handleFileUnlink(payload.reqId, payload.opType, payload.filename);
	    break;
      }
      case RDMA_RENAME_FILE:
      {
	    RdmaRenameFilePayload payload = op->renameFilePayload;
		handleFileRename(payload.reqId, payload.opType, payload.oldFilename, payload.newFilename);
		break;
      }
      case RDMA_TRUNCATE_FILE:
      {
        RdmaTruncateFilePayload payload = op->truncateFilePayload;
        handleFileTruncate(payload.reqId, payload.filename, payload.offset);
        break;
      }
      case RDMA_FTRUNCATE_FILE:
      {
        RdmaFtruncateFilePayload payload = op->ftruncateFilePayload;
        handleFileFtruncate(payload.reqId, payload.fd, payload.offset);
        break;
      }
      case RDMA_STAT_FILE:
      {
		RdmaStatPayload payload = op->statPayload;
		handleFileStat(payload.reqId, payload.filename);
		break;
      }
      case RDMA_FSTAT_FILE:
      {
		RdmaFstatPayload payload = op->fstatPayload;
		handleFileFstat(payload.reqId, payload.fd);
		break;
      }
      case RDMA_READ_FILE:
      {
        fprintf(stderr, "Read request in syscalls queue");
		break;
      }
      case RDMA_WRITE_FILE:
	  {
        fprintf(stderr, "write request in syscalls queue");
        break;
      }
      case RDMA_TERMINATE_OP:
      {
		terminate = !terminate;
		free(op);
		break;
      }
      default: fprintf(stderr, "Unknown message type: %d\n", op->messageType);
    }
    free(op);
  }
  return NULL;
}

static void* processWriteRequests(void* arg) {
  RdmaWriteFilePayload* payload;
  HashNode* current_batch_fileNode = NULL;
  struct aiocb* fsync_aiocb = NULL;
  uint32_t last_lsn_partition_of_batch = 0;
  uint64_t last_lsn_offset_of_batch = 0;
  bool batch_active = false;
  struct aiocb** write_aiocb_list = NULL;
  int write_count = 0;
  int write_list_capacity = 0;
  bool batch_error = false; // Flag to track if an error occurred in the current batch
  bool writes_in_batch = false; // Flag to indicate if any writes were processed in the batch

  while (true) {
    payload = writeDequeue(writeQueue);

    if (!payload) {
      fprintf(stderr, "Error: writeDequeue returned NULL, likely queue shutdown or error.\n");
      break;
    }

    if (payload->start) {
      // Start of a new batch
      batch_active = true;
      batch_error = false; // Reset error flag for the new batch
      writes_in_batch = false; // Reset the flag for the new batch
      current_batch_fileNode = get(fileHashTable, payload->fd);
      if (!current_batch_fileNode) {
        fprintf(stderr, "Error: Could not find file descriptor %u in hash table for lsn %u - %lu.\n",
                payload->fd, payload->lsn_partition, payload->lsn_offset);
        batch_active = false;
        free(payload->data);
        free(payload);
        sendFlushAcknowledgement(-1, payload->lsn_partition, payload->lsn_offset, EBADF);
        continue;
      }
      // Consistency check: client-provided fd should match the node's version
      if (payload->fd != current_batch_fileNode->version) {
        fprintf(stderr, "Error: Client FD %u does not match HashNode version %u (start of batch).\n",
                payload->fd, current_batch_fileNode->version);
        batch_active = false;
        current_batch_fileNode = NULL;
        free(payload->data);
        free(payload);
        sendFlushAcknowledgement(-1, payload->lsn_partition, payload->lsn_offset, EINVAL);
        continue;
      }

      // Initialize the list to store aiocb for writes in this batch
      if (write_aiocb_list) {
        free(write_aiocb_list);
        write_aiocb_list = NULL;
      }
      write_count = 0;
      write_list_capacity = 0;
    } else if (!batch_active) {
      // Received a non-start payload when no batch is active, this might be an error
      fprintf(stderr, "Error: Received non-start payload for FD %u and lsn %u - %lu when no batch is active.\n",
              payload->fd, payload->lsn_partition, payload->lsn_offset);
      free(payload->data);
      free(payload);
      sendFlushAcknowledgement(-1, payload->lsn_partition, payload->lsn_offset, EINVAL);
      continue;
    }

    // If an error occurred in the batch, skip further write requests until the 'end' payload
    if (batch_error && !payload->end) {
      free(payload->data);
      free(payload);
      continue;
    }

    // Prepare aiocb for write
    struct aiocb* write_aiocb = (struct aiocb*)calloc(1, sizeof(struct aiocb));
    if (!write_aiocb) {
      perror("Error allocating memory for write aiocb");
      batch_error = true;
      free(payload->data);
      free(payload);
      sendFlushAcknowledgement(-1, payload->lsn_partition, payload->lsn_offset, errno);
      continue;
    }
    write_aiocb->aio_fildes = current_batch_fileNode->fd;
    write_aiocb->aio_offset = payload->offset;
    write_aiocb->aio_buf = payload->data;
    write_aiocb->aio_nbytes = payload->size;
    write_aiocb->aio_sigevent.sigev_notify = SIGEV_NONE; // No notification needed here

    // Increment pending AIO count for the write
    atomic_fetch_add(&current_batch_fileNode->pending_aio_count, 1); // Using atomic_fetch_add

    // Initiate asynchronous write
    if (aio_write(write_aiocb) == -1) {
      perror("Error initiating asynchronous write");
      batch_error = true;
      int currentCount = atomic_fetch_sub(&current_batch_fileNode->pending_aio_count, 1); // Using atomic_fetch_sub
      if (currentCount == 0) {
      sem_post(&current_batch_fileNode->completion_sem);
      }
      free(write_aiocb);
      free(payload->data);
      free(payload);
      sendFlushAcknowledgement(-1, payload->lsn_partition, payload->lsn_offset, errno);
      continue;
    }

    // Store the aiocb for this write
    if (write_count >= write_list_capacity) {
      write_list_capacity += 16; // Allocate in chunks
      struct aiocb** new_list = (struct aiocb**)realloc(write_aiocb_list, write_list_capacity * sizeof(struct aiocb*));
      if (!new_list) {
        perror("Error reallocating memory for write aiocb list");
        batch_error = true;
        int currentCount = atomic_fetch_sub(&current_batch_fileNode->pending_aio_count, 1); // Using atomic_fetch_sub
        if (currentCount == 0) {
          sem_post(&current_batch_fileNode->completion_sem);
        }
        free(write_aiocb);
        free(payload->data);
        free(payload);
        sendFlushAcknowledgement(-1, payload->lsn_partition, payload->lsn_offset, errno);
        continue;
      }
      write_aiocb_list = new_list;
    }
    write_aiocb_list[write_count++] = write_aiocb;
    writes_in_batch = true; // Set the flag as we processed a write

    if (payload->end) {
      batch_active = false;
      if (batch_error) {
        fprintf(stderr, "Error encountered in batch ending at LSN %u - %lu. Skipping fsync and cleaning up writes.\n",
                last_lsn_partition_of_batch, last_lsn_offset_of_batch);
        // Decrement the pending_aio_count for all writes that were initiated
        for (int i = 0; i < write_count; ++i) {
          if (write_aiocb_list[i]) {
            free(write_aiocb_list[i]);
            int currentCount = atomic_fetch_sub(&current_batch_fileNode->pending_aio_count, 1); // Using atomic_fetch_sub
            if (currentCount == 0) {
              sem_post(&current_batch_fileNode->completion_sem);
            }
            write_aiocb_list[i] = NULL; // Avoid double free
          }
        }
        if (write_aiocb_list) {
          free(write_aiocb_list);
          write_aiocb_list = NULL;
        }
        write_count = 0;
        write_list_capacity = 0;
        sendFlushAcknowledgement(-1, payload->lsn_partition, payload->lsn_offset, -1); // Indicate failure
        current_batch_fileNode = NULL;
        continue; // Skip fsync and enqueue
      }

      // Wait for all writes in the batch to complete (if no error)
      for (int i = 0; i < write_count; ++i) {
        if (batch_error) { // Check if an error occurred in a previous iteration
          if (write_aiocb_list[i]) {
            free(write_aiocb_list[i]);
            int currentCount = atomic_fetch_sub(&current_batch_fileNode->pending_aio_count, 1); // Using atomic_fetch_sub
            if (currentCount == 0) {
              sem_post(&current_batch_fileNode->completion_sem);
            }
            write_aiocb_list[i] = NULL; // Mark as processed
          }
          continue; // Skip waiting and further checks
        }

        if (aio_error(write_aiocb_list[i]) == EINPROGRESS) {
          const struct aiocb* const list[] = { write_aiocb_list[i] };
          if (aio_suspend(list, 1, NULL) == -1) {
            if (errno == EINTR) continue;
            perror("Error waiting for asynchronous write");
            sendFlushAcknowledgement(-1, payload->lsn_partition, payload->lsn_offset, errno);
            batch_error = true;
            int currentCount = atomic_fetch_sub(&current_batch_fileNode->pending_aio_count, 1); // Using atomic_fetch_sub
            if (currentCount == 0) {
              sem_post(&current_batch_fileNode->completion_sem);
            }
            free(write_aiocb_list[i]);
            write_aiocb_list[i] = NULL;
            continue; // process (abandon) the rest of the elements in the list
          }
        }
        if (aio_error(write_aiocb_list[i]) != 0) {
          fprintf(stderr, "Warning: Error during asynchronous write completion: %s\n", strerror(aio_error(write_aiocb_list[i])));
          sendFlushAcknowledgement(-1, payload->lsn_partition, payload->lsn_offset, errno);
          int currentCount = atomic_fetch_sub(&current_batch_fileNode->pending_aio_count, 1); // Using atomic_fetch_sub
          if (currentCount == 0) {
            sem_post(&current_batch_fileNode->completion_sem);
          }
          batch_error = true;
        } else {
          // Decrement pending AIO count for successful write completion, except for the last one
          if (i < write_count - 1) {
            atomic_fetch_sub(&current_batch_fileNode->pending_aio_count, 1); // Using atomic_fetch_sub
      // do not signal here!
          }
        }
        free(write_aiocb_list[i]);
        write_aiocb_list[i] = NULL; // Ensure it's freed
      }
      free(write_aiocb_list);
      write_aiocb_list = NULL;
      write_count = 0;
      write_list_capacity = 0;

      // Prepare aiocb for fsync
      fsync_aiocb = (struct aiocb*)calloc(1, sizeof(struct aiocb));
      if (!fsync_aiocb) {
        perror("Error allocating memory for fsync aiocb");
        int currentCount = atomic_fetch_sub(&current_batch_fileNode->pending_aio_count, 1); // Using atomic_fetch_sub
        if (currentCount == 0) {
          sem_post(&current_batch_fileNode->completion_sem);
        }
        free(payload->data);
        free(payload);
        sendFlushAcknowledgement(-1, payload->lsn_partition, payload->lsn_offset, errno);
        current_batch_fileNode = NULL;
        continue;
      }
      fsync_aiocb->aio_fildes = current_batch_fileNode->fd;
      fsync_aiocb->aio_offset = 0;
      fsync_aiocb->aio_nbytes = 0;
      fsync_aiocb->aio_sigevent.sigev_notify = SIGEV_NONE;

      // Initiate asynchronous flush *before* enqueuing
      if (writes_in_batch && !batch_error) {
        if (aio_fsync(O_SYNC, fsync_aiocb) == -1) {
          perror("Error initiating asynchronous flush");
          int currentCount = atomic_fetch_sub(&current_batch_fileNode->pending_aio_count, 1); // Using atomic_fetch_sub
          if (currentCount == 0) {
            sem_post(&current_batch_fileNode->completion_sem);
          }
          free(fsync_aiocb);
          sendFlushAcknowledgement(-1, payload->lsn_partition, payload->lsn_offset, errno);
          current_batch_fileNode = NULL;
          continue;
        }

        // Enqueue fsync operation
        FsyncOperation* fsyncOp = (FsyncOperation*)malloc(sizeof(FsyncOperation));
        if (!fsyncOp) {
          perror("Error allocating memory for FsyncOperation");
          free(fsync_aiocb);
          sendFlushAcknowledgement(-1, payload->lsn_partition, payload->lsn_offset, errno);
          current_batch_fileNode = NULL;
          continue;
        }
        fsyncOp->lsn_partition = last_lsn_partition_of_batch;
        fsyncOp->lsn_offset = last_lsn_offset_of_batch;
        fsyncOp->aio_context = fsync_aiocb;
        fsyncOp->fileNode = current_batch_fileNode;
        fsyncEnqueue(fsyncQueue, fsyncOp);
      } else {
        // If there was a batch error or no writes, free the fsync aiocb
        free(fsync_aiocb);
        sendFlushAcknowledgement(-1, payload->lsn_partition, payload->lsn_offset, -1); // Indicate failure
      }

      current_batch_fileNode = NULL;
    }

    free(payload->data);
    free(payload);
  }

  return NULL;
}

static void* processFlushes(void* arg) {
  while (true) {
    FsyncOperation* payload = fsyncDequeue(fsyncQueue);

    if (!payload) {
      fprintf(stderr, "Error: fsyncDequeue returned NULL, likely queue shutdown or error.\n");
      break; // What a terrible failure: some impossile error happened;
    }

    struct aiocb* cb = payload->aio_context;
    const struct aiocb *list[1] = {cb};
    int ret_suspend;
    ssize_t ret_return;
    bool error = false;
    HashNode* fileNode = payload->fileNode;

    // Wait for the asynchronous operation to complete
    while (aio_error(cb) == EINPROGRESS) {
      ret_suspend = aio_suspend(list, 1, NULL);
      if (ret_suspend == -1) {
        if (errno == EINTR) {
          continue;
        }
        perror("Error waiting for asynchronous flush (aio_suspend)");
        sendFlushAcknowledgement(-1, payload->lsn_partition, payload->lsn_offset, errno);
        error = true;
        break;
      }
    }

    if (!error) {
      // Get the result of the asynchronous operation
      ret_return = aio_return(cb);

      if (ret_return == -1) {
        perror("Error during asynchronous flush (aio_return)");
        sendFlushAcknowledgement(-1, payload->lsn_partition, payload->lsn_offset, errno);
      } else {
        // Successful flush
        sendFlushAcknowledgement(0, payload->lsn_partition, payload->lsn_offset, 0);
      }
    }

    // decrement irrespective of error or success as the operation is guaranteed to have ended
    if (fileNode) {
      int current_count = atomic_fetch_sub(&fileNode->pending_aio_count, 1); // Using atomic_fetch_sub
      if (current_count == 0) {
        sem_post(&fileNode->completion_sem);
      }
    } else {
      fprintf(stderr, "Warning: File node is NULL after dequeuing fsync operation.\n");
    }

    // Free the aiocb context
    if (cb) {
      free(cb);
    }

    // Free the payload structure
    free(payload);
  }
  return NULL;
}

static void* processReadRequests(void* args) {
  while (true) {
    RdmaReadFilePayload* payload = readDequeue(readQueue);

    if (!payload) {
      fprintf(stderr, "Error: readDequeue returned NULL, likely queue shutdown or error.\n");
      break;
    }

    HashNode* fileNode = get(fileHashTable, payload->fd);
    if (!fileNode) {
	  fprintf(stderr, "Failed to find file descriptor for version %u in hash table.\n", payload->fd);
      sendReadAcknowledgement(payload->reqId, -1, EBADF, payload->offset, payload->size, true, NULL);
      free(payload);
      continue;
    }

    char* buf = (char *) malloc(payload->size);
    ssize_t read_size = pread(fileNode->fd, buf, payload->size, payload->offset);
    if (read_size == -1) {
      fprintf(stderr, "Failed to perform read for file descriptor %u due to error %d.\n", payload->fd, errno);
      sendReadAcknowledgement(payload->reqId, -1, errno, payload->offset, payload->size, true, NULL);
      free(buf);
      free(payload);
      continue;
    }

    int count = 1;
    for (off_t offset = 0; offset < read_size; offset += MAX_READ_MESSAGE_SIZE, ++count) {
      // chunked read responses
      char* send_buf = buf + offset;
      ssize_t chunk_size = read_size - offset;
      if (chunk_size <= (ssize_t) MAX_READ_MESSAGE_SIZE) {
        // last chunk
        sendReadAcknowledgement(payload->reqId, 0, 0, offset, chunk_size, true, NULL);
      } else {
        // other chunks
      	sendReadAcknowledgement(payload->reqId, 0, 0, offset, MAX_READ_MESSAGE_SIZE, false, send_buf);
      }
      ++count;
      if (count == 2040) {
        validateAckCompletion(rdma.read_send_qp, count);
        count = 0;
      }
    }

    validateAckCompletion(rdma.read_send_qp, count);

    free(buf);
    free(payload);
  }
  return NULL;
}

int main(int argc, char* argv[]) {
  Rdma rdma;
  init_rdma_struct(&rdma);

  rdma.rctrl = rctrl_create(RDMA_PORT, NULL);
  if (!rdma.rctrl) {
    fprintf(stderr, "Failed to create RCtrl.\n");
    return EXIT_FAILURE;
  }

  rdma.manager = recv_manager_create(rdma.rctrl);
  if (!rdma.manager) {
    fprintf(stderr, "Failed to create recv manager.\n");
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  size_t count = 0;
  rdma.dev_idx_array = rnic_info_query_dev_names(&count);
  if (!rdma.dev_idx_array) {
    fprintf(stderr, "Invalid nic index specified.\n");
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  rdmaio_devidx_t selected_dev_idx = rdma.dev_idx_array[RDMA_USE_NIC_IDX];
  uint8_t gid = 0;
  rdma.nic = rnic_create(selected_dev_idx, gid);
  if (!rdma.nic) {
    fprintf(stderr, "Failed to create RNic.\n");
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  if (!rctrl_register_nic(rdma.rctrl, (uint64_t) RDMA_REG_MEM_NAME, rdma.nic)) {
    fprintf(stderr, "Failed to register NIC for mem id %d.\n", RDMA_REG_MEM_NAME);
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  if (!rctrl_register_nic(rdma.rctrl, (uint64_t) RDMA_REG_ACK_MEM_NAME, rdma.nic)) {
    fprintf(stderr, "Failed to register ack NIC for mem id %d.\n", RDMA_REG_ACK_MEM_NAME);
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  char cq_err_msg[256];
  struct ibv_cq* recv_cq = rdmaio_create_cq(rdma.nic, RDMA_ENTRY_SIZE, cq_err_msg, sizeof(cq_err_msg));
  if (!recv_cq) {
    fprintf(stderr, "Failed to create receive cq: %s.\n", cq_err_msg);
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  rdma.mem = rmem_create(RDMA_BUFFER_SIZE);
  if (!rdma.mem) {
    fprintf(stderr, "Failed to create receive mem of size %d.\n", RDMA_BUFFER_SIZE);
    rdmaio_destroy_cq(recv_cq);
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  rdma.handler = rdmaio_reg_handler_create(rdma.mem, rdma.nic);
  if (!rdma.handler) {
    fprintf(stderr, "Failed to create receive handler.\n");
    rdmaio_destroy_cq(recv_cq);
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  rdmaio_regattr_t reg_attr = rdmaio_reg_handler_get_attr(rdma.handler);
  rdma.allocator = simple_allocator_create(rdma.mem, reg_attr.rkey);
  if (!rdma.allocator) {
    fprintf(stderr, "Failed to create SimpleAllocator.\n");
    rdmaio_destroy_cq(recv_cq);
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  if (!recv_manager_reg_recv_cq(rdma.manager, RDMA_CQ_NAME, recv_cq, rdma.allocator)) {
    fprintf(stderr, "Failed to register receive CQ with RecvManager.\n");
    rdmaio_destroy_cq(recv_cq);
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  if (!rdmaio_rctrl_register_mr(rdma.rctrl, RDMA_REG_MEM_NAME, rdma.handler)) {
    fprintf(stderr, "Failed to register memory region with RCtrl.\n");
    rdmaio_destroy_cq(recv_cq);
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  if (!rctrl_start_daemon(rdma.rctrl)) {
    fprintf(stderr, "Failed to start RCtrl daemon.\n");
    rdmaio_destroy_cq(recv_cq);
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  rdma.recv_qp = rctrl_query_qp(rdma.rctrl, RDMA_CLIENT_QP_NAME);
  while (!rdma.recv_qp) {
    fprintf(stderr, "Client QP not yet registered. Retrying...\n");
    sleep(1);
    rdma.recv_qp = rctrl_query_qp(rdma.rctrl, RDMA_CLIENT_QP_NAME);
  }

  rdma.recv_rs = recv_manager_query_recv_entries(rdma.manager, RDMA_CLIENT_QP_NAME);
  while (!rdma.recv_rs) {
    fprintf(stderr, "Client Recv entries not yet registered. Retrying...\n");
    sleep(1);
    rdma.recv_rs = recv_manager_query_recv_entries(rdma.manager, RDMA_CLIENT_QP_NAME);
  }

  rdmaio_destroy_cq(recv_cq);

  printf("Client Recv entries registered. Ready to receive messages!\n");

  rdmaio_qpconfig_t* qp_config = rdmaio_qpconfig_create_default();
  rdma.send_qp = rdmaio_rc_create(rdma.nic, qp_config, NULL);
  if (!rdma.send_qp) {
    fprintf(stderr, "Failed to create QP");
    rdmaio_qpconfig_destroy(qp_config);
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  rdma.cm = rdmaio_connect_manager_create(RDMA_CLIENT_ADDR);
  if (!rdma.cm) {
    fprintf(stderr, "Failed to create RDMA connection manager.\n");
    rdmaio_qpconfig_destroy(qp_config);
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  rdmaio_iocode_t res = rdmaio_connect_manager_wait_ready(rdma.cm, 1.0, 4);
  if (res == RDMAIO_TIMEOUT) {
    fprintf(stderr, "Connect time out!\n");
    rdmaio_qpconfig_destroy(qp_config);
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  sleep(1);

  res = rdmaio_connect_manager_cc_rc_msg(rdma.cm, RDMA_SERVER_QP_NAME, RDMA_ACK_CQ_NAME, sizeof(RdmaSyscallResponse),
	                                 rdma.send_qp, RDMA_REG_ACK_MEM_NAME, qp_config);
  if (res != RDMAIO_OK) {
    fprintf(stderr, "Failed to connect RC QP: %d\n", res);
    rdmaio_qpconfig_destroy(qp_config);
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  rdmaio_regattr_t remote_attr;
  res = rdmaio_connect_manager_fetch_remote_mr(rdma.cm, RDMA_REG_ACK_MEM_NAME, &remote_attr);
  if (res != RDMAIO_OK) {
    fprintf(stderr, "Failed to fetch remote MR: %d\n", res);
    rdmaio_qpconfig_destroy(qp_config);
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  rdmaio_rmem_t* local_rmem = rmem_create(RDMA_ACK_BUFFER_SIZE);
  rdma.local_mr = rdmaio_reg_handler_create(local_rmem, rdma.nic);
  rdmaio_regattr_t local_mr_attr = rdmaio_reg_handler_get_attr(rdma.local_mr);

  rdmaio_rc_bind_remote_mr(rdma.send_qp, &remote_attr);
  rdmaio_rc_bind_local_mr(rdma.send_qp, &local_mr_attr);

  printf("rc server ready to send acknowledgements to the client!\n");

  rdma.write_send_qp = rdmaio_rc_create(rdma.nic, qp_config, NULL);
  if (!rdma.write_send_qp) {
    fprintf(stderr, "Failed to create write QP");
    rdmaio_qpconfig_destroy(qp_config);
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  rdma.write_cm = rdmaio_connect_manager_create(RDMA_CLIENT_ADDR);
  if (!rdma.write_cm) {
    fprintf(stderr, "Failed to create RDMA connection manager.\n");
    rdmaio_qpconfig_destroy(qp_config);
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  res = rdmaio_connect_manager_wait_ready(rdma.write_cm, 1.0, 4);
  if (res == RDMAIO_TIMEOUT) {
    fprintf(stderr, "Connect time out!\n");
    rdmaio_qpconfig_destroy(qp_config);
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  sleep(1);

  res = rdmaio_connect_manager_cc_rc_msg(rdma.write_cm, RDMA_WRITE_ACK_QP_NAME,
                                        RDMA_WRITE_ACK_CQ_NAME, sizeof(RdmaSyscallResponse),
					rdma.write_send_qp, RDMA_REG_WRITE_ACK_MEM_NAME, qp_config);
  if (res != RDMAIO_OK) {
    fprintf(stderr, "Failed to connect RC QP: %d\n", res);
    rdmaio_qpconfig_destroy(qp_config);
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  rdmaio_regattr_t write_remote_attr;
  res = rdmaio_connect_manager_fetch_remote_mr(rdma.write_cm, RDMA_REG_WRITE_ACK_MEM_NAME, &write_remote_attr);
  if (res != RDMAIO_OK) {
    fprintf(stderr, "Failed to fetch remote MR: %d\n", res);
    rdmaio_qpconfig_destroy(qp_config);
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  rdmaio_rmem_t* write_local_rmem = rmem_create(RDMA_WRITE_ACK_BUFFER_SIZE);
  rdma.write_local_mr = rdmaio_reg_handler_create(write_local_rmem, rdma.nic);
  rdmaio_regattr_t write_local_mr_attr = rdmaio_reg_handler_get_attr(rdma.write_local_mr);

  rdmaio_rc_bind_remote_mr(rdma.write_send_qp, &write_remote_attr);
  rdmaio_rc_bind_local_mr(rdma.write_send_qp, &write_local_mr_attr);

  rdmaio_qpconfig_destroy(qp_config);

  printf("rc server ready to send write lsn to the client!\n");

  rdma.read_send_qp = rdmaio_rc_create(rdma.nic, qp_config, NULL);

  if (!rdma.read_send_qp) {
    fprintf(stderr, "Failed to create read QP");
    rdmaio_qpconfig_destroy(qp_config);
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  rdma.read_cm = rdmaio_connect_manager_create(RDMA_CLIENT_ADDR);
  if (!rdma.read_cm) {
    fprintf(stderr, "Failed to create RDMA connection manager.\n");
    rdmaio_qpconfig_destroy(qp_config);
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  res = rdmaio_connect_manager_wait_ready(rdma.read_cm, 1.0, 4);
  if (res == RDMAIO_TIMEOUT) {
    fprintf(stderr, "Connect time out!\n");
    rdmaio_qpconfig_destroy(qp_config);
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  sleep(1);

  res = rdmaio_connect_manager_cc_rc_msg(rdma.read_cm, RDMA_READ_ACK_QP_NAME,
                                        RDMA_READ_ACK_CQ_NAME, sizeof(RdmaReadFileResponse),
            rdma.read_send_qp, RDMA_REG_READ_ACK_MEM_NAME, qp_config);
  if (res != RDMAIO_OK) {
    fprintf(stderr, "Failed to connect RC QP: %d\n", res);
    rdmaio_qpconfig_destroy(qp_config);
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  rdmaio_regattr_t read_remote_attr;
  res = rdmaio_connect_manager_fetch_remote_mr(rdma.read_cm, RDMA_REG_READ_ACK_MEM_NAME, &read_remote_attr);
  if (res != RDMAIO_OK) {
    fprintf(stderr, "Failed to fetch remote MR: %d\n", res);
    rdmaio_qpconfig_destroy(qp_config);
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  rdmaio_rmem_t* read_local_rmem = rmem_create(RDMA_READ_ACK_BUFFER_SIZE);
  rdma.read_local_mr = rdmaio_reg_handler_create(read_local_rmem, rdma.nic);
  rdmaio_regattr_t read_local_mr_attr = rdmaio_reg_handler_get_attr(rdma.read_local_mr);

  rdmaio_rc_bind_remote_mr(rdma.read_send_qp, &read_remote_attr);
  rdmaio_rc_bind_local_mr(rdma.read_send_qp, &read_local_mr_attr);
  rdmaio_qpconfig_destroy(qp_config);

  printf("rc server ready to send read buffers to the client!\n");

  commandsQueue = queueInit();
  writeQueue = writeQueueInit();
  fsyncQueue = fsyncQueueInit();
  readQueue = readQueueInit();
  fileHashTable = createHashTable();

  pthread_t commandExecutor;
  if(pthread_create(&commandExecutor, NULL, processOperations, NULL) != 0) {
    perror("Failed to spawn command executor thread\n");
    destroyQueue(commandsQueue);
    destroyWriteQueue(writeQueue);
    destroyFsyncQueue(fsyncQueue);
    destroyHashTable(fileHashTable);
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  pthread_t writeExecutor;
  if(pthread_create(&writeExecutor, NULL, processWriteRequests, NULL) != 0) {
    perror("Failed to spawn write executor thread\n");
    destroyQueue(commandsQueue);
    destroyWriteQueue(writeQueue);
    destroyFsyncQueue(fsyncQueue);
    destroyReadQueue(readQueue);
    destroyHashTable(fileHashTable);
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  pthread_t fsyncExecutor;
  if(pthread_create(&fsyncExecutor, NULL, processFlushes, NULL) != 0) {
    perror("Failed to spawn fsync executor thread\n");
    destroyQueue(commandsQueue);
    destroyWriteQueue(writeQueue);
    destroyFsyncQueue(fsyncQueue);
    destroyReadQueue(readQueue);
    destroyHashTable(fileHashTable);
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  pthread_t readExecutor;
  if(pthread_create(&readExecutor, NULL, processReadRequests, NULL) != 0) {
    perror("Failed to spawn read executor thread\n");
    destroyQueue(commandsQueue);
    destroyWriteQueue(writeQueue);
    destroyFsyncQueue(fsyncQueue);
    destroyReadQueue(readQueue);
    destroyHashTable(fileHashTable);
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  // TODO: Fix directory path
  if (list_directory_contents("shore_wal") != 0) {
    perror("Failed to perform directory read and send\n");
    destroyQueue(commandsQueue);
    destroyWriteQueue(writeQueue);
    destroyFsyncQueue(fsyncQueue);
    destroyReadQueue(readQueue);
    destroyHashTable(fileHashTable);
    destroy_rdma_struct(&rdma);
    return EXIT_FAILURE;
  }

  bool terminate = false;
  while (!terminate) {
    rdmaio_recv_iter_t* iter = rdmaio_recv_iter_create(rdma.recv_qp, rdma.recv_rs);
    if (iter) {
      while (rdmaio_recv_iter_has_msgs(iter)) {
		uint32_t imm_msg;
		uintptr_t buf_addr;
		if (rdmaio_recv_iter_cur_msg(iter, &imm_msg, &buf_addr)) {
	  	  MessageType messageType = imm_msg;
	  	  switch (messageType) {
	    	case RDMA_INIT_OP: {
	      	  // parse the request
	      	  OperationLog* op = (OperationLog*) malloc(sizeof(OperationLog));
	      	  op->messageType = messageType;
	      	  RdmaInitPayload* payload = (RdmaInitPayload*) buf_addr;
	      	  op->initPayload = *payload;
	      	  enqueue(commandsQueue, op);
	      	  break;
	    	}
	    	case RDMA_OPEN_FILE:
	    	{
	      	  OperationLog* op = (OperationLog*) malloc(sizeof(OperationLog));
	      	  op->messageType = messageType;
	      	  RdmaOpenFilePayload* payload = (RdmaOpenFilePayload*) buf_addr;
	      	  op->openFilePayload = *payload;
	      	  enqueue(commandsQueue, op);
	      	  break;
	    	}
	    	case RDMA_CLOSE_FILE:
	    	{
	      	  OperationLog* op = (OperationLog*) malloc(sizeof(OperationLog));
	      	  op->messageType = messageType;
	      	  RdmaCloseFilePayload* payload = (RdmaCloseFilePayload*) buf_addr;
	      	  op->closeFilePayload = *payload;
	      	  enqueue(commandsQueue, op);
	      	  break;
	    	}
	    	case RDMA_UNLINK_FILE:
	    	{
	      	  OperationLog* op = (OperationLog*) malloc(sizeof(OperationLog));
	      	  op->messageType = messageType;
	      	  RdmaUnlinkFilePayload* payload = (RdmaUnlinkFilePayload*) buf_addr;
	      	  op->unlinkFilePayload = *payload;
	      	  enqueue(commandsQueue, op);
	      	  break;
	    	}
	    	case RDMA_RENAME_FILE:
	    	{
	      	  OperationLog* op = (OperationLog*) malloc(sizeof(OperationLog));
	      	  op->messageType = messageType;
	      	  RdmaRenameFilePayload* payload = (RdmaRenameFilePayload*) buf_addr;
	      	  op->renameFilePayload = *payload;
	      	  enqueue(commandsQueue, op);
	      	  break;
	    	}
            case RDMA_TRUNCATE_FILE:
            {
              OperationLog* op = (OperationLog*) malloc(sizeof(OperationLog));
              op->messageType = messageType;
              RdmaTruncateFilePayload* payload = (RdmaTruncateFilePayload*) buf_addr;
              op->truncateFilePayload = *payload;
              enqueue(commandsQueue, op);
              break;
            }
            case RDMA_FTRUNCATE_FILE:
            {
              OperationLog* op = (OperationLog*) malloc(sizeof(OperationLog));
              op->messageType = messageType;
              RdmaFtruncateFilePayload* payload = (RdmaFtruncateFilePayload*) buf_addr;
              op->ftruncateFilePayload = *payload;
              enqueue(commandsQueue, op);
              break;
            }
	    	case RDMA_STAT_FILE:
	    	{
	      	  OperationLog* op = (OperationLog*) malloc(sizeof(OperationLog));
	      	  op->messageType = messageType;
	      	  RdmaStatPayload* payload = (RdmaStatPayload*) buf_addr;
	      	  op->statPayload = *payload;
	      	  enqueue(commandsQueue, op);
	      	  break;
	    	}
            case RDMA_FSTAT_FILE:
	    	{
	      	  OperationLog* op = (OperationLog*) malloc(sizeof(OperationLog));
	      	  op->messageType = messageType;
	      	  RdmaFstatPayload* payload = (RdmaFstatPayload*) buf_addr;
	      	  op->fstatPayload = *payload;
	      	  enqueue(commandsQueue, op);
	      	  break;
	    	}
	    	case RDMA_READ_FILE:
            {
              RdmaReadFilePayload* payload = (RdmaReadFilePayload*) malloc(sizeof(RdmaReadFilePayload));
              memcpy(payload, (RdmaReadFilePayload*) buf_addr, sizeof(RdmaReadFilePayload));
              readEnqueue(readQueue, payload);
	      	  break;
            }
	    	case RDMA_WRITE_FILE:
	    	{
	      	  RdmaWriteFilePayload* newPayload = (RdmaWriteFilePayload*) malloc(sizeof(RdmaWriteFilePayload));
	      	  memcpy(newPayload, (RdmaWriteFilePayload*) buf_addr, sizeof(RdmaWriteFilePayload));
	      	  writeEnqueue(writeQueue, newPayload);
	      	  break;
	    	}
	    	case RDMA_TERMINATE_OP:
	      	  break; // the appropriate message is already encoded!
	    	default: fprintf(stderr, "Unknown message type: %d\n", messageType);
	  	  }
		} else {
	  	  fprintf(stderr, "Error getting current message.\n");
		}
		rdmaio_recv_iter_next(iter);
      }
      rdmaio_recv_iter_destroy(iter);
    } else {
      fprintf(stderr, "Error creating recv iterator in main loop.\n");
      break;
    }
  }

  printf("Server shutting down!\n");

  destroy_rdma_struct(&rdma);

  return EXIT_SUCCESS;
}