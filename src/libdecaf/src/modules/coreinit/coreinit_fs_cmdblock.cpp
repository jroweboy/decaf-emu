#include "coreinit.h"
#include "coreinit_appio.h"
#include "coreinit_fs.h"
#include "coreinit_fs_client.h"
#include "coreinit_fs_driver.h"
#include "coreinit_fs_cmdblock.h"
#include "coreinit_fsa_shim.h"
#include "ppcutils/wfunc_call.h"

#include <common/align.h>
#include <common/log.h>
#include <libcpu/mem.h>

namespace coreinit
{

/**
 * Initialise an FSCmdBlock structure.
 */
void
FSInitCmdBlock(FSCmdBlock *block)
{
   if (!block) {
      return;
   }

   std::memset(block, 0, sizeof(FSCmdBlock));

   auto blockBody = internal::fsCmdBlockGetBody(block);
   blockBody->status = FSCmdBlockStatus::Initialised;
   blockBody->priority = 16;
}


/**
 * Get the value stored in FSCmdBlock by FSSetUserData.
 */
void *
FSGetUserData(FSCmdBlock *block)
{
   auto blockBody = internal::fsCmdBlockGetBody(block);
   return blockBody->userData;
}


/**
 * Store a user value in FSCmdBlock which can be retrieved by FSGetUserData.
 */
void
FSSetUserData(FSCmdBlock *block,
              void *userData)
{
   auto blockBody = internal::fsCmdBlockGetBody(block);
   blockBody->userData = userData;
}


namespace internal
{

FSFinishCmdFn
fsCmdBlockFinishCmdFn = nullptr;

FSFinishCmdFn
fsCmdBlockFinishReadCmdFn = nullptr;

static void
fsCmdBlockFinishCmd(FSCmdBlockBody *blockBody,
                    FSStatus status);

static void
fsCmdBlockFinishReadCmd(FSCmdBlockBody *blockBody,
                        FSStatus status);


/**
 * Get an aligned FSCmdBlockBody from an FSCmdBlock.
 */
FSCmdBlockBody *
fsCmdBlockGetBody(FSCmdBlock *cmdBlock)
{
   auto body = mem::translate<FSCmdBlockBody>(align_up(mem::untranslate(cmdBlock), 0x40));
   body->block = cmdBlock;
   return body;
}


/**
 * Prepare a FSCmdBlock for an asynchronous operation.
 *
 * \return
 * Returns a positive value on success, FSStatus error code otherwise.
 */
FSStatus
fsCmdBlockPrepareAsync(FSClientBody *clientBody,
                       FSCmdBlockBody *blockBody,
                       FSErrorFlag errorMask,
                       const FSAsyncData *asyncData)
{
   decaf_check(clientBody);
   decaf_check(blockBody);

   if (blockBody->status != FSCmdBlockStatus::Initialised && blockBody->status != FSCmdBlockStatus::Cancelled) {
      gLog->error("Invalid FSCmdBlockData state {}", blockBody->status.value());
      return FSStatus::FatalError;
   }

   if (asyncData->userCallback && asyncData->ioMsgQueue) {
      gLog->error("userCallback and ioMsgQueue are exclusive.");
      return FSStatus::FatalError;
   }

   blockBody->errorMask = errorMask;
   blockBody->clientBody = clientBody;
   return fsAsyncResultInit(clientBody, &blockBody->asyncResult, asyncData);
}


/**
 * Prepare a FSCmdBlock for a synchronous operation.
 */
void
fsCmdBlockPrepareSync(FSClient *client,
                      FSCmdBlock *block,
                      FSAsyncData *asyncData)
{
   auto clientBody = internal::fsClientGetBody(client);
   auto blockBody = internal::fsCmdBlockGetBody(block);
   OSInitMessageQueue(&blockBody->syncQueue, blockBody->syncQueueMsgs, 1);
   asyncData->ioMsgQueue = &blockBody->syncQueue;
}


/**
 * Requeues an FS command.
 */
void
fsCmdBlockRequeue(FSCmdQueue *queue,
                  FSCmdBlockBody *blockBody,
                  BOOL insertAtFront,
                  FSFinishCmdFn finishCmdFn)
{
   OSFastMutex_Lock(&queue->mutex);

   if (blockBody->cancelFlags & FSCmdCancelFlags::Cancelling) {
      blockBody->cancelFlags &= ~FSCmdCancelFlags::Cancelling;
      blockBody->status = FSCmdBlockStatus::Cancelled;
      blockBody->clientBody->lastDequeuedCommand = nullptr;
      OSFastMutex_Unlock(&queue->mutex);
      fsCmdBlockReplyResult(blockBody, FSStatus::Cancelled);
      return;
   }

   blockBody->finishCmdFn = finishCmdFn;
   blockBody->status = FSCmdBlockStatus::QueuedCommand;
   fsCmdQueueFinishCmd(queue);

   if (insertAtFront) {
      fsCmdQueuePushFront(queue, blockBody);
      OSFastMutex_Unlock(&queue->mutex);
   } else {
      fsCmdQueueEnqueue(queue, blockBody, true);
      OSFastMutex_Unlock(&queue->mutex);
   }

   fsCmdQueueProcessCmd(queue);
}


/**
 * Set the result for an FSCmd.
 *
 * A message will be sent to the user's ioMsgQueue if one was provided or to
 * the AppIO queue where the user's callback will be called instead.
 */
void
fsCmdBlockSetResult(FSCmdBlockBody *blockBody,
                    FSStatus status)
{
   blockBody->asyncResult.block = blockBody->block;
   blockBody->asyncResult.status = status;

   if (!OSSendMessage(blockBody->asyncResult.asyncData.ioMsgQueue,
                      reinterpret_cast<OSMessage *>(&blockBody->asyncResult.ioMsg),
                      OSMessageFlags::None)) {
      decaf_abort("fsCmdBlockReplyResult: Could not send async result message");
   }
}


/**
 * Calls the blockBody->finishCmdFn with the result of the command.
 */
void
fsCmdBlockReplyResult(FSCmdBlockBody *blockBody,
                      FSStatus status)
{
   if (!blockBody) {
      return;
   }

   // Finish the current command
   auto queue = &blockBody->clientBody->cmdQueue;
   OSFastMutex_Lock(&queue->mutex);
   fsCmdQueueFinishCmd(queue);
   OSFastMutex_Unlock(&queue->mutex);

   if (blockBody->finishCmdFn) {
      blockBody->finishCmdFn(blockBody, status);
   }

   // Start off next command
   fsCmdQueueProcessCmd(queue);
}


/**
 * Called from the AppIO thread to handle the result of an FS command.
 */
void
fsCmdBlockHandleResult(FSCmdBlockBody *blockBody)
{
   auto clientBody = blockBody->clientBody;
   auto result = static_cast<FSStatus>(blockBody->fsaStatus.value());
   auto errorFlags = FSErrorFlag::All;

   if (!fsClientRegistered(clientBody)) {
      if (blockBody->finishCmdFn) {
         blockBody->finishCmdFn(blockBody, FSStatus::Cancelled);
      }

      return;
   }

   clientBody->lastError = blockBody->fsaStatus;

   if (blockBody->fsaStatus == FSAStatus::MediaNotReady) {
      fsmSetState(&clientBody->fsm, FSVolumeState::WrongMedia, clientBody);
      return;
   } else if (blockBody->fsaStatus == FSAStatus::WriteProtected) {
      fsmSetState(&clientBody->fsm, FSVolumeState::MediaError, clientBody);
      return;
   }

   if (blockBody->fsaStatus < 0) {
      switch (blockBody->fsaStatus) {
      case FSAStatus::NotInit:
      case FSAStatus::OutOfRange:
      case FSAStatus::OutOfResources:
      case FSAStatus::LinkEntry:
      case FSAStatus::UnavailableCmd:
      case FSAStatus::InvalidParam:
      case FSAStatus::InvalidPath:
      case FSAStatus::InvalidBuffer:
      case FSAStatus::InvalidAlignment:
      case FSAStatus::InvalidClientHandle:
      case FSAStatus::InvalidFileHandle:
      case FSAStatus::InvalidDirHandle:
         errorFlags = FSErrorFlag::None;
         break;
      case FSAStatus::Busy:
         fsCmdBlockRequeue(&clientBody->cmdQueue, blockBody, TRUE, blockBody->finishCmdFn);
         return;
      case FSAStatus::Cancelled:
         result = FSStatus::Cancelled;
         break;
      case FSAStatus::EndOfDir:
         result = FSStatus::End;
         break;
      case FSAStatus::EndOfFile:
         result = FSStatus::End;
         break;
      case FSAStatus::MaxMountpoints:
      case FSAStatus::MaxVolumes:
      case FSAStatus::MaxClients:
      case FSAStatus::MaxFiles:
      case FSAStatus::MaxDirs:
         errorFlags = FSErrorFlag::Max;
         result = FSStatus::Max;
         break;
      case FSAStatus::AlreadyOpen:
         errorFlags = FSErrorFlag::AlreadyOpen;
         result = FSStatus::AlreadyOpen;
         break;
      case FSAStatus::NotFound:
         errorFlags = FSErrorFlag::NotFound;
         result = FSStatus::NotFound;
         break;
      case FSAStatus::AlreadyExists:
      case FSAStatus::NotEmpty:
         errorFlags = FSErrorFlag::Exists;
         result = FSStatus::Exists;
         break;
      case FSAStatus::AccessError:
         errorFlags = FSErrorFlag::AccessError;
         result = FSStatus::AccessError;
         break;
      case FSAStatus::PermissionError:
         errorFlags = FSErrorFlag::PermissionError;
         result = FSStatus::PermissionError;
         break;
      case FSAStatus::DataCorrupted:
         // TODO: FSAStatus::DataCorrupted
         decaf_abort("TODO: Reverse me.");
         break;
      case FSAStatus::StorageFull:
         errorFlags = FSErrorFlag::StorageFull;
         result = FSStatus::StorageFull;
         break;
      case FSAStatus::JournalFull:
         errorFlags = FSErrorFlag::JournalFull;
         result = FSStatus::JournalFull;
         break;
      case FSAStatus::UnsupportedCmd:
         errorFlags = FSErrorFlag::UnsupportedCmd;
         result = FSStatus::UnsupportedCmd;
         break;
      case FSAStatus::NotFile:
         errorFlags = FSErrorFlag::NotFile;
         result = FSStatus::NotFile;
         break;
      case FSAStatus::NotDir:
         errorFlags = FSErrorFlag::NotDir;
         result = FSStatus::NotDirectory;
         break;
      case FSAStatus::FileTooBig:
         errorFlags = FSErrorFlag::FileTooBig;
         result = FSStatus::FileTooBig;
         break;
      case FSAStatus::MediaError:
         // TODO: FSAStatus::MediaError
         decaf_abort("TODO: Reverse me.");
         break;
      case FSAStatus::InvalidMedia:
         errorFlags = FSErrorFlag::None;
         return;
      }

      if (blockBody->errorMask & errorFlags) {
         fsmEnterState(&clientBody->fsm, FSVolumeState::Fatal, clientBody);
         return;
      }
   }

   if (clientBody->lastDequeuedCommand == blockBody) {
      clientBody->lastDequeuedCommand = nullptr;
   }

   fsCmdBlockReplyResult(blockBody, result);
}


/**
 * Copies the IOS command results to FS output.
 *
 * Set as blockBlody->finishCmdFn.
 * Called from fsCmdBlockReplyResult.
 */
void
fsCmdBlockFinishCmd(FSCmdBlockBody *blockBody,
                    FSStatus result)
{
   auto clientBody = blockBody->clientBody;

   OSFastMutex_Lock(&clientBody->mutex);
   blockBody->cancelFlags &= ~FSCmdCancelFlags::Cancelling;

   if (clientBody->lastDequeuedCommand == blockBody) {
      clientBody->lastDequeuedCommand = nullptr;
   }

   blockBody->status = FSCmdBlockStatus::Cancelled;
   OSFastMutex_Unlock(&clientBody->mutex);

   if (result < 0) {
      fsCmdBlockSetResult(blockBody, result);
      return;
   }

   blockBody->unk0x9EA = 0;
   blockBody->unk0x9F4 = 0;

   auto shim = &blockBody->fsaShimBuffer;

   switch (shim->command) {
   case FSACommand::Mount:
   case FSACommand::Unmount:
   case FSACommand::ChangeDir:
   case FSACommand::MakeDir:
   case FSACommand::Remove:
   case FSACommand::Rename:
   case FSACommand::RewindDir:
   case FSACommand::CloseDir:
   case FSACommand::ReadFile:
   case FSACommand::WriteFile:
   case FSACommand::SetPosFile:
   case FSACommand::IsEof:
   case FSACommand::CloseFile:
   case FSACommand::GetError:
   case FSACommand::FlushFile:
   case FSACommand::AppendFile:
   case FSACommand::TruncateFile:
   case FSACommand::MakeQuota:
   case FSACommand::FlushQuota:
   case FSACommand::RollbackQuota:
   case FSACommand::ChangeMode:
   case FSACommand::RegisterFlushQuota:
   case FSACommand::FlushMultiQuota:
   case FSACommand::RemoveQuota:
   case FSACommand::MakeLink:
      fsCmdBlockSetResult(blockBody, result);
      break;
   case FSACommand::GetVolumeInfo:
   {
      auto info = blockBody->cmdData.getVolumeInfo.info;
      std::memmove(info, &shim->response.getVolumeInfo.volumeInfo, sizeof(FSAVolumeInfo));
      info->unk0x0C = 0;
      info->unk0x10 = 0;
      info->unk0x14 = -1;
      info->unk0x18 = -1;
      fsCmdBlockSetResult(blockBody, result);
      break;
   }
   case FSACommand::OpenDir:
   {
      auto handle = blockBody->cmdData.openDir.handle;
      *handle = shim->response.openDir.handle;
      fsCmdBlockSetResult(blockBody, result);
      break;
   }
   case FSACommand::ReadDir:
   {
      auto entry = blockBody->cmdData.readDir.entry;
      std::memmove(entry, &shim->response.readDir.entry, sizeof(FSDirEntry));
      fsCmdBlockSetResult(blockBody, result);
      break;
   }
   case FSACommand::OpenFile:
   case FSACommand::OpenFileByStat:
   {
      *blockBody->cmdData.openFile.handle = shim->response.openFile.handle;
      fsCmdBlockSetResult(blockBody, result);
      break;
   }
   case FSACommand::GetPosFile:
   {
      auto pos = blockBody->cmdData.getPosFile.pos;
      *pos = shim->response.getPosFile.pos;
      fsCmdBlockSetResult(blockBody, result);
      break;
   }
   case FSACommand::StatFile:
   {
      auto stat = blockBody->cmdData.statFile.stat;
      std::memmove(stat, &shim->response.statFile.stat, sizeof(FSStat));
      fsCmdBlockSetResult(blockBody, result);
      break;
   }
   case FSACommand::GetFileBlockAddress:
   {
      auto address = blockBody->cmdData.getFileBlockAddress.address;

      if (address) {
         *address = shim->response.getFileBlockAddress.address;
      }

      fsCmdBlockSetResult(blockBody, result);
      break;
   }
   case FSACommand::GetCwd:
   {
      auto bytes = blockBody->cmdData.getCwd.bytes;
      auto returnedPath = blockBody->cmdData.getCwd.returnedPath;

      if (bytes) {
         auto len = static_cast<uint32_t>(strlen(shim->response.getCwd.path));
         decaf_check(len < bytes);
         std::strncpy(returnedPath, shim->response.getCwd.path, bytes);
         std::memset(returnedPath + len, 0, bytes - len);
      }

      fsCmdBlockSetResult(blockBody, result);
      break;
   }
   case FSACommand::GetInfoByQuery:
      // TODO: Handle FSACommand::GetInfoByQuery
      decaf_abort("TODO: Reverse me.");
   default:
      decaf_abort(fmt::format("Invalid FSA command {}", shim->command));
   }
}

/**
 * Finish a FSACommand::ReadFile command.
 *
 * Files are read in chunk of up to FSMaxBytesPerRequest bytes per time, this
 * finish function will keep requeuing the command until we have completed
 * the full read.
 */
void
fsCmdBlockFinishReadCmd(FSCmdBlockBody *blockBody,
                        FSStatus result)
{
   auto bytesRead = static_cast<uint32_t>(result);

   if (result < 0) {
      return fsCmdBlockFinishCmd(blockBody, result);
   }

   // Update read state
   auto readState = &blockBody->cmdData.readFile;
   readState->bytesRead += bytesRead;
   readState->bytesRemaining -= bytesRead;

   // Check if the read is complete
   if (readState->bytesRemaining == 0 || bytesRead < readState->readSize) {
      auto chunksRead = readState->bytesRead / readState->chunkSize;
      return fsCmdBlockFinishCmd(blockBody, static_cast<FSStatus>(chunksRead));
   }

   // Check if we can read the final chunk yet
   if (readState->bytesRemaining > FSMaxBytesPerRequest) {
      readState->readSize = FSMaxBytesPerRequest;
   } else {
      readState->readSize = readState->bytesRemaining;
   }

   // Queue a new read request
   auto readRequest = &blockBody->fsaShimBuffer.request.readFile;
   readRequest->buffer = readRequest->buffer + bytesRead;
   readRequest->size = readState->readSize;
   readRequest->count = 1;

   if (readRequest->readFlags & FSReadFlag::ReadWithPos) {
      readRequest->pos += bytesRead;
   }

   auto shim = &blockBody->fsaShimBuffer;
   shim->ioctlvVec[1].paddr = readRequest->buffer;
   shim->ioctlvVec[1].len = readRequest->size;

   fsCmdBlockRequeue(&blockBody->clientBody->cmdQueue, blockBody, FALSE, fsCmdBlockFinishReadCmdFn);
}

} // namespace internal

void
Module::registerFsCmdBlockFunctions()
{
   RegisterKernelFunction(FSInitCmdBlock);
   RegisterKernelFunction(FSGetUserData);
   RegisterKernelFunction(FSSetUserData);

   RegisterInternalFunction(internal::fsCmdBlockFinishCmd, internal::fsCmdBlockFinishCmdFn);
   RegisterInternalFunction(internal::fsCmdBlockFinishReadCmd, internal::fsCmdBlockFinishReadCmdFn);
}

} // namespace coreinit
