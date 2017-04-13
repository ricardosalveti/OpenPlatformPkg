/** @file

  Copyright (c) 2014, ARM Ltd. All rights reserved.<BR>
  Copyright (c) 2015-2017, Linaro. All rights reserved.

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

/*
  Implementation of the Android Fastboot Platform protocol, to be used by the
  Fastboot UEFI application, for Hisilicon HiKey platform.
*/

#include <Protocol/AndroidFastbootPlatform.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DiskIo.h>
#include <Protocol/EraseBlock.h>
#include <Protocol/SimpleTextOut.h>

#include <Protocol/DevicePathToText.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>

#include <Guid/HiKey960Variable.h>

#define FLASH_DEVICE_PATH_SIZE(DevPath) ( GetDevicePathSize (DevPath) - \
                                            sizeof (EFI_DEVICE_PATH_PROTOCOL))

#define PARTITION_NAME_MAX_LENGTH 72/2

#define IS_ALPHA(Char) (((Char) <= L'z' && (Char) >= L'a') || \
                        ((Char) <= L'Z' && (Char) >= L'Z'))
#define IS_HEXCHAR(Char) (((Char) <= L'9' && (Char) >= L'0') || \
                          IS_ALPHA(Char))

#define SERIAL_NUMBER_LENGTH      16
#define BOOT_DEVICE_LENGTH        16

#define HIKEY_ERASE_SIZE          (16 * 1024 * 1024)
#define HIKEY_ERASE_BLOCKS        (HIKEY_ERASE_SIZE / EFI_PAGE_SIZE)

typedef struct _FASTBOOT_PARTITION_LIST {
  LIST_ENTRY  Link;
  CHAR16      PartitionName[PARTITION_NAME_MAX_LENGTH];
  EFI_HANDLE  PartitionHandle;
  EFI_LBA     Lba;
} FASTBOOT_PARTITION_LIST;

STATIC LIST_ENTRY mPartitionListHead;

STATIC EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *mTextOut;

/*
  Helper to free the partition list
*/
STATIC
VOID
FreePartitionList (
  VOID
  )
{
  FASTBOOT_PARTITION_LIST *Entry;
  FASTBOOT_PARTITION_LIST *NextEntry;

  Entry = (FASTBOOT_PARTITION_LIST *) GetFirstNode (&mPartitionListHead);
  while (!IsNull (&mPartitionListHead, &Entry->Link)) {
    NextEntry = (FASTBOOT_PARTITION_LIST *) GetNextNode (&mPartitionListHead, &Entry->Link);

    RemoveEntryList (&Entry->Link);
    FreePool (Entry);

    Entry = NextEntry;
  }
}
/*
  Read the PartitionName fields from the GPT partition entries, putting them
  into an allocated array that should later be freed.
*/
STATIC
EFI_STATUS
ReadPartitionEntries (
  IN  EFI_BLOCK_IO_PROTOCOL *BlockIo,
  OUT EFI_PARTITION_ENTRY  **PartitionEntries
  )
{
  UINT32                      MediaId;
  EFI_PARTITION_TABLE_HEADER *GptHeader;
  EFI_STATUS                  Status;
  VOID                       *Buffer;
  UINTN                       PageCount;
  UINTN                       BlockSize;

  MediaId = BlockIo->Media->MediaId;
  BlockSize = BlockIo->Media->BlockSize;

  //
  // Read size of Partition entry and number of entries from GPT header
  //

  PageCount = EFI_SIZE_TO_PAGES (6 * BlockSize);
  Buffer = AllocatePages (PageCount);
  if (Buffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = BlockIo->ReadBlocks (BlockIo, MediaId, 0, PageCount * EFI_PAGE_SIZE, Buffer);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  GptHeader = (EFI_PARTITION_TABLE_HEADER *)(Buffer + BlockSize);

  // Check there is a GPT on the media
  if (GptHeader->Header.Signature != EFI_PTAB_HEADER_ID ||
      GptHeader->MyLBA != 1) {
    DEBUG ((DEBUG_ERROR,
      "Fastboot platform: No GPT on flash. "
      "Fastboot on Versatile Express does not support MBR.\n"
      ));
    return EFI_DEVICE_ERROR;
  }

  *PartitionEntries = (EFI_PARTITION_ENTRY *)((UINTN)Buffer + (2 * BlockSize));
  return EFI_SUCCESS;
}

EFI_STATUS
LoadPtable (
  VOID
  )
{
  EFI_STATUS                          Status;
  EFI_DEVICE_PATH_PROTOCOL           *FlashDevicePath;
  EFI_DEVICE_PATH_PROTOCOL           *FlashDevicePathDup;
  EFI_DEVICE_PATH_PROTOCOL           *DevicePath;
  EFI_DEVICE_PATH_PROTOCOL           *NextNode;
  HARDDRIVE_DEVICE_PATH              *PartitionNode;
  UINTN                               NumHandles;
  EFI_HANDLE                         *AllHandles;
  UINTN                               LoopIndex;
  EFI_HANDLE                          FlashHandle;
  EFI_BLOCK_IO_PROTOCOL              *FlashBlockIo;
  EFI_PARTITION_ENTRY                *PartitionEntries;
  FASTBOOT_PARTITION_LIST            *Entry;

  InitializeListHead (&mPartitionListHead);

  Status = gBS->LocateProtocol (&gEfiSimpleTextOutProtocolGuid, NULL, (VOID **) &mTextOut);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR,
      "Fastboot platform: Couldn't open Text Output Protocol: %r\n", Status
      ));
    return Status;
  }

  //
  // Get EFI_HANDLES for all the partitions on the block devices pointed to by
  // PcdFastbootFlashDevicePath, also saving their GPT partition labels.
  // There's no way to find all of a device's children, so we get every handle
  // in the system supporting EFI_BLOCK_IO_PROTOCOL and then filter out ones
  // that don't represent partitions on the flash device.
  //

  FlashDevicePath = ConvertTextToDevicePath ((CHAR16*)FixedPcdGetPtr (PcdAndroidFastbootNvmDevicePath));

  //
  // Open the Disk IO protocol on the flash device - this will be used to read
  // partition names out of the GPT entries
  //
  // Create another device path pointer because LocateDevicePath will modify it.
  FlashDevicePathDup = FlashDevicePath;
  Status = gBS->LocateDevicePath (&gEfiBlockIoProtocolGuid, &FlashDevicePathDup, &FlashHandle);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Warning: Couldn't locate Android NVM device (status: %r)\n", Status));
    // Failing to locate partitions should not prevent to do other Android FastBoot actions
    return EFI_SUCCESS;
  }

  Status = gBS->OpenProtocol (
                  FlashHandle,
                  &gEfiBlockIoProtocolGuid,
                  (VOID **) &FlashBlockIo,
                  gImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Fastboot platform: Couldn't open Android NVM device (status: %r)\n", Status));
    return EFI_DEVICE_ERROR;
  }

  // Read the GPT partition entry array into memory so we can get the partition names
  Status = ReadPartitionEntries (FlashBlockIo, &PartitionEntries);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Warning: Failed to read partitions from Android NVM device (status: %r)\n", Status));
    // Failing to locate partitions should not prevent to do other Android FastBoot actions
    return EFI_SUCCESS;
  }

  // Get every Block IO protocol instance installed in the system
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiBlockIoProtocolGuid,
                  NULL,
                  &NumHandles,
                  &AllHandles
                  );
  ASSERT_EFI_ERROR (Status);

  // Filter out handles that aren't children of the flash device
  for (LoopIndex = 0; LoopIndex < NumHandles; LoopIndex++) {
    // Get the device path for the handle
    Status = gBS->OpenProtocol (
                    AllHandles[LoopIndex],
                    &gEfiDevicePathProtocolGuid,
                    (VOID **) &DevicePath,
                    gImageHandle,
                    NULL,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL
                    );
    ASSERT_EFI_ERROR (Status);

    // Check if it is a sub-device of the flash device
    if (!CompareMem (DevicePath, FlashDevicePath, FLASH_DEVICE_PATH_SIZE (FlashDevicePath))) {
      // Device path is VenHw()/UFS()/HD(). Skip the first two level.
      NextNode = NextDevicePathNode (DevicePath);
      if (IsDevicePathEndType (NextNode)) {
        continue;
      }
      NextNode = NextDevicePathNode (NextNode);
      if (IsDevicePathEndType (NextNode)) {
        continue;
      }

      PartitionNode = (HARDDRIVE_DEVICE_PATH *) NextNode;

      // The firmware may install a handle for "partition 0", representing the
      // whole device. Ignore it.
      if ((PartitionNode->PartitionNumber == 0) || (PartitionNode->PartitionNumber > 128)) {
        continue;
      }

      //
      // Add the partition handle to the list
      //

      // Create entry
      Entry = AllocatePool (sizeof (FASTBOOT_PARTITION_LIST));
      if (Entry == NULL) {
        Status = EFI_OUT_OF_RESOURCES;
        FreePartitionList ();
        goto Exit;
      }

      // Copy handle and partition name
      Entry->PartitionHandle = AllHandles[LoopIndex];
      StrnCpy (
        Entry->PartitionName,
        PartitionEntries[PartitionNode->PartitionNumber - 1].PartitionName, // Partition numbers start from 1.
        PARTITION_NAME_MAX_LENGTH
        );
      Entry->Lba = PartitionEntries[PartitionNode->PartitionNumber - 1].StartingLBA;
      InsertTailList (&mPartitionListHead, &Entry->Link);

      // Print a debug message if the partition label is empty or looks like
      // garbage.
      if (!IS_ALPHA (Entry->PartitionName[0])) {
        DEBUG ((DEBUG_ERROR,
          "Warning: Partition %d doesn't seem to have a GPT partition label. "
          "You won't be able to flash it with Fastboot.\n",
          PartitionNode->PartitionNumber
          ));
        Status = EFI_INVALID_PARAMETER;
        FreePartitionList ();
        goto Exit;
      }
    }
  }

Exit:
  FreePages ((VOID *)((UINTN)PartitionEntries - (2 * EFI_PAGE_SIZE)), EFI_SIZE_TO_PAGES (6 * EFI_PAGE_SIZE));
  FreePool (FlashDevicePath);
  FreePool (AllHandles);
  return Status;
}

/*
  Initialise: Open the Android NVM device and find the partitions on it. Save them in
  a list along with the "PartitionName" fields for their GPT entries.
  We will use these partition names as the key in
  HiKey960FastbootPlatformFlashPartition.
*/
EFI_STATUS
HiKey960FastbootPlatformInit (
  VOID
  )
{
  return LoadPtable ();
}

VOID
HiKey960FastbootPlatformUnInit (
  VOID
  )
{
  FreePartitionList ();
}

EFI_STATUS
HiKey960FlashPtable (
  IN UINTN   Size,
  IN VOID   *Image
  )
{
  EFI_STATUS               Status;
  //EFI_BLOCK_IO_PROTOCOL   *BlockIo;
  EFI_HANDLE                          FlashHandle;
  EFI_DEVICE_PATH_PROTOCOL           *FlashDevicePath;
  EFI_DEVICE_PATH_PROTOCOL           *FlashDevicePathDup;
  EFI_BLOCK_IO_PROTOCOL              *FlashBlockIo;

  FlashDevicePath = ConvertTextToDevicePath ((CHAR16*)FixedPcdGetPtr (PcdAndroidFastbootNvmDevicePath));

  //
  // Open the Disk IO protocol on the flash device - this will be used to read
  // partition names out of the GPT entries
  //
  // Create another device path pointer because LocateDevicePath will modify it.
  FlashDevicePathDup = FlashDevicePath;
  Status = gBS->LocateDevicePath (&gEfiBlockIoProtocolGuid, &FlashDevicePathDup, &FlashHandle);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Warning: Couldn't locate Android NVM device (status: %r)\n", Status));
    // Failing to locate partitions should not prevent to do other Android FastBoot actions
    return EFI_SUCCESS;
  }

  Status = gBS->OpenProtocol (
                  FlashHandle,
                  &gEfiBlockIoProtocolGuid,
                  (VOID **) &FlashBlockIo,
                  gImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Fastboot platform: Couldn't open Android NVM device (status: %r)\n", Status));
    return EFI_DEVICE_ERROR;
  }
  Status = FlashBlockIo->WriteBlocks (
                           FlashBlockIo,
                           FlashBlockIo->Media->MediaId,
                           0,
                           Size,
                           Image
                           );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to write (status:%r)\n", Status));
    return Status;
  }
  FreePartitionList ();
  Status = LoadPtable ();
  return Status;
}

EFI_STATUS
HiKey960FastbootPlatformFlashPartition (
  IN CHAR8  *PartitionName,
  IN UINTN   Size,
  IN VOID   *Image
  )
{
  EFI_STATUS               Status;
  EFI_BLOCK_IO_PROTOCOL   *BlockIo;
  EFI_DISK_IO_PROTOCOL    *DiskIo;
  UINT32                   MediaId;
  UINTN                    PartitionSize;
  FASTBOOT_PARTITION_LIST *Entry;
  CHAR16                   PartitionNameUnicode[60];
  BOOLEAN                  PartitionFound;
#ifdef SPARSE_HEADER
  SPARSE_HEADER           *SparseHeader;
  CHUNK_HEADER            *ChunkHeader;
  UINTN                    Offset = 0;
  UINT32                   Chunk, EntrySize, EntryOffset;
  UINT32                  *FillVal, TmpCount, FillBuf[1024];
#endif

  if (AsciiStrCmp (PartitionName, "ptable") == 0) {
    return HiKey960FlashPtable (Size, Image);
  }

  AsciiStrToUnicodeStr (PartitionName, PartitionNameUnicode);
  PartitionFound = FALSE;
  Entry = (FASTBOOT_PARTITION_LIST *) GetFirstNode (&(mPartitionListHead));
  while (!IsNull (&mPartitionListHead, &Entry->Link)) {
    // Search the partition list for the partition named by PartitionName
    if (StrCmp (Entry->PartitionName, PartitionNameUnicode) == 0) {
      PartitionFound = TRUE;
      break;
    }

   Entry = (FASTBOOT_PARTITION_LIST *) GetNextNode (&mPartitionListHead, &(Entry)->Link);
  }
  if (!PartitionFound) {
    return EFI_NOT_FOUND;
  }

  Status = gBS->OpenProtocol (
                  Entry->PartitionHandle,
                  &gEfiBlockIoProtocolGuid,
                  (VOID **) &BlockIo,
                  gImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Fastboot platform: couldn't open Block IO for flash: %r\n", Status));
    return EFI_NOT_FOUND;
  }

#ifdef SPARSE_HEADER
  SparseHeader=(SPARSE_HEADER *)Image;

  if (SparseHeader->Magic == SPARSE_HEADER_MAGIC) {
    DEBUG ((DEBUG_INFO, "Sparse Magic: 0x%x Major: %d Minor: %d fhs: %d chs: %d bs: %d tbs: %d tcs: %d checksum: %d \n",
                SparseHeader->Magic, SparseHeader->MajorVersion, SparseHeader->MinorVersion,  SparseHeader->FileHeaderSize,
                SparseHeader->ChunkHeaderSize, SparseHeader->BlockSize, SparseHeader->TotalBlocks,
                SparseHeader->TotalChunks, SparseHeader->ImageChecksum));
    if (SparseHeader->MajorVersion != 1) {
        DEBUG ((DEBUG_ERROR, "Sparse image version %d.%d not supported.\n",
                    SparseHeader->MajorVersion, SparseHeader->MinorVersion));
        return EFI_INVALID_PARAMETER;
    }

    Size = SparseHeader->BlockSize * SparseHeader->TotalBlocks;
  }
#endif

  // Check image will fit on device
  PartitionSize = (BlockIo->Media->LastBlock + 1) * BlockIo->Media->BlockSize;
  if (PartitionSize < Size) {
    DEBUG ((DEBUG_ERROR, "Partition not big enough.\n"));
    DEBUG ((DEBUG_ERROR, "Partition Size:\t%ld\nImage Size:\t%ld\n", PartitionSize, Size));

    return EFI_VOLUME_FULL;
  }

  MediaId = BlockIo->Media->MediaId;

  Status = gBS->OpenProtocol (
                  Entry->PartitionHandle,
                  &gEfiDiskIoProtocolGuid,
                  (VOID **) &DiskIo,
                  gImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  ASSERT_EFI_ERROR (Status);

#ifdef SPARSE_HEADER
  if (SparseHeader->Magic == SPARSE_HEADER_MAGIC) {
    CHAR16 OutputString[64];
    UINTN ChunkPrintDensity =
        SparseHeader->TotalChunks > 1600 ? SparseHeader->TotalChunks / 200 : 32;

    Image += SparseHeader->FileHeaderSize;
    for (Chunk = 0; Chunk < SparseHeader->TotalChunks; Chunk++) {
      UINTN WriteSize;
      ChunkHeader = (CHUNK_HEADER *)Image;

      // Show progress. Don't do it for every packet as outputting text
      // might be time consuming. ChunkPrintDensity is calculated to
      // provide an update every half percent change for large
      // downloads.
      if (Chunk % ChunkPrintDensity == 0) {
        UnicodeSPrint(OutputString, sizeof(OutputString),
                      L"\r%5d / %5d chunks written (%d%%)", Chunk,
                      SparseHeader->TotalChunks,
                     (Chunk * 100) / SparseHeader->TotalChunks);
        mTextOut->OutputString(mTextOut, OutputString);
      }

      DEBUG ((DEBUG_INFO, "Chunk #%d - Type: 0x%x Size: %d TotalSize: %d Offset %d\n",
                  (Chunk+1), ChunkHeader->ChunkType, ChunkHeader->ChunkSize,
                  ChunkHeader->TotalSize, Offset));
      Image += sizeof(CHUNK_HEADER);
      WriteSize=(SparseHeader->BlockSize) * ChunkHeader->ChunkSize;
      switch (ChunkHeader->ChunkType) {
        case CHUNK_TYPE_RAW:
          DEBUG ((DEBUG_INFO, "Writing %d at Offset %d\n", WriteSize, Offset));
          Status = DiskIo->WriteDisk (DiskIo, MediaId, Offset, WriteSize, Image);
          if (EFI_ERROR (Status)) {
            return Status;
          }
          Image+=WriteSize;
          break;
        case CHUNK_TYPE_FILL:
          //Assume fillVal is 0, and we can skip here
          FillVal = (UINT32 *)Image;
          Image += sizeof(UINT32);
          if (*FillVal != 0){
            mTextOut->OutputString(mTextOut, OutputString);
            for(TmpCount = 0; TmpCount < 1024; TmpCount++){
                FillBuf[TmpCount] = *FillVal;
            }
            for (TmpCount= 0; TmpCount < WriteSize; TmpCount += sizeof(FillBuf)) {
                if ((WriteSize - TmpCount) < sizeof(FillBuf)) {
                  Status = DiskIo->WriteDisk (DiskIo, MediaId, Offset + TmpCount, WriteSize - TmpCount, FillBuf);
                } else {
                  Status = DiskIo->WriteDisk (DiskIo, MediaId, Offset + TmpCount, sizeof(FillBuf), FillBuf);
                }
                if (EFI_ERROR (Status)) {
                    return Status;
                }
            }
          }
          break;
        case CHUNK_TYPE_DONT_CARE:
          break;
        case CHUNK_TYPE_CRC32:
          break;
        default:
          DEBUG ((DEBUG_ERROR, "Unknown Chunk Type: 0x%x", ChunkHeader->ChunkType));
          return EFI_PROTOCOL_ERROR;
      }
      Offset += WriteSize;
    }

    UnicodeSPrint(OutputString, sizeof(OutputString),
                  L"\r%5d / %5d chunks written (100%%)\r\n",
                  SparseHeader->TotalChunks, SparseHeader->TotalChunks);
    mTextOut->OutputString(mTextOut, OutputString);
  } else {
#endif
    Status = DiskIo->WriteDisk (DiskIo, MediaId, 0, Size, Image);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Failed to write %d bytes into 0x%x, Status:%r\n", Size, Image, Status));
      return Status;
    }
#ifdef SPARSE_HEADER
  }
#endif

  BlockIo->FlushBlocks(BlockIo);
  MicroSecondDelay (50000);

  return Status;
}

EFI_STATUS
HiKey960FastbootPlatformErasePartition (
  IN CHAR8 *PartitionName
  )
{
  return EFI_SUCCESS;
}

EFI_STATUS
HiKey960FastbootPlatformGetVar (
  IN  CHAR8   *Name,
  OUT CHAR8   *Value
  )
{
  EFI_STATUS               Status;
  EFI_BLOCK_IO_PROTOCOL   *BlockIo;
  UINT64                   PartitionSize;
  FASTBOOT_PARTITION_LIST *Entry;
  CHAR16                   PartitionNameUnicode[60];
  BOOLEAN                  PartitionFound;
  CHAR16                   DataUnicode[17];
  UINTN                    VariableSize;

  if (!AsciiStrCmp (Name, "max-download-size")) {
    AsciiStrCpy (Value, FixedPcdGetPtr (PcdArmFastbootFlashLimit));
  } else if (!AsciiStrCmp (Name, "product")) {
    AsciiStrCpy (Value, FixedPcdGetPtr (PcdFirmwareVendor));
  } else if (!AsciiStrCmp (Name, "serialno")) {
    VariableSize = 17 * sizeof (CHAR16);
    Status = gRT->GetVariable (
                    (CHAR16 *)L"SerialNo",
                    &gHiKey960VariableGuid,
                    NULL,
                    &VariableSize,
                    &DataUnicode
                    );
    if (EFI_ERROR (Status)) {
      *Value = '\0';
      return EFI_NOT_FOUND;
    }
    DataUnicode[(VariableSize / sizeof(CHAR16)) - 1] = '\0';
    UnicodeStrToAsciiStr (DataUnicode, Value);
  } else if ( !AsciiStrnCmp (Name, "partition-size", 14)) {
    AsciiStrToUnicodeStr ((Name + 15), PartitionNameUnicode);
    PartitionFound = FALSE;
    Entry = (FASTBOOT_PARTITION_LIST *) GetFirstNode (&(mPartitionListHead));
    while (!IsNull (&mPartitionListHead, &Entry->Link)) {
      // Search the partition list for the partition named by PartitionName
      if (StrCmp (Entry->PartitionName, PartitionNameUnicode) == 0) {
        PartitionFound = TRUE;
        break;
      }

     Entry = (FASTBOOT_PARTITION_LIST *) GetNextNode (&mPartitionListHead, &(Entry)->Link);
    }
    if (!PartitionFound) {
      *Value = '\0';
      return EFI_NOT_FOUND;
    }

    Status = gBS->OpenProtocol (
                    Entry->PartitionHandle,
                    &gEfiBlockIoProtocolGuid,
                    (VOID **) &BlockIo,
                    gImageHandle,
                    NULL,
                    EFI_OPEN_PROTOCOL_GET_PROTOCOL
                    );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Fastboot platform: couldn't open Block IO for flash: %r\n", Status));
      *Value = '\0';
      return EFI_NOT_FOUND;
    }

    PartitionSize = (BlockIo->Media->LastBlock + 1) * BlockIo->Media->BlockSize;
    DEBUG ((DEBUG_ERROR, "Fastboot platform: check for partition-size:%a 0X%llx\n", Name, PartitionSize ));
    AsciiSPrint (Value, 12, "0x%llx", PartitionSize);
  } else if ( !AsciiStrnCmp (Name, "partition-type", 14)) {
      DEBUG ((DEBUG_ERROR, "Fastboot platform: check for partition-type:%a\n", (Name + 15) ));
    if ( !AsciiStrnCmp  ( (Name + 15) , "system", 6) || !AsciiStrnCmp  ( (Name + 15) , "userdata", 8)
            || !AsciiStrnCmp  ( (Name + 15) , "cache", 5)) {
      AsciiStrCpy (Value, "ext4");
    } else {
      AsciiStrCpy (Value, "raw");
    }
  } else {
    *Value = '\0';
  }
  return EFI_SUCCESS;
}

EFI_STATUS
HiKey960FastbootPlatformOemCommand (
  IN  CHAR8   *Command
  )
{
  if (AsciiStrCmp (Command, "Demonstrate") == 0) {
    DEBUG ((DEBUG_ERROR, "ARM OEM Fastboot command 'Demonstrate' received.\n"));
    return EFI_SUCCESS;
  } else {
    DEBUG ((DEBUG_ERROR,
      "HiKey: Unrecognised Fastboot OEM command: %s\n",
      Command
      ));
    return EFI_NOT_FOUND;
  }
}

EFI_STATUS
HiKey960FastbootPlatformFlashPartitionEx (
  IN CHAR8  *PartitionName,
  IN UINTN   Offset,
  IN UINTN   Size,
  IN VOID   *Image
  )
{
  EFI_STATUS               Status;
  EFI_BLOCK_IO_PROTOCOL   *BlockIo;
  EFI_DISK_IO_PROTOCOL    *DiskIo;
  UINT32                   MediaId;
  UINTN                    PartitionSize;
  FASTBOOT_PARTITION_LIST *Entry;
  CHAR16                   PartitionNameUnicode[60];
  BOOLEAN                  PartitionFound;

  AsciiStrToUnicodeStr (PartitionName, PartitionNameUnicode);
  PartitionFound = FALSE;
  Entry = (FASTBOOT_PARTITION_LIST *) GetFirstNode (&(mPartitionListHead));
  while (!IsNull (&mPartitionListHead, &Entry->Link)) {
    // Search the partition list for the partition named by PartitionName
    if (StrCmp (Entry->PartitionName, PartitionNameUnicode) == 0) {
      PartitionFound = TRUE;
      break;
    }

   Entry = (FASTBOOT_PARTITION_LIST *) GetNextNode (&mPartitionListHead, &(Entry)->Link);
  }
  if (!PartitionFound) {
    return EFI_NOT_FOUND;
  }

  Status = gBS->OpenProtocol (
                  Entry->PartitionHandle,
                  &gEfiBlockIoProtocolGuid,
                  (VOID **) &BlockIo,
                  gImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Fastboot platform: couldn't open Block IO for flash: %r\n", Status));
    return EFI_NOT_FOUND;
  }

  // Check image will fit on device
  PartitionSize = (BlockIo->Media->LastBlock + 1) * BlockIo->Media->BlockSize;
  if (PartitionSize < Size) {
    DEBUG ((DEBUG_ERROR, "Partition not big enough.\n"));
    DEBUG ((DEBUG_ERROR, "Partition Size:\t%ld\nImage Size:\t%ld\n", PartitionSize, Size));

    return EFI_VOLUME_FULL;
  }

  MediaId = BlockIo->Media->MediaId;

  Status = gBS->OpenProtocol (
                  Entry->PartitionHandle,
                  &gEfiDiskIoProtocolGuid,
                  (VOID **) &DiskIo,
                  gImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  ASSERT_EFI_ERROR (Status);

  Status = DiskIo->WriteDisk (DiskIo, MediaId, Offset, Size, Image);
  return Status;
}

FASTBOOT_PLATFORM_PROTOCOL mPlatformProtocol = {
  HiKey960FastbootPlatformInit,
  HiKey960FastbootPlatformUnInit,
  HiKey960FastbootPlatformFlashPartition,
  HiKey960FastbootPlatformErasePartition,
  HiKey960FastbootPlatformGetVar,
  HiKey960FastbootPlatformOemCommand,
  HiKey960FastbootPlatformFlashPartitionEx
};

EFI_STATUS
EFIAPI
HiKey960FastbootPlatformEntryPoint (
  IN EFI_HANDLE                            ImageHandle,
  IN EFI_SYSTEM_TABLE                      *SystemTable
  )
{
  return gBS->InstallProtocolInterface (
                &ImageHandle,
                &gAndroidFastbootPlatformProtocolGuid,
                EFI_NATIVE_INTERFACE,
                &mPlatformProtocol
                );
}
