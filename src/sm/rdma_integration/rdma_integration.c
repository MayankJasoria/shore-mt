#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "rdma_integration.h"
#include "hashmap.h"

#ifndef MIN
#define MIN(x, y)       ((x) < (y) ? (x) : (y))
#endif /*MIN*/

typedef struct rdma {
  rdmaio_rctrl_t* ctrl;
  rdmaio_recv_manager_t* manager;
  rdmaio_devidx_t* dev_idx_array;
  rdmaio_nic_t* nic;
  rdmaio_rc_t* send_qp;
  rdmaio_connect_manager_t* cm;
  rdmaio_rmem_t* mem;
  struct ibv_cq* recv_cq;
  rdmaio_reg_handler_t* handler;
  simple_allocator_t* allocator;
  rdmaio_rmem_t* local_rmem;
  rdmaio_reg_handler_t* local_mr;
  rdmaio_qp_t* recv_qp;
  recv_entries_handle_t* recv_rs;
  rdmaio_rmem_t* write_mem;
  struct ibv_cq* write_recv_cq;
  rdmaio_reg_handler_t* write_handler;
  simple_allocator_t* write_allocator;
  rdmaio_qp_t* write_recv_qp;
  recv_entries_handle_t* write_recv_rs;
  rdmaio_rmem_t* read_mem;
  struct ibv_cq* read_recv_cq;
  rdmaio_reg_handler_t* read_handler;
  simple_allocator_t* read_allocator;
  rdmaio_qp_t* read_recv_qp;
  recv_entries_handle_t* read_recv_rs;
} rdma;

rdma rdmaContext;
HashTable* hashTable;
static atomic_uint counter = ATOMIC_VAR_INIT(0);
char* base_buf = NULL;
size_t current_offset = 0;
size_t total_buffer_size = 0;
pthread_spinlock_t send_lock;
pthread_spinlock_t read_recv_lock;

static atomic_bool global_polling = ATOMIC_VAR_INIT(false);

static void setInitialValues() {
  rdmaContext.ctrl = NULL;
  rdmaContext.manager = NULL;
  rdmaContext.dev_idx_array = NULL;
  rdmaContext.nic = NULL;
  rdmaContext.send_qp = NULL;
  rdmaContext.cm = NULL;
  rdmaContext.local_mr = NULL;
  rdmaContext.local_rmem = NULL;
  rdmaContext.recv_cq = NULL;
  rdmaContext.mem = NULL;
  rdmaContext.handler = NULL;
  rdmaContext.allocator = NULL;
  rdmaContext.recv_qp = NULL;
  rdmaContext.recv_rs = NULL;
  rdmaContext.write_recv_cq = NULL;
  rdmaContext.write_mem = NULL;
  rdmaContext.write_handler = NULL;
  rdmaContext.write_allocator = NULL;
  rdmaContext.write_recv_qp = NULL;
  rdmaContext.write_recv_rs = NULL;
  rdmaContext.read_recv_cq = NULL;
  rdmaContext.read_mem = NULL;
  rdmaContext.read_handler = NULL;
  rdmaContext.read_allocator = NULL;
  rdmaContext.read_recv_qp = NULL;
  rdmaContext.read_recv_rs = NULL;
}
static void rdmaContextDestroy() {
  if (rdmaContext.read_recv_rs) {
    recv_entries_handle_destroy(rdmaContext.read_recv_rs);
}
if (rdmaContext.read_allocator) {
    simple_allocator_destroy(rdmaContext.read_allocator);
}
if (rdmaContext.read_handler) {
    // Assuming read/write handlers use the same destroy function as handler/local_mr
    rdmaio_reg_handler_destroy(rdmaContext.read_handler);
}
if (rdmaContext.read_mem) {
    // Assuming read/write mems use the same destroy function as mem
    rmem_destroy(rdmaContext.read_mem);
}
if (rdmaContext.write_recv_rs) {
    recv_entries_handle_destroy(rdmaContext.write_recv_rs);
}
if (rdmaContext.write_allocator) {
    simple_allocator_destroy(rdmaContext.write_allocator);
}
if (rdmaContext.write_handler) {
    // Assuming read/write handlers use the same destroy function as handler/local_mr
    rdmaio_reg_handler_destroy(rdmaContext.write_handler);
}
if (rdmaContext.write_mem) {
    // Assuming read/write mems use the same destroy function as mem
    rmem_destroy(rdmaContext.write_mem);
}
// These are the members without read_/write_ prefixes in your list
if (rdmaContext.recv_rs) {
    // Using your previously provided destroy function
    recv_entries_handle_destroy(rdmaContext.recv_rs);
}
if (rdmaContext.allocator) {
    simple_allocator_destroy(rdmaContext.allocator);
}
if (rdmaContext.handler) {
    rdmaio_reg_handler_destroy(rdmaContext.handler);
}
if (rdmaContext.mem) {
    rmem_destroy(rdmaContext.mem);
}
// These seem to be more fundamental or general context members
if (rdmaContext.local_rmem) {
    // **Guessing** local_rmem uses the same destroy function as mem
    rmem_destroy(rdmaContext.local_rmem);
}
if (rdmaContext.local_mr) {
    rdmaio_reg_handler_destroy(rdmaContext.local_mr);
}
if (rdmaContext.cm) {
    rdmaio_connect_manager_destroy(rdmaContext.cm);
}
if (rdmaContext.send_qp) {
    rdmaio_rc_destroy(rdmaContext.send_qp);
}
if (rdmaContext.nic) {
    rnic_destroy(rdmaContext.nic);
}
if (rdmaContext.dev_idx_array) {
    rnic_info_free_dev_names(rdmaContext.dev_idx_array);
}
if (rdmaContext.manager) {
    // Assumes 'manager' corresponds to recv_manager
    recv_manager_destroy(rdmaContext.manager);
}
if (rdmaContext.ctrl) {
    // Assumes 'ctrl' corresponds to rctrl
    rctrl_destroy(rdmaContext.ctrl);
}
  setInitialValues();
}

static void initSendBuf() {
  rdmaio_regattr_t attr = rdmaio_reg_handler_get_attr(rdmaContext.local_mr);
  base_buf = (char *) attr.addr;
  total_buffer_size = RDMA_BUFFER_SIZE;
  printf("RDMA send buffer initialized (size: %zu).\n", total_buffer_size);
}

// crude implementation only for GCC
static unsigned int getRequestId() {
  return atomic_fetch_add(&counter, 1);
}

static bool sendRdmaMessage(char* buf, int size, int imm_data) {
  bool success = false;

  rdmaio_reqdesc_t send_desc;
  send_desc.op = IBV_WR_SEND_WITH_IMM;
  send_desc.flags = IBV_SEND_SIGNALED;
  send_desc.len = size;
  send_desc.wr_id = 0;

  rdmaio_reqpayload_t send_payload;
  send_payload.remote_addr = 0;
  send_payload.imm_data = imm_data;

  if (pthread_spin_lock(&send_lock) != 0) {
    fprintf(stderr, "pthread_spin_lock failed.\n");
    return false;
  }

  char* new_buf = base_buf;
  if (buf) {
    new_buf = base_buf + current_offset;
    memcpy(new_buf, buf, size);
    current_offset += size;
  }

  if (current_offset > total_buffer_size) {
    // cycle back to the start
    current_offset = 0;
  }

  send_payload.local_addr = (uintptr_t) new_buf;

  char error_msg[256];
  int res_s = rdmaio_rc_send_normal(rdmaContext.send_qp, &send_desc, &send_payload, error_msg, sizeof(error_msg));
  if (res_s != 0) {
    fprintf(stderr, "Error sending RDMA message: %s.\n", error_msg);
    goto unlock;
  }

  struct ibv_wc wc;
  int res_p = rdmaio_rc_wait_rc_comp(rdmaContext.send_qp, NULL, &wc);
  if (res_p != 0) {
    fprintf(stderr, "Error waiting for RDMA message completion: %d.\n", res_p);
    goto unlock;
  }

  success = true;

unlock:
  if (pthread_spin_unlock(&send_lock) != 0) {
    fprintf(stderr, "pthread_spin_unlock failed.\n");
  }

  return success;
}

static RdmaSyscallResponse awaitAcknowledgement(struct chainNode* node) {
  // Loop to handle waiting and becoming the poller.
  while (true) {

    // check if ack was received
    if (atomic_load(&node->response_set)) {
      RdmaSyscallResponse response = node->response;
      deleteNode(hashTable, node);
      return response;
    }

    // response was not received, try to become poller
    bool expected_bool = false;
    bool desired_bool = true;
    bool currently_polling = atomic_compare_exchange_strong(&global_polling, &expected_bool, desired_bool);

    if (!currently_polling) {
      // Successfully became the global poller. Break the loop to proceed with polling.
      break;
    } else {
      // Another thread is polling or we failed to become the poller. Wait on this node's semaphore.
      sem_wait(&node->response_ready);
      if (atomic_load(&node->response_set)) {
        // Response received. Delete this node and return the response.
        RdmaSyscallResponse response = node->response;
        deleteNode(hashTable, node);
        return response;
      }
      /*
       * If response is not set after waking up, loop again to try and become the poller.
       * This happens if the previous poller finished and signaled us to take over.
       */
    }
  }

  // we are the poller. First check if our response was set
  if (atomic_load(&node->response_set)) {
    // Response received.
    RdmaSyscallResponse response = node->response;

    // relinquish polling
    bool expected_bool = true;
    bool desired_bool = false;
    while (!atomic_compare_exchange_strong(&global_polling, &expected_bool, desired_bool)) {
      // retry (ideally shouldn't be needed, as we are here only beecause the global_polling variable was true)
    }

    // signal another waiting thread
    ChainNode* next_poller_node = findFirstEntry(hashTable);
    if (next_poller_node != NULL) {
      sem_post(&next_poller_node->response_ready);
    }

    // delete the hashtable entry
    deleteNode(hashTable, node);
    return response;
  }

  // Polling loop: Continue polling until an acknowledgement for this node's request ID is received.
  RdmaSyscallResponse response;
  bool received_own_ack = false;

  while (!received_own_ack) {
    rdmaio_recv_iter_t* iter = rdmaio_recv_iter_create(rdmaContext.recv_qp, rdmaContext.recv_rs);
    if (!iter) {
      fprintf(stderr, "Error creating RDMA receive iterator.\n");
      continue; // retry this operation
    }

    // Iterate through all available messages in the current iterator.
    while (rdmaio_recv_iter_has_msgs(iter)) {
      uint32_t imm_msg;
      uintptr_t buf_addr;
      if (rdmaio_recv_iter_cur_msg(iter, &imm_msg, &buf_addr)) {
        unsigned int reqId = (unsigned int)imm_msg;
        RdmaSyscallResponse* received_response = (RdmaSyscallResponse*)buf_addr;

        // Check if this acknowledgement is for our own request.
        if (reqId == node->reqId) {
          // record our response
          received_own_ack = true;
          response = *received_response;

          // delete our record from pending responses
          deleteNode(hashTable, node);
        } else {
          // Use the modify function to handle acknowledgements for other requests.
          modify(hashTable, reqId, *received_response);
        }
      } else {
        fprintf(stderr, "Error receiving message from RDMA iterator.\n");
      }
      rdmaio_recv_iter_next(iter);
    }
    rdmaio_recv_iter_destroy(iter);
  }

  // relinquish polling
  bool expected_bool = true;
  bool desired_bool = false;
  while (!atomic_compare_exchange_strong(&global_polling, &expected_bool, desired_bool)) {
    // retry (ideally shouldn't be needed, as we are here only beecause the global_polling variable was true)
  }

  // Signal another waiting thread to become the poller if one exists
  ChainNode* next_poller_node = findFirstEntry(hashTable);
  if (next_poller_node != NULL) {
    sem_post(&next_poller_node->response_ready);
  }

  return response;
}

bool rdmaContextCreate() {
  printf("Starting RDMA context creation.\n");

  if (rdmaContext.ctrl) {
    fprintf(stderr, "RDMA context already exists.\n");
    return true;	// context already exists
  }

  setInitialValues();

  rdmaContext.ctrl = rctrl_create(RDMA_PORT, NULL);
  if (!rdmaContext.ctrl) {
    fprintf(stderr, "Failed to create RDMA control block.\n");
    return false;
  }

  rdmaContext.manager = recv_manager_create(rdmaContext.ctrl);
  if (!rdmaContext.manager) {
    fprintf(stderr, "Failed to create RDMA receive manager.\n");
    rdmaContextDestroy();
    return false;
  }

  size_t count = 0;
  rdmaContext.dev_idx_array = rnic_info_query_dev_names(&count);
  if (rdmaContext.dev_idx_array == NULL || count== 0) {
    fprintf(stderr, "Failed to query RDMA device names.\n");
    rdmaContextDestroy();
    return false;
  }

  if (RDMA_USE_NIC_IDX >= count) {
    fprintf(stderr, "Invalid RDMA NIC index.\n");
    rdmaContextDestroy();
    return false;
  }

  rdmaio_devidx_t selected_dev_idx = rdmaContext.dev_idx_array[RDMA_USE_NIC_IDX];
  uint8_t gid = 0;

  rdmaContext.nic = rnic_create(selected_dev_idx, gid);
  if (!rdmaContext.nic) {
    fprintf(stderr, "Failed to create RDMA NIC.\n");
    rdmaContextDestroy();
    return false;
  }

  if (!rctrl_register_nic(rdmaContext.ctrl, (uint64_t) RDMA_REG_MEM_NAME, rdmaContext.nic)) {
    fprintf(stderr, "Failed to register RDMA NIC with control block (memory region name: %d).\n", RDMA_REG_MEM_NAME);
    rdmaContextDestroy();
    return false;
  }

  if (!rctrl_register_nic(rdmaContext.ctrl, (uint64_t) RDMA_REG_ACK_MEM_NAME, rdmaContext.nic)) {
    fprintf(stderr, "Failed to register RDMA NIC with control block (memory region name: %d).\n", RDMA_REG_ACK_MEM_NAME);
    rdmaContextDestroy();
    return false;
  }

  // create send qp
  rdmaio_qpconfig_t* qp_config = rdmaio_qpconfig_create_default();
  if (!qp_config) {
    fprintf(stderr, "Failed to create RDMA QP configuration.\n");
    rdmaContextDestroy();
    return false;
  }

  rdmaContext.send_qp = rdmaio_rc_create(rdmaContext.nic, qp_config, NULL);
  if (!rdmaContext.send_qp) {
    fprintf(stderr, "Failed to create RDMA send QP.\n");
    rdmaio_qpconfig_destroy(qp_config);
    rdmaContextDestroy();
    return false;
  }

  rdmaContext.cm = rdmaio_connect_manager_create(RDMA_SERVER_ADDR);
  if (!rdmaContext.cm) {
    fprintf(stderr, "Failed to create RDMA connect manager for address: %s.\n", RDMA_SERVER_ADDR);
    rdmaio_qpconfig_destroy(qp_config);
    rdmaContextDestroy();
    return false;
  }

  rdmaio_iocode_t res = rdmaio_connect_manager_wait_ready(rdmaContext.cm, 1.0, 4);	// Timeout in seconds
  if (res != RDMAIO_OK) {
    fprintf(stderr, "RDMA connect manager failed to become ready.\n");
    rdmaio_qpconfig_destroy(qp_config);
    rdmaContextDestroy();
    return false;
  }

  res = rdmaio_connect_manager_cc_rc_msg(rdmaContext.cm, RDMA_CLIENT_QP_NAME, RDMA_CQ_NAME, RDMA_MAX_MSG_SIZE,
									     rdmaContext.send_qp, RDMA_REG_MEM_NAME, qp_config);
  if (res != RDMAIO_OK) {
    fprintf(stderr, "RDMA connect manager failed to complete RC message connection.\n");
    rdmaio_qpconfig_destroy(qp_config);
    rdmaContextDestroy();
    return false;
  }

  rdmaio_regattr_t remote_attr;
  res = rdmaio_connect_manager_fetch_remote_mr(rdmaContext.cm, RDMA_REG_MEM_NAME, &remote_attr);

  rdmaContext.local_rmem = rmem_create(RDMA_BUFFER_SIZE);
  if (!rdmaContext.local_rmem) {
    fprintf(stderr, "Failed to create RDMA local memory region (size: %d).\n", RDMA_BUFFER_SIZE);
    rdmaio_qpconfig_destroy(qp_config);
    rdmaContextDestroy();
    return false;
  }

  rdmaContext.local_mr = rdmaio_reg_handler_create(rdmaContext.local_rmem, rdmaContext.nic);
  if (!rdmaContext.local_mr) {
    fprintf(stderr, "Failed to create RDMA local memory registration handler.\n");
    rmem_destroy(rdmaContext.local_rmem);
    rdmaio_qpconfig_destroy(qp_config);
    rdmaContextDestroy();
    return false;
  }

  rdmaio_regattr_t local_mr_attr = rdmaio_reg_handler_get_attr(rdmaContext.local_mr);
  rdmaio_rc_bind_remote_mr(rdmaContext.send_qp, &remote_attr);
  rdmaio_rc_bind_local_mr(rdmaContext.send_qp, &local_mr_attr);

  rdmaio_qpconfig_destroy(qp_config);

  // SEND QP CREATION COMPLETED
  // create recv qp
  char cq_err_msg[256];
  rdmaContext.recv_cq = rdmaio_create_cq(rdmaContext.nic, RDMA_ENTRY_SIZE, cq_err_msg, sizeof(cq_err_msg));
  if (!rdmaContext.recv_cq) {
    fprintf(stderr, "Failed to create RDMA receive completion queue: %s.\n", cq_err_msg);
    rdmaContextDestroy();
    return false;
  }

  rdmaContext.mem = rmem_create(RDMA_ACK_BUFFER_SIZE);
  if (!rdmaContext.mem) {
    fprintf(stderr, "Failed to create RDMA memory region for acknowledgements (size: %zu).\n", RDMA_ACK_BUFFER_SIZE);
    rdmaContextDestroy();
    return false;
  }

  rdmaContext.handler = rdmaio_reg_handler_create(rdmaContext.mem, rdmaContext.nic);
  if (!rdmaContext.handler) {
    fprintf(stderr, "Failed to create RDMA registration handler for acknowledgements.\n");
    rdmaContextDestroy();
    return false;
  }

  rdmaio_regattr_t reg_attr = rdmaio_reg_handler_get_attr(rdmaContext.handler);

  rdmaContext.allocator = simple_allocator_create(rdmaContext.mem, reg_attr.rkey);
  if (!rdmaContext.allocator) {
    fprintf(stderr, "Failed to create RDMA simple allocator for acknowledgements.\n");
    rdmaContextDestroy();
    return false;
  }

  if (!recv_manager_reg_recv_cq(rdmaContext.manager, RDMA_ACK_CQ_NAME, rdmaContext.recv_cq, rdmaContext.allocator)) {
    fprintf(stderr, "Failed to register receive CQ with receive manager (CQ name: %s).\n", RDMA_ACK_CQ_NAME);
    rdmaContextDestroy();
    return false;
  }

  if (!rdmaio_rctrl_register_mr(rdmaContext.ctrl, RDMA_REG_ACK_MEM_NAME, rdmaContext.handler)) {
    fprintf(stderr, "Failed to register memory region for acknowledgements with control block (memory region name: %d).\n", RDMA_REG_ACK_MEM_NAME);
    rdmaContextDestroy();
    return false;
  }

  // create write ack recv qp
  rdmaContext.write_recv_cq = rdmaio_create_cq(rdmaContext.nic, RDMA_ENTRY_SIZE, cq_err_msg, sizeof(cq_err_msg));
  if (!rdmaContext.write_recv_cq) {
    fprintf(stderr, "Failed to create RDMA receive completion queue: %s.\n", cq_err_msg);
    rdmaContextDestroy();
    return false;
  }

  rdmaContext.write_mem = rmem_create(RDMA_WRITE_ACK_BUFFER_SIZE);
  if (!rdmaContext.write_mem) {
    fprintf(stderr, "Failed to create RDMA memory region for acknowledgements (size: %zu).\n", RDMA_WRITE_ACK_BUFFER_SIZE);
    rdmaContextDestroy();
    return false;
  }

  rdmaContext.write_handler = rdmaio_reg_handler_create(rdmaContext.write_mem, rdmaContext.nic);
  if (!rdmaContext.write_handler) {
    fprintf(stderr, "Failed to create RDMA registration handler for acknowledgements.\n");
    rdmaContextDestroy();
    return false;
  }

  rdmaio_regattr_t write_reg_attr = rdmaio_reg_handler_get_attr(rdmaContext.write_handler);

  rdmaContext.write_allocator = simple_allocator_create(rdmaContext.write_mem, write_reg_attr.rkey);
  if (!rdmaContext.write_allocator) {
    fprintf(stderr, "Failed to create RDMA simple allocator for acknowledgements.\n");
    rdmaContextDestroy();
    return false;
  }

  if (!recv_manager_reg_recv_cq(rdmaContext.manager, RDMA_WRITE_ACK_CQ_NAME, rdmaContext.write_recv_cq, rdmaContext.write_allocator)) {
    fprintf(stderr, "Failed to register receive CQ with receive manager (CQ name: %s).\n", RDMA_WRITE_ACK_CQ_NAME);
    rdmaContextDestroy();
    return false;
  }

  if (!rdmaio_rctrl_register_mr(rdmaContext.ctrl, RDMA_REG_WRITE_ACK_MEM_NAME, rdmaContext.write_handler)) {
    fprintf(stderr, "Failed to register memory region for acknowledgements with control block (memory region name: %d).\n", RDMA_REG_WRITE_ACK_MEM_NAME);
    rdmaContextDestroy();
    return false;
  }

  // create write ack recv qp
  rdmaContext.read_recv_cq = rdmaio_create_cq(rdmaContext.nic, RDMA_ENTRY_SIZE, cq_err_msg, sizeof(cq_err_msg));
  if (!rdmaContext.read_recv_cq) {
    fprintf(stderr, "Failed to create RDMA receive completion queue: %s.\n", cq_err_msg);
    rdmaContextDestroy();
    return false;
  }

  rdmaContext.read_mem = rmem_create(RDMA_READ_ACK_BUFFER_SIZE);
  if (!rdmaContext.read_mem) {
    fprintf(stderr, "Failed to create RDMA memory region for acknowledgements (size: %zu).\n", RDMA_READ_ACK_BUFFER_SIZE);
    rdmaContextDestroy();
    return false;
  }

  rdmaContext.read_handler = rdmaio_reg_handler_create(rdmaContext.read_mem, rdmaContext.nic);
  if (!rdmaContext.read_handler) {
    fprintf(stderr, "Failed to create RDMA registration handler for acknowledgements.\n");
    rdmaContextDestroy();
    return false;
  }

  rdmaio_regattr_t read_reg_attr = rdmaio_reg_handler_get_attr(rdmaContext.read_handler);

  rdmaContext.read_allocator = simple_allocator_create(rdmaContext.read_mem, read_reg_attr.rkey);
  if (!rdmaContext.read_allocator) {
    fprintf(stderr, "Failed to create RDMA simple allocator for acknowledgements.\n");
    rdmaContextDestroy();
    return false;
  }

  if (!recv_manager_reg_recv_cq(rdmaContext.manager, RDMA_READ_ACK_CQ_NAME, rdmaContext.read_recv_cq, rdmaContext.read_allocator)) {
    fprintf(stderr, "Failed to register receive CQ with receive manager (CQ name: %s).\n", RDMA_READ_ACK_CQ_NAME);
    rdmaContextDestroy();
    return false;
  }

  if (!rdmaio_rctrl_register_mr(rdmaContext.ctrl, RDMA_REG_READ_ACK_MEM_NAME, rdmaContext.read_handler)) {
    fprintf(stderr, "Failed to register memory region for acknowledgements with control block (memory region name: %d).\n", RDMA_REG_READ_ACK_MEM_NAME);
    rdmaContextDestroy();
    return false;
  }

  if (!rctrl_start_daemon(rdmaContext.ctrl)) {
    fprintf(stderr, "Failed to start RDMA control daemon.\n");
    rdmaContextDestroy();
    return false;
  }

  rdmaContext.recv_qp = rctrl_query_qp(rdmaContext.ctrl, RDMA_SERVER_QP_NAME);
  int retry_count = 0;
  while (!rdmaContext.recv_qp) {
    sleep(1);
    rdmaContext.recv_qp = rctrl_query_qp(rdmaContext.ctrl, RDMA_SERVER_QP_NAME);
    ++retry_count;
  }

  rdmaContext.recv_rs = recv_manager_query_recv_entries(rdmaContext.manager, RDMA_SERVER_QP_NAME);
  int recv_rs_retry_count = 0;
  while (!rdmaContext.recv_rs) {
    sleep(1);
    rdmaContext.recv_rs = recv_manager_query_recv_entries(rdmaContext.manager, RDMA_SERVER_QP_NAME);
    ++recv_rs_retry_count;
  }

  rdmaContext.write_recv_qp = rctrl_query_qp(rdmaContext.ctrl, RDMA_WRITE_ACK_QP_NAME);
  int write_retry_count = 0;
  while (!rdmaContext.write_recv_qp) {
    sleep(1);
    rdmaContext.write_recv_qp = rctrl_query_qp(rdmaContext.ctrl, RDMA_WRITE_ACK_QP_NAME);
    ++write_retry_count;
  }

  rdmaContext.write_recv_rs = recv_manager_query_recv_entries(rdmaContext.manager, RDMA_WRITE_ACK_QP_NAME);
  int write_recv_rs_retry_count = 0;
  while (!rdmaContext.write_recv_rs) {
    sleep(1);
    rdmaContext.write_recv_rs = recv_manager_query_recv_entries(rdmaContext.manager, RDMA_WRITE_ACK_QP_NAME);
    ++write_recv_rs_retry_count;
  }

  rdmaContext.read_recv_qp = rctrl_query_qp(rdmaContext.ctrl, RDMA_READ_ACK_QP_NAME);
  int read_retry_count = 0;
  while (!rdmaContext.read_recv_qp) {
    sleep(1);
    rdmaContext.read_recv_qp = rctrl_query_qp(rdmaContext.ctrl, RDMA_READ_ACK_QP_NAME);
    ++read_retry_count;
  }

  rdmaContext.read_recv_rs = recv_manager_query_recv_entries(rdmaContext.manager, RDMA_READ_ACK_QP_NAME);
  int read_recv_rs_retry_count = 0;
  while (!rdmaContext.read_recv_rs) {
    sleep(1);
    rdmaContext.read_recv_rs = recv_manager_query_recv_entries(rdmaContext.manager, RDMA_READ_ACK_QP_NAME);
    ++read_recv_rs_retry_count;
  }

  // create hashtable
  hashTable = initHashTable();
  if (!hashTable) {
    fprintf(stderr, "Failed to create hash table.\n");
    rdmaContextDestroy();
    return false;
  }

  if(pthread_spin_init(&send_lock, PTHREAD_PROCESS_PRIVATE) != 0) {
    fprintf(stderr, "Failed to initialize send lock.\n");
    destroyHashTable(hashTable);
    rdmaContextDestroy();
    return false;
  }

  if (pthread_spin_init(&read_recv_lock, PTHREAD_PROCESS_PRIVATE) != 0) {
    fprintf(stderr, "Failed to initialize read recv lock.\n");
    destroyHashTable(hashTable);
    rdmaContextDestroy();
    return false;
  }

  initSendBuf();

  printf("RDMA context creation completed successfully.\n");
  return true;
}

void terminateRdmaContext() {
  printf("Starting RDMA context termination.\n");
  if (!rdmaContext.ctrl) {
    printf("RDMA context has already been destroyed.\n");
    return;
  }

  sendRdmaMessage(NULL, 0, RDMA_TERMINATE_OP);

  rdmaContextDestroy();
  printf("RDMA context termination completed.\n");
}

void rdmaInitMessage(char* message) {
  uint32_t reqId = getRequestId();
  RdmaInitPayload initPayload;
  initPayload.reqId = reqId;
  memcpy(initPayload.message, message, strlen(message) + 1);

  // register request
  ChainNode* node = insert(hashTable, reqId);
  if (!sendRdmaMessage((char*) &initPayload, sizeof(initPayload), reqId)) {
    // handle error
    deleteNode(hashTable, node);
  }

  // await ack and return result
  RdmaSyscallResponse response = awaitAcknowledgement(node);
  if (response.status == 0) {
    printf("RDMA Init Successful!\n");
    return;
  }

  fprintf(stderr, "Error validating RDMA init.\n");
  if (node) {
    deleteNode(hashTable, node);
  }
  free(hashTable);
}

// open a file on remote machine
RdmaSyscallResponse rdmaOpenFile(char* filename, int flags, mode_t mode) {
  int reqId = getRequestId();
  RdmaOpenFilePayload payload;
  payload.reqId = reqId;
  payload.flags = flags;
  payload.mode = mode;
  snprintf(payload.filename, MAX_FILE_NAME_SIZE, "%s", filename);

  ChainNode* node = insert(hashTable, reqId);
  if (!sendRdmaMessage((char*) &payload, sizeof(RdmaOpenFilePayload), RDMA_OPEN_FILE)) {
    // handle error
    deleteNode(hashTable, node);
    RdmaSyscallResponse response;
    response.status = -1;
    response.errnum = EAGAIN; // RDMA operation failed, not sure what error to provide
    return response;
  }

  // await ack and return result
  return awaitAcknowledgement(node);
}

RdmaSyscallResponse rdmaLseekFile(int fd, off_t position, int whence) {
  int reqId = getRequestId();
  RdmaLseekFilePayload payload;
  payload.reqId = reqId;
  payload.fd = fd;
  payload.position = position;
  payload.whence = whence;

  ChainNode* node = insert(hashTable, reqId);
  if (!sendRdmaMessage((char*) &payload, sizeof(RdmaLseekFilePayload), RDMA_LSEEK_FILE)) {
    deleteNode(hashTable, node);
    RdmaSyscallResponse response;
    response.status = -1;
    response.errnum = EAGAIN;
    return response;
  }

  // awair ack and return result
  return awaitAcknowledgement(node);
}

// close a file on remote machine
RdmaSyscallResponse rdmaCloseFile(int fd) {
  int reqId = getRequestId();
  RdmaCloseFilePayload payload;
  payload.reqId = reqId;
  payload.fd = fd;

  ChainNode* node = insert(hashTable, reqId);
  if (!sendRdmaMessage((char*) &payload, sizeof(RdmaCloseFilePayload), RDMA_CLOSE_FILE)) {
    // handle error
    deleteNode(hashTable, node);
    RdmaSyscallResponse response;
    response.status = -1;
    response.errnum = EAGAIN; // RDMA operation failed, not sure what error to provide
    return response;
  }

  // await ack and return result
  return awaitAcknowledgement(node);
}

// unlink file
RdmaSyscallResponse rdmaUnlinkFile(char* filename, OpType opType) {
  int reqId = getRequestId();
  RdmaUnlinkFilePayload payload;
  payload.reqId = reqId;
  payload.opType = opType;
  snprintf(payload.filename, MAX_FILE_NAME_SIZE, "%s", filename);

  ChainNode* node = insert(hashTable, reqId);
  if (!sendRdmaMessage((char*) &payload, sizeof(RdmaUnlinkFilePayload), RDMA_UNLINK_FILE)) {
    // handle error
    deleteNode(hashTable, node);
    RdmaSyscallResponse response;
    response.status = -1;
    response.errnum = EAGAIN; // RDMA operation failed, not sure what error to provide
    return response;
  }

  // await ack and send result
  return awaitAcknowledgement(node);
}

// rename file
RdmaSyscallResponse rdmaRenameFile(char* oldFilename, char* newFilename, OpType opType) {
  int reqId = getRequestId();
  RdmaRenameFilePayload payload;
  payload.reqId = reqId;
  payload.opType = opType;
  snprintf(payload.oldFilename, MAX_FILE_NAME_SIZE, "%s", oldFilename);
  snprintf(payload.newFilename, MAX_FILE_NAME_SIZE, "%s", newFilename);

  ChainNode* node = insert(hashTable, reqId);
  if (!sendRdmaMessage((char*) &payload, sizeof(RdmaRenameFilePayload), RDMA_RENAME_FILE)) {
    // handle error
    deleteNode(hashTable, node);
    RdmaSyscallResponse response;
    response.status = -1;
    response.errnum = EAGAIN; // RDMA operation failed, not sure what error to provide
    return response;
  }

  // await ack and return result
  return awaitAcknowledgement(node);
}

// rename file
RdmaSyscallResponse rdmaTruncateFile(char* filename, off_t size) {
  int reqId = getRequestId();
  RdmaTruncateFilePayload payload;
  payload.reqId = reqId;
  payload.offset = size;
  snprintf(payload.filename, MAX_FILE_NAME_SIZE, "%s", filename);

  ChainNode* node = insert(hashTable, reqId);
  if (!sendRdmaMessage((char*) &payload, sizeof(RdmaTruncateFilePayload), RDMA_TRUNCATE_FILE)) {
    // handle error
    deleteNode(hashTable, node);
    RdmaSyscallResponse response;
    response.status = -1;
    response.errnum = EAGAIN; // RDMA operation failed, not sure what error to provide
    return response;
  }

  // await ack and return result
  return awaitAcknowledgement(node);
}

RdmaSyscallResponse rdmaFtruncateFile(unsigned int fd, off_t size) {
  int reqId = getRequestId();
  RdmaFtruncateFilePayload payload;
  payload.reqId = reqId;
  payload.fd = fd;
  payload.offset = size;

  ChainNode* node = insert(hashTable, reqId);
  if (!sendRdmaMessage((char*) &payload, sizeof(RdmaFtruncateFilePayload), RDMA_FTRUNCATE_FILE)) {
    // handle error
    deleteNode(hashTable, node);
    RdmaSyscallResponse response;
    response.status = -1;
    response.errnum = EAGAIN;
    return response;
  }

  // await ack and return result
  return awaitAcknowledgement(node);
}

// stat call
RdmaSyscallResponse rdmaStatCall(char* path) {
  int reqId = getRequestId();
  RdmaStatPayload payload;
  payload.reqId = reqId;
  snprintf(payload.filename, MAX_FILE_NAME_SIZE, "%s", path);

  ChainNode* node = insert(hashTable, reqId);
  if (!sendRdmaMessage((char*) &payload, sizeof(RdmaStatPayload), RDMA_STAT_FILE)) {
    // handle error
    deleteNode(hashTable, node);
    RdmaSyscallResponse response;
    response.status = -1;
    response.errnum = EAGAIN; // RDMA operation failed, not sure what error to provide
    return response;
  }

  // await ack and return result
  return awaitAcknowledgement(node);
}

RdmaSyscallResponse rdmaFstatCall(unsigned int fd) {
  int reqId = getRequestId();
  RdmaFstatPayload payload;
  payload.reqId = reqId;
  payload.fd = fd;

  ChainNode* node = insert(hashTable, reqId);
  if (!sendRdmaMessage((char*) &payload, sizeof(RdmaFstatPayload), RDMA_FSTAT_FILE)) {
    // handle error
    deleteNode(hashTable, node);
    RdmaSyscallResponse response;
    response.status = -1;
    response.errnum = EAGAIN;
    return response;
  }

  // await ack and return result
  return awaitAcknowledgement(node);
}

ssize_t rdmaWalRead(unsigned int fd, off_t offset, size_t size, char* buffer) {
  unsigned int reqId = getRequestId();
  RdmaReadFilePayload payload;
  payload.reqId = reqId;
  payload.fd = fd;
  payload.offset = offset;
  payload.size = size;

  if (pthread_spin_lock(&read_recv_lock) != 0) {
    fprintf(stderr, "pthread_spin_lock acquire failed\n");
    return -1;
  }

  if (!sendRdmaMessage((char*) &payload, sizeof(RdmaReadFilePayload), RDMA_READ_FILE)) {
    return -1; // read failed
  }

  // trust in-order delivery of RDMA for now. Eventually make more robust
  bool end = false;
  bool error = false;
  ssize_t receivedSize = 0;
  while (!end) {
    // Iterate through all available messages in the current iterator.
    rdmaio_recv_iter_t* iter = rdmaio_recv_iter_create(rdmaContext.read_recv_qp, rdmaContext.read_recv_rs);
    while (rdmaio_recv_iter_has_msgs(iter)) {
      uint32_t imm_msg;
      uintptr_t buf_addr;
      if (rdmaio_recv_iter_cur_msg(iter, &imm_msg, &buf_addr)) {
        unsigned int recvReqId = (unsigned int) imm_msg;
        if (recvReqId == reqId) {
          // quick fix to prevent processing messages in case of error
          RdmaReadFileResponse* received_response = (RdmaReadFileResponse*)buf_addr;
          if (!error && received_response->status == 0) {
            receivedSize += received_response->size;
            memcpy(buffer + received_response->offset, received_response->buffer, received_response->size);
            end = received_response->end;
          } else {
            error = true;
            fprintf(stderr, "Read operation failed due to error %d\n", received_response->errnum);
          }
        } else {
          fprintf(stderr, "Error: expected request id %u but found %u\n", reqId, recvReqId);
          error = true;
        }
      } else {
        fprintf(stderr, "Error receiving message from RDMA iterator.\n");
        error = true;
      }
      rdmaio_recv_iter_next(iter);
    }
    rdmaio_recv_iter_destroy(iter);
  }

  if (pthread_spin_unlock(&read_recv_lock) != 0) {
    fprintf(stderr, "pthread_spin_lock release failed\n");
    return -1;
  }

  if (error) {
    return -1;
  }

  return receivedSize;
}

ssize_t rdmaWalWrite(const char* msg, unsigned int fd, lsn_t_c* lsn, off_t offset, size_t size, bool start, bool end) {
  if (size == 0) {
    return 0;
  }
  if (msg == NULL) {
    errno = EFAULT;
    return -1;
  }
  size_t total_bytes_requested = size; // Store the total size requested
  size_t bytes_sent_so_far = 0; // Track bytes successfully sent
  for (size_t diffFromOffset = 0; diffFromOffset < total_bytes_requested; diffFromOffset += MAX_WRITE_MESSAGE_SIZE) {
    // each chunk will hava a max size of 'MAX_WRITE_MESSGE_SIZE'
    size_t msgSize = MIN(MAX_WRITE_MESSAGE_SIZE, total_bytes_requested - diffFromOffset);
    RdmaWriteFilePayload payload;
    payload.lsn_offset = lsn_get_offset_c(lsn);
    payload.lsn_partition = lsn_get_partition_c(lsn);
    payload.fd = fd;
    payload.offset = offset + diffFromOffset;
    payload.size = msgSize;
    payload.start = (diffFromOffset == 0 && start);
    payload.end = (total_bytes_requested <= diffFromOffset + MAX_WRITE_MESSAGE_SIZE && end);
    memcpy(payload.data, msg + diffFromOffset, msgSize);

    if (!sendRdmaMessage((char*) &payload, sizeof(RdmaWriteFilePayload), RDMA_WRITE_FILE)) {
      errno = EAGAIN;
      return -1; // write failed
    }

    bytes_sent_so_far += msgSize;
  }

  // if all messages were sent, assume write success
  return total_bytes_requested;
}

int isRdmaFlushCompleted(lsn_t_c* lsn) {
  int ack = -1;
  while (ack != 0) {
    rdmaio_recv_iter_t* iter = rdmaio_recv_iter_create(rdmaContext.write_recv_qp, rdmaContext.write_recv_rs);
    if (!iter) {
      fprintf(stderr, "Error creating RDMA receive iterator.\n");
      break;
    }
    while (rdmaio_recv_iter_has_msgs(iter)) {
      uint32_t imm_msg;
      uintptr_t buf_addr;
      if (rdmaio_recv_iter_cur_msg(iter, &imm_msg, &buf_addr)) {
	    int status = (int) imm_msg;
	    RdmaFlushResponse* response = (RdmaFlushResponse*) buf_addr;
	    if (status == -1) {
	      // error - report failure!
	      rdmaio_recv_iter_destroy(iter);
	      return status;
	    } else {
	      // success - atomic update the last flushed LSN somehow [ignore for now, daemon is handling that]
	      if (lsn_is_greater_equal_c(lsn, response->lsn_partition, response->lsn_offset)) {
	        ack = 0; // we have found what we were looking for. Clear the current iterator and return
	      }
	    }
      } else {
	    fprintf(stderr, "Error receiving message from RDMA iterator.\n");
      }
      rdmaio_recv_iter_next(iter);
    }
    rdmaio_recv_iter_destroy(iter);
  }

  return ack;
}

c_list_handle rdmaGetDirContentsList(bool* error) {
  c_list_handle list = c_list_create();
  bool ack = false;
  while (!ack) {
    rdmaio_recv_iter_t* iter = rdmaio_recv_iter_create(rdmaContext.write_recv_qp, rdmaContext.write_recv_rs);
    if (!iter) {
      fprintf(stderr, "Error creating RDMA receive iterator.\n");
      break;
    }
    while (rdmaio_recv_iter_has_msgs(iter)) {
      uint32_t imm_msg;
      uintptr_t buf_addr;
      if (rdmaio_recv_iter_cur_msg(iter, &imm_msg, &buf_addr)) {
	    int status = (int) imm_msg;
        switch (status) {
          case 1:
          {
            // read the contents
            RdmaDirContents* contents = (RdmaDirContents*) buf_addr;
            if(c_list_add_back(list, contents) == -1) {
              // error, terminate loop
              *error = true;
              ack = true;
            }
            break;
          }
          case 0:
          {
            // last message
            ack = true; // breaks the loop
			break;
		  }
          case -1:
          {
            // error state, do something
            fprintf(stderr, "Error message from RDMA iterator.\n");
            *error = true;
            break;
          }
          default:
          {
            fprintf(stderr, "Invalid status from RDMA iterator: %d.\n", status);
            *error = true;
            ack = true;
            break;
	      }
        }
      } else {
        *error = true;
	    fprintf(stderr, "Error receiving message from RDMA iterator.\n");
        ack = true;
      }
      if (ack || error) {
        break;
      }
      rdmaio_recv_iter_next(iter);
    }
    rdmaio_recv_iter_destroy(iter);
  }

  if (error) {
    // iterate the list, remove all contents
    c_list_iterator_handle iter = c_list_begin(list);
    c_list_iterator_handle iterEnd = c_list_end(list);
    while (!c_list_iterator_is_equal(iter, iterEnd)) {
      c_list_erase(list, iter);
    }

    c_list_destroy_iterator(iter);
    c_list_destroy_iterator(iterEnd);
    c_list_destroy(list);
  }

  return list; // in case of error, client will ignore the return value.
}

// TODO: Separate ack queue for tracking write completionsi. [X] for now
//  OR - use one-sided RDMA from server to update the LSN in client.
// But then fsync? - it will wait on the variable somehow... but how?