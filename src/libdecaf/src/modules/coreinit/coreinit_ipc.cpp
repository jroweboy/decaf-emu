#include "coreinit.h"
#include "coreinit_core.h"
#include "coreinit_ipc.h"
#include "kernel/kernel_ipc.h"
#include "ppcutils/wfunc_call.h"

#include <array>

namespace coreinit
{

struct IpcData
{
   std::array<IPCBuffer, IPCBufferCount * CoreCount> ipcBuffers;
   std::array<IPCDriver, CoreCount> drivers;
};

static IpcData *
sIpcData = nullptr;


/**
 * Initialise the IPC driver.
 */
void
IPCDriverInit()
{
   auto coreId = OSGetCoreId();
   auto driver = &sIpcData->drivers[coreId];

   OSInitEvent(&driver->waitFreeFifoEvent, FALSE, OSEventMode::AutoReset);
   driver->status = IPCDriverStatus::Initialised;
   driver->coreId = coreId;
   driver->ipcBuffers = &sIpcData->ipcBuffers[IPCBufferCount * coreId];
}


/**
 * Open the IPC driver.
 *
 * \retval IOSError::OK
 * Success.
 *
 * \retval IOSError::NotReady
 * The IPC driver status must be Closed or Initialised.
 */
IOSError
IPCDriverOpen()
{
   auto driver = internal::getIPCDriver();
   
   // Verify driver state
   if (driver->status != IPCDriverStatus::Closed &&
       driver->status != IPCDriverStatus::Initialised) {
      return IOSError::NotReady;
   }

   // Initialise requests
   for (auto i = 0u; i < IPCBufferCount; ++i) {
      auto buffer = &driver->ipcBuffers[i];
      auto request = &driver->requests[i];
      request->ipcBuffer = buffer;
      request->asyncCallback = nullptr;
      request->asyncContext = nullptr;
   }

   driver->initialisedRequests = TRUE;

   // Initialise FIFO
   internal::ipcDriverFifoInit(&driver->freeFifo);
   internal::ipcDriverFifoInit(&driver->outboundFifo);

   // Push all items into free queue
   for (auto i = 0u; i < IPCBufferCount; ++i) {
      internal::ipcDriverFifoPush(&driver->freeFifo, &driver->requests[i]);
   }

   return IOSError::OK;
}


/**
 * Close the IPC driver.
 *
 * \retval IOSError::OK
 * Success.
 */
IOSError
IPCDriverClose()
{
   auto driver = internal::getIPCDriver();
   driver->status = IPCDriverStatus::Closed;
   return IOSError::OK;
}


namespace internal
{

/**
 * Get the IPC driver for the current core
 */
IPCDriver *
getIPCDriver()
{
   auto coreId = OSGetCoreId();
   return &sIpcData->drivers[coreId];
}


/**
 * Initialise IPCDriverFIFO
 */
void
ipcDriverFifoInit(IPCDriverFIFO *fifo)
{
   fifo->pushIndex = 0;
   fifo->popIndex = -1;
   fifo->count = 0;
   fifo->maxCount = 0;

   for (auto i = 0u; i < IPCBufferCount; ++i) {
      fifo->requests[i] = nullptr;
   }
}


/**
 * Push a request into an IPCDriverFIFO structure
 *
 * \retval IOSError::OK Success
 * \retval IOSError::QFull There was no free space in the queue to push the request.
 */
IOSError
ipcDriverFifoPush(IPCDriverFIFO *fifo,
                  IPCDriverRequest *request)
{
   if (fifo->pushIndex == fifo->popIndex) {
      return IOSError::QFull;
   }

   fifo->requests[fifo->pushIndex] = request;

   if (fifo->popIndex == -1) {
      fifo->popIndex = fifo->pushIndex;
   }

   fifo->count += 1;
   fifo->pushIndex = (fifo->pushIndex + 1) % IPCBufferCount;

   if (fifo->count > fifo->maxCount) {
      fifo->maxCount = fifo->count;
   }

   return IOSError::OK;
}


/**
 * Pop a request into an IPCDriverFIFO structure.
 *
 * \retval IOSError::OK
 * Success
 *
 * \retval IOSError::QEmpty
 * There was no requests to pop from the queue.
 */
IOSError
ipcDriverFifoPop(IPCDriverFIFO *fifo,
                 IPCDriverRequest **requestOut)
{
   if (fifo->popIndex == -1) {
      return IOSError::QEmpty;
   }

   auto request = fifo->requests[fifo->popIndex];
   fifo->count -= 1;

   if (fifo->count == 0) {
      fifo->popIndex = -1;
   } else {
      fifo->popIndex = (fifo->popIndex + 1) % IPCBufferCount;
   }

   *requestOut = request;
   return IOSError::OK;
}


/**
 * Allocates and initialises a IPCDriverRequest.
 *
 * This function can block with OSWaitEvent until there is a free request to
 * pop from the freeFifo queue.
 *
 * \return
 * Returns IOSError::OK on success, an IOSError code otherwise.
 */
IOSError
ipcDriverAllocateRequest(IPCDriver *driver,
                         IPCDriverRequest **requestOut,
                         IOSHandle handle,
                         IOSCommand command,
                         uint32_t requestUnk0x04,
                         IOSAsyncCallbackFn asyncCallback,
                         void *asyncContext)
{
   IPCDriverRequest *request = nullptr;
   auto error = IOSError::OK;

   do {
      error = ipcDriverFifoPop(&driver->freeFifo, &request);

      if (error) {
         driver->failedAllocateRequestBlock += 1;

         if (error == IOSError::QEmpty) {
            driver->waitingFreeFifo = TRUE;
            OSWaitEvent(&driver->waitFreeFifoEvent);
         }
      }
   } while (error == IOSError::QEmpty);

   if (error != IOSError::OK) {
      return error;
   }

   auto ipcBuffer = request->ipcBuffer;
   memset(ipcBuffer->args, 0, sizeof(uint32_t) * IPCBuffer::ArgCount);

   request->allocated = TRUE;
   request->unk0x04 = requestUnk0x04;
   request->asyncCallback = asyncCallback;
   request->asyncContext = asyncContext;

   ipcBuffer->command = command;
   ipcBuffer->handle = handle;
   ipcBuffer->flags = 0;
   ipcBuffer->processId = 0;
   ipcBuffer->reply = IOSError::OK;

   *requestOut = request;
   return IOSError::OK;
}


/**
 * Free a IPCDriverRequest.
 *
 * \retval IOSError::OK
 * Success.
 *
 * \retval IOSError::QFull
 * The driver's freeFifo queue was full thus we were unable to free the request.
 */
IOSError
ipcDriverFreeRequest(IPCDriver *driver,
                     IPCDriverRequest *request)
{
   auto error = ipcDriverFifoPush(&driver->freeFifo, request);
   request->allocated = FALSE;

   if (error != IOSError::OK) {
      driver->failedFreeRequestBlock += 1;
   }

   return error;
}


/**
 * Submits an IPCDriverRequest to the kernel IPC driver.
 *
 * \retval IOSError::OK
 * Success.
 */
IOSError
ipcDriverSubmitRequest(IPCDriver *driver,
                       IPCDriverRequest *request)
{
   OSInitEvent(&request->finishEvent, FALSE, OSEventMode::AutoReset);
   driver->requestsSubmitted++;
   kernel::ipcDriverKernelSubmitRequest(request->ipcBuffer);
   return IOSError::OK;
}


/**
 * Blocks and waits for a response to an IPCDriverRequest.
 *
 * \return
 * Returns IOSError::OK or an IOSHandle on success, or an IOSError code otherwise.
 */
IOSError
ipcDriverWaitResponse(IPCDriver *ipcDriver,
                      IPCDriverRequest *ipcRequest)
{
   auto reply = IOSError::OK;
   OSWaitEvent(&ipcRequest->finishEvent);
   reply = ipcRequest->ipcBuffer->reply;
   ipcDriverFreeRequest(ipcDriver, ipcRequest);
   OSSignalEventAll(&ipcDriver->waitFreeFifoEvent);
   return reply;
}


/**
 * Called by kernel IPC driver to indicate there are pending responses to process.
 */
void
ipcDriverProcessResponses()
{
   auto driver = getIPCDriver();

   for (auto i = 0u; i < driver->numResponses; ++i) {
      auto buffer = driver->responses[i];
      auto index = buffer.get() - driver->ipcBuffers.get();
      decaf_check(index >= 0);
      decaf_check(index <= IPCBufferCount);

      auto request = &driver->requests[index];
      decaf_check(request->ipcBuffer == buffer);

      if (!request->asyncCallback) {
         OSSignalEvent(&request->finishEvent);
      } else {
         request->asyncCallback(request->ipcBuffer->reply, request->asyncContext);
         ipcDriverFreeRequest(driver, request);
      }

      driver->requestsProcessed++;
      driver->responses[i] = nullptr;
   }

   driver->numResponses = 0;
}

} // namespace internal

void
Module::registerIpcFunctions()
{
   RegisterKernelFunction(IPCDriverInit);
   RegisterKernelFunction(IPCDriverOpen);
   RegisterKernelFunction(IPCDriverClose);
   RegisterInternalData(sIpcData);
}

} // namespace coreinit
