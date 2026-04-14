#include "persistent_store.hh"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace MasstreeLHM {

namespace {

static constexpr uint32_t kMetadataMagic = 0x4C484D31u;  // "LHM1"
static constexpr uint16_t kLayoutVersion = 1;
static constexpr std::size_t kDirectIoAlign = 4096;

// O_DIRECT 在少数平台上可能不存在；这里保证至少可编译。
#ifndef O_DIRECT
#define O_DIRECT 0
#endif

bool alloc_aligned_block(void** out_ptr) {
    *out_ptr = nullptr;
    int rc = ::posix_memalign(out_ptr, kDirectIoAlign, kMetadataBlockBytes);
    return rc == 0 && *out_ptr != nullptr;
}

bool pread_all(int fd, void* buf, size_t bytes, off_t off) {
    uint8_t* p = reinterpret_cast<uint8_t*>(buf);
    size_t done = 0;
    while (done < bytes) {
        ssize_t n = ::pread(fd, p + done, bytes - done, off + done);
        if (n > 0) {
            done += static_cast<size_t>(n);
            continue;
        }
        if (n == 0) {
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool pwrite_all(int fd, const void* buf, size_t bytes, off_t off) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(buf);
    size_t done = 0;
    while (done < bytes) {
        ssize_t n = ::pwrite(fd, p + done, bytes - done, off + done);
        if (n > 0) {
            done += static_cast<size_t>(n);
            continue;
        }
        if (n == 0) {
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

off_t block_offset(uint32_t block_id) {
    return static_cast<off_t>(block_id) * static_cast<off_t>(kMetadataBlockBytes);
}

}  // namespace

PersistentStore::PersistentStore()
    : fd_(-1), next_alloc_block_(1), sequence_(1),
      active_inode_block_id_(0), next_inode_slot_index_(0) {
}

PersistentStore::~PersistentStore() {
    Close();
}

bool PersistentStore::OpenOrCreate(const std::string& path) {
    Close();
    // 当前阶段统一采用 direct I/O，绕过页缓存。
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_DIRECT, 0644);
    if (fd < 0) {
        return false;
    }

    fd_ = fd;
    path_ = path;
    next_alloc_block_ = 1;
    sequence_ = 1;
    active_inode_block_id_ = 0;
    next_inode_slot_index_ = 0;

    struct stat st;
    if (::fstat(fd_, &st) != 0) {
        Close();
        return false;
    }

    if (st.st_size == 0) {
        if (!InitializeFreshStore()) {
            Close();
            return false;
        }
        return true;
    }

    if (!RecomputeNextAllocBlock()) {
        Close();
        return false;
    }
    return true;
}

void PersistentStore::Close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    path_.clear();
    next_alloc_block_ = 1;
    sequence_ = 1;
    active_inode_block_id_ = 0;
    next_inode_slot_index_ = 0;
}

bool PersistentStore::IsOpen() const {
    return fd_ >= 0;
}

bool PersistentStore::Sync() {
    if (!IsOpen()) {
        return false;
    }
    return ::fsync(fd_) == 0;
}

bool PersistentStore::ReadBlock(uint32_t block_id, metadata_block_bytes& out) const {
    if (!IsOpen()) {
        return false;
    }
    void* aligned = nullptr;
    if (!alloc_aligned_block(&aligned)) {
        return false;
    }
    bool ok = pread_all(fd_, aligned, kMetadataBlockBytes, block_offset(block_id));
    if (ok) {
        std::memcpy(out.data(), aligned, kMetadataBlockBytes);
    }
    ::free(aligned);
    return ok;
}

bool PersistentStore::WriteBlock(uint32_t block_id, const metadata_block_bytes& data) {
    if (!IsOpen()) {
        return false;
    }
    if (!EnsureBlockCount(block_id + 1)) {
        return false;
    }
    void* aligned = nullptr;
    if (!alloc_aligned_block(&aligned)) {
        return false;
    }
    std::memcpy(aligned, data.data(), kMetadataBlockBytes);
    bool ok = pwrite_all(fd_, aligned, kMetadataBlockBytes, block_offset(block_id));
    ::free(aligned);
    return ok;
}

bool PersistentStore::AllocateBlock(uint32_t& out_block_id) {
    if (!IsOpen()) {
        return false;
    }
    out_block_id = next_alloc_block_++;
    return EnsureBlockCount(next_alloc_block_);
}

bool PersistentStore::ReadSuperblock(superblock_payload& out) const {
    metadata_block_bytes block;
    if (!ReadBlock(0, block)) {
        return false;
    }
    const auto* common = reinterpret_cast<const metadata_common_block_header*>(block.data());
    if (common->magic != kMetadataMagic
        || common->block_type != static_cast<uint16_t>(metadata_block_type::superblock)) {
        return false;
    }
    const auto* payload =
        reinterpret_cast<const superblock_payload*>(block.data() + kCommonBlockHeaderBytes);
    out = *payload;
    return true;
}

bool PersistentStore::WriteSuperblock(const superblock_payload& payload) {
    metadata_block_bytes block{};
    auto* common = reinterpret_cast<metadata_common_block_header*>(block.data());
    BuildCommonHeader(*common, metadata_block_type::superblock, 0,
                      sizeof(superblock_payload), sequence_++, 0, 0, 0);
    auto* p = reinterpret_cast<superblock_payload*>(block.data() + kCommonBlockHeaderBytes);
    *p = payload;
    return WriteBlock(0, block);
}

bool PersistentStore::ReadCheckpoint(uint32_t block_id, checkpoint_payload& out) const {
    metadata_block_bytes block;
    if (!ReadBlock(block_id, block)) {
        return false;
    }
    const auto* common = reinterpret_cast<const metadata_common_block_header*>(block.data());
    if (common->magic != kMetadataMagic
        || common->block_type != static_cast<uint16_t>(metadata_block_type::checkpoint)) {
        return false;
    }
    const auto* payload =
        reinterpret_cast<const checkpoint_payload*>(block.data() + kCommonBlockHeaderBytes);
    out = *payload;
    return true;
}

bool PersistentStore::WriteCheckpoint(uint32_t block_id, const checkpoint_payload& payload) {
    metadata_block_bytes block{};
    auto* common = reinterpret_cast<metadata_common_block_header*>(block.data());
    BuildCommonHeader(*common, metadata_block_type::checkpoint, block_id,
                      sizeof(checkpoint_payload), sequence_++, 0, 0, 0);
    auto* p = reinterpret_cast<checkpoint_payload*>(block.data() + kCommonBlockHeaderBytes);
    *p = payload;
    return WriteBlock(block_id, block);
}

bool PersistentStore::ReadAllocatorState(uint32_t block_id, allocator_state_payload& out) const {
    metadata_block_bytes block;
    if (!ReadBlock(block_id, block)) {
        return false;
    }
    const auto* common = reinterpret_cast<const metadata_common_block_header*>(block.data());
    if (common->magic != kMetadataMagic
        || common->block_type != static_cast<uint16_t>(metadata_block_type::allocator_state)) {
        return false;
    }
    const auto* payload =
        reinterpret_cast<const allocator_state_payload*>(block.data() + kCommonBlockHeaderBytes);
    out = *payload;
    return true;
}

bool PersistentStore::WriteAllocatorState(uint32_t block_id,
                                          const allocator_state_payload& payload) {
    metadata_block_bytes block{};
    auto* common = reinterpret_cast<metadata_common_block_header*>(block.data());
    BuildCommonHeader(*common, metadata_block_type::allocator_state, block_id,
                      sizeof(allocator_state_payload), sequence_++, 0, 0, 0);
    auto* p =
        reinterpret_cast<allocator_state_payload*>(block.data() + kCommonBlockHeaderBytes);
    *p = payload;
    return WriteBlock(block_id, block);
}

bool PersistentStore::ReadInodeBlock(uint32_t block_id, inode_block_image& out) const {
    metadata_block_bytes block;
    if (!ReadBlock(block_id, block)) {
        return false;
    }
    std::memcpy(&out, block.data(), sizeof(out));
    if (out.common.magic != kMetadataMagic
        || out.common.block_type != static_cast<uint16_t>(metadata_block_type::inode_block)) {
        return false;
    }
    return true;
}

bool PersistentStore::WriteInodeBlock(uint32_t block_id, const inode_block_image& image) {
    metadata_block_bytes block{};
    std::memcpy(block.data(), &image, sizeof(image));
    return WriteBlock(block_id, block);
}

bool PersistentStore::AllocateInodeSlot(inode_ref& out_ref) {
    if (!IsOpen()) {
        return false;
    }
    // 线性分配策略：
    // 1) 当前 inode 块未满则直接取 next slot；
    // 2) 当前块满/不存在时新建一个 inode 块继续顺序分配；
    // 3) 不做全表扫描与空洞复用。
    if (active_inode_block_id_ == 0 || next_inode_slot_index_ >= kInodesPerBlock) {
        uint32_t new_block = 0;
        if (!AllocateBlock(new_block)) {
            return false;
        }

        inode_block_image fresh{};
        BuildCommonHeader(fresh.common, metadata_block_type::inode_block, new_block,
                          sizeof(inode_block_header) + kInodesPerBlock * sizeof(inode_disk),
                          sequence_++, 0, 0, 0);
        fresh.header.slot_count = kInodesPerBlock;
        fresh.header.used_count = 0;
        fresh.header.flags = 0;
        fresh.header.reserved0 = 0;
        fresh.header.alloc_epoch = sequence_;
        std::memset(fresh.header.slot_bitmap, 0, sizeof(fresh.header.slot_bitmap));
        std::memset(fresh.slots, 0, sizeof(fresh.slots));
        if (!WriteInodeBlock(new_block, fresh)) {
            return false;
        }
        active_inode_block_id_ = new_block;
        next_inode_slot_index_ = 0;
    }

    inode_block_image image;
    if (!ReadInodeBlock(active_inode_block_id_, image)) {
        return false;
    }

    uint32_t slot = next_inode_slot_index_;
    if (slot >= kInodesPerBlock) {
        active_inode_block_id_ = 0;
        next_inode_slot_index_ = 0;
        return AllocateInodeSlot(out_ref);
    }
    if (IsSlotUsed(image.header, slot)) {
        // 只在块内向前探测，避免全表扫描；出现异常时继续线性推进。
        while (slot < kInodesPerBlock && IsSlotUsed(image.header, slot)) {
            ++slot;
        }
        if (slot >= kInodesPerBlock) {
            active_inode_block_id_ = 0;
            next_inode_slot_index_ = 0;
            return AllocateInodeSlot(out_ref);
        }
    }

    SetSlotUsed(image.header, slot, true);
    if (image.header.used_count <= slot) {
        image.header.used_count = slot + 1;
    }
    std::memset(&image.slots[slot], 0, sizeof(inode_disk));
    if (!WriteInodeBlock(active_inode_block_id_, image)) {
        return false;
    }

    out_ref = make_inode_ref(active_inode_block_id_, slot);
    next_inode_slot_index_ = slot + 1;
    return true;
}

bool PersistentStore::ReadInode(const inode_ref& ref, inode_disk& out) const {
    if (ref.offset >= kInodesPerBlock) {
        return false;
    }
    inode_block_image image;
    if (!ReadInodeBlock(ref.block_id, image)) {
        return false;
    }
    if (!IsSlotUsed(image.header, ref.offset)) {
        return false;
    }
    out = image.slots[ref.offset];
    return true;
}

bool PersistentStore::WriteInode(const inode_ref& ref, const inode_disk& inode) {
    if (ref.offset >= kInodesPerBlock) {
        return false;
    }
    inode_block_image image;
    if (!ReadInodeBlock(ref.block_id, image)) {
        return false;
    }
    if (!IsSlotUsed(image.header, ref.offset)) {
        return false;
    }
    image.slots[ref.offset] = inode;
    return WriteInodeBlock(ref.block_id, image);
}

bool PersistentStore::ReadDirectoryBlock(uint32_t block_id, directory_block_image& out) const {
    metadata_block_bytes block;
    if (!ReadBlock(block_id, block)) {
        return false;
    }
    std::memcpy(&out, block.data(), sizeof(out));
    if (out.common.magic != kMetadataMagic
        || out.common.block_type != static_cast<uint16_t>(metadata_block_type::directory_block)) {
        return false;
    }
    return true;
}

bool PersistentStore::WriteDirectoryBlock(uint32_t block_id,
                                          const directory_block_image& image) {
    metadata_block_bytes block{};
    std::memcpy(block.data(), &image, sizeof(image));
    return WriteBlock(block_id, block);
}

uint32_t PersistentStore::NextAllocBlock() const {
    return next_alloc_block_;
}

const std::string& PersistentStore::BackingFile() const {
    return path_;
}

bool PersistentStore::EnsureBlockCount(uint32_t block_count) {
    off_t target_size = block_offset(block_count);
    struct stat st;
    if (::fstat(fd_, &st) != 0) {
        return false;
    }
    if (st.st_size >= target_size) {
        return true;
    }
    return ::ftruncate(fd_, target_size) == 0;
}

bool PersistentStore::InitializeFreshStore() {
    if (!EnsureBlockCount(1)) {
        return false;
    }

    superblock_payload sb{};
    sb.metadata_block_bytes = kMetadataBlockBytes;
    sb.latest_checkpoint_block = 0;
    sb.latest_checkpoint_seq = 0;
    sb.fs_uuid_hi = 0;
    sb.fs_uuid_lo = 0;
    sb.flags = 0;
    if (!WriteSuperblock(sb)) {
        return false;
    }

    next_alloc_block_ = 1;
    sequence_ = 1;
    active_inode_block_id_ = 0;
    next_inode_slot_index_ = 0;
    return true;
}

bool PersistentStore::RecomputeNextAllocBlock() {
    struct stat st;
    if (::fstat(fd_, &st) != 0) {
        return false;
    }
    if (st.st_size < 0) {
        return false;
    }
    uint64_t bytes = static_cast<uint64_t>(st.st_size);
    uint64_t blocks = (bytes + kMetadataBlockBytes - 1) / kMetadataBlockBytes;
    if (blocks == 0) {
        next_alloc_block_ = 1;
        return InitializeFreshStore();
    }
    if (blocks > UINT32_MAX) {
        return false;
    }
    next_alloc_block_ = static_cast<uint32_t>(blocks);

    // 确认 block0 是 superblock，否则拒绝加载。
    metadata_block_bytes block0;
    if (!ReadBlock(0, block0)) {
        return false;
    }
    const auto* common = reinterpret_cast<const metadata_common_block_header*>(block0.data());
    if (common->magic != kMetadataMagic
        || common->block_type != static_cast<uint16_t>(metadata_block_type::superblock)) {
        return false;
    }
    sequence_ = common->sequence + 1;
    return RecomputeLinearInodeCursor();
}

bool PersistentStore::RecomputeLinearInodeCursor() {
    active_inode_block_id_ = 0;
    next_inode_slot_index_ = 0;

    // 启动阶段允许一次线性扫描，用于恢复“最后一个 inode block + 下一个 slot”游标。
    for (uint32_t b = 1; b < next_alloc_block_; ++b) {
        metadata_block_bytes raw;
        if (!ReadBlock(b, raw)) {
            continue;
        }
        const auto* common =
            reinterpret_cast<const metadata_common_block_header*>(raw.data());
        if (common->magic != kMetadataMagic
            || common->block_type != static_cast<uint16_t>(metadata_block_type::inode_block)) {
            continue;
        }

        inode_block_image image;
        if (!ReadInodeBlock(b, image)) {
            return false;
        }
        active_inode_block_id_ = b;
        uint32_t used = image.header.used_count;
        if (used > kInodesPerBlock) {
            used = kInodesPerBlock;
        }
        next_inode_slot_index_ = used;
    }
    return true;
}

bool PersistentStore::IsKnownBlockType(uint16_t block_type) const {
    return block_type >= static_cast<uint16_t>(metadata_block_type::superblock)
        && block_type <= static_cast<uint16_t>(metadata_block_type::allocator_state);
}

bool PersistentStore::IsSlotUsed(const inode_block_header& header, uint32_t slot) {
    if (slot >= kInodesPerBlock) {
        return false;
    }
    uint32_t byte_index = slot / 8;
    uint32_t bit_index = slot % 8;
    return (header.slot_bitmap[byte_index] & static_cast<uint8_t>(1u << bit_index)) != 0;
}

void PersistentStore::SetSlotUsed(inode_block_header& header, uint32_t slot, bool used) {
    if (slot >= kInodesPerBlock) {
        return;
    }
    uint32_t byte_index = slot / 8;
    uint32_t bit_index = slot % 8;
    uint8_t mask = static_cast<uint8_t>(1u << bit_index);
    if (used) {
        header.slot_bitmap[byte_index] |= mask;
    } else {
        header.slot_bitmap[byte_index] &= static_cast<uint8_t>(~mask);
    }
}

void PersistentStore::BuildCommonHeader(metadata_common_block_header& out,
                                        metadata_block_type block_type,
                                        uint32_t block_id,
                                        uint32_t payload_bytes,
                                        uint64_t sequence,
                                        uint64_t owner_inode_id,
                                        uint32_t next_block_id,
                                        uint32_t flags) {
    std::memset(&out, 0, sizeof(out));
    out.magic = kMetadataMagic;
    out.block_type = static_cast<uint16_t>(block_type);
    out.layout_version = kLayoutVersion;
    out.block_id = block_id;
    out.payload_bytes = payload_bytes;
    out.checksum = 0;
    out.sequence = sequence;
    out.owner_inode_id = owner_inode_id;
    out.next_block_id = next_block_id;
    out.flags = flags;
}

}  // namespace MasstreeLHM
