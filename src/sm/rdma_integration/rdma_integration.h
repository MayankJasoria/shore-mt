#ifndef RDMA_INTEGRATION_H
#define RDMA_INTEGRATION_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/stat.h>

#include "rdma_structs.h"

#include "lsn_t_c.h"
#include "c_list.h"


#ifdef __cplusplus
extern "C" {
#endif

// Initialize the RDMA subsystem (false if init fails)
bool rdmaContextCreate();

// cleanup RDMA context
void terminateRdmaContext();

void rdmaInitMessage(char* message);

// open a file on remote machine
RdmaSyscallResponse rdmaOpenFile(char* filename, int flags, mode_t mode);

RdmaSyscallResponse rdmaLseekFile(int fd, off_t position, int whence);

// close a file on remote machine
RdmaSyscallResponse rdmaCloseFile(int fd);

// unlink file
RdmaSyscallResponse rdmaUnlinkFile(char* filename, OpType opType);

// rename file
RdmaSyscallResponse rdmaRenameFile(char* oldFilename, char* newFilename, OpType opType);

RdmaSyscallResponse rdmaTruncateFile(char* filename, off_t size);

RdmaSyscallResponse rdmaFtruncateFile(unsigned int fd, off_t size);

RdmaSyscallResponse rdmaStatCall(char* path);

RdmaSyscallResponse rdmaFstatCall(unsigned int fd);

ssize_t rdmaWalWrite(const char* msg, unsigned int fd, lsn_t_c* lsn, off_t offset, size_t size, bool start, bool end);

ssize_t rdmaWalRead(unsigned int fd, off_t offset, size_t size, char* buffer);

// check if rdma write for a specific lsn is completed (i.e. durably saved in remote disk)
int isRdmaFlushCompleted(lsn_t_c* lsn);

c_list_handle rdmaGetDirContentsList(bool* error);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // RDMA_INTEGRATION_H