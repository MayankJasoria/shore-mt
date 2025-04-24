#ifndef QUEUE_H
#define QUEUE_H

#include "common.h"

typedef struct queue Queue;
typedef struct operationLog {
    int messageType;
    union {
        RdmaOpenFilePayload openFilePayload;
        RdmaLseekFilePayload lseekFilePayload;
        RdmaCloseFilePayload closeFilePayload;
        RdmaUnlinkFilePayload unlinkFilePayload;
        RdmaRenameFilePayload renameFilePayload;
        RdmaTruncateFilePayload truncateFilePayload;
        RdmaFtruncateFilePayload ftruncateFilePayload;
        RdmaStatPayload statPayload;
        RdmaFstatPayload fstatPayload;
        RdmaInitPayload initPayload;
    };
} OperationLog;

Queue* queueInit();

void enqueue(Queue* queue, OperationLog* log);

OperationLog* dequeue(Queue* queue);

void destroyQueue(Queue* queue);

#endif //QUEUE_H
