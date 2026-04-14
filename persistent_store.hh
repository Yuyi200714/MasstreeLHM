#ifndef MASSTREELHM_PERSISTENT_STORE_HH
#define MASSTREELHM_PERSISTENT_STORE_HH

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "metadata_layout.hh"

namespace MasstreeLHM {

// 一个完整的 16KB 元数据块镜像。
using metadata_block_bytes = std::array<uint8_t, kMetadataBlockBytes>;

// inode 块的内存镜像，方便一次读写与尺寸校验。
struct inode_block_image {
    metadata_common_block_header common;
    inode_block_header header;
    inode_disk slots[kInodesPerBlock];
};
static_assert(sizeof(inode_block_image) == kMetadataBlockBytes,
              "inode block image must be one 16KB metadata block");

// 目录块的内存镜像：固定头 + 目录项载荷。
struct directory_block_image {
    metadata_common_block_header common;
    directory_block_header header;
    uint8_t payload[kDirectoryBlockPayloadBytes];
};
static_assert(sizeof(directory_block_image) == kMetadataBlockBytes,
              "directory block image must be one 16KB metadata block");

// 第一阶段最小持久化存储：
// 1) 提供 16KB 元数据块读写；
// 2) 提供顺序块分配；
// 3) 提供 inode slot 分配与 inode 读写；
// 4) 提供目录块读写。
//
// 当前不实现崩溃恢复流程，只保留布局和最小操作能力。
class PersistentStore {
  public:
    PersistentStore();
    ~PersistentStore();

    PersistentStore(const PersistentStore&) = delete;
    PersistentStore& operator=(const PersistentStore&) = delete;

    bool OpenOrCreate(const std::string& path);
    void Close();
    bool IsOpen() const;
    bool Sync();

    // 基础块读写接口（固定 16KB）。
    bool ReadBlock(uint32_t block_id, metadata_block_bytes& out) const;
    bool WriteBlock(uint32_t block_id, const metadata_block_bytes& data);

    // 顺序分配一个新块号（当前为 append-only 策略）。
    bool AllocateBlock(uint32_t& out_block_id);

    // 控制块读写（block_id=0 固定 superblock）。
    bool ReadSuperblock(superblock_payload& out) const;
    bool WriteSuperblock(const superblock_payload& payload);
    bool ReadCheckpoint(uint32_t block_id, checkpoint_payload& out) const;
    bool WriteCheckpoint(uint32_t block_id, const checkpoint_payload& payload);
    bool ReadAllocatorState(uint32_t block_id, allocator_state_payload& out) const;
    bool WriteAllocatorState(uint32_t block_id, const allocator_state_payload& payload);

    // inode 块与 inode slot 操作。
    bool ReadInodeBlock(uint32_t block_id, inode_block_image& out) const;
    bool WriteInodeBlock(uint32_t block_id, const inode_block_image& image);
    bool AllocateInodeSlot(inode_ref& out_ref);
    bool ReadInode(const inode_ref& ref, inode_disk& out) const;
    bool WriteInode(const inode_ref& ref, const inode_disk& inode);

    // 目录块操作。
    bool ReadDirectoryBlock(uint32_t block_id, directory_block_image& out) const;
    bool WriteDirectoryBlock(uint32_t block_id, const directory_block_image& image);

    uint32_t NextAllocBlock() const;
    const std::string& BackingFile() const;

  private:
    bool EnsureBlockCount(uint32_t block_count);
    bool InitializeFreshStore();
    bool RecomputeNextAllocBlock();
    bool RecomputeLinearInodeCursor();
    bool IsKnownBlockType(uint16_t block_type) const;

    static bool IsSlotUsed(const inode_block_header& header, uint32_t slot);
    static void SetSlotUsed(inode_block_header& header, uint32_t slot, bool used);
    static void BuildCommonHeader(metadata_common_block_header& out,
                                  metadata_block_type block_type,
                                  uint32_t block_id,
                                  uint32_t payload_bytes,
                                  uint64_t sequence,
                                  uint64_t owner_inode_id,
                                  uint32_t next_block_id,
                                  uint32_t flags);

    int fd_;
    std::string path_;
    uint32_t next_alloc_block_;
    uint64_t sequence_;
    uint32_t active_inode_block_id_;
    uint32_t next_inode_slot_index_;
};

}  // namespace MasstreeLHM

#endif
