# LHM over Masstree: Current Design (Concise)

## 1. 目标

LHM 的当前目标是：

- 使用 Masstree 作为**内存命名空间路由层**；
- 使用目录感知布局作为**SSD 元数据真相层**；
- 支持 `stat / ls / create / delete / rename`；
- 保留目录级高效 `rename` 的结构优势；
- 为后续与 RocksDB 映射方案（InfiniFS / SingularFS）对比提供统一实现基线。

---

## 2. 当前总体架构

### 2.1 分层

1. 内存层（LHM + Masstree）
- 路由路径：`父目录 + 最后一级组件`
- 热索引：叶子值可携带 `name + inode_ref`
- 用于快速 `stat/create/delete/rename` 判定与定位

2. 磁盘层（SSD metadata）
- `stable directory blocks`：目录项真相层（名字 + inode_ref）
- `inode/file metadata blocks`：对象元数据真相层
- `superblock/checkpoint/allocator state`：控制与预留恢复字段

### 2.2 真相与缓存边界

- 磁盘目录块与 inode 块是 **source of truth**。
- Masstree 叶子值是**可重建的加速副本**。
- 写路径遵循：优先更新真相层，再更新叶子索引（或用版本校验延迟修正）。

---

## 3. 路径与索引语义

### 3.1 路径编码

- 绝对路径按 `/` 切分为分量；
- 分量哈希为 `uint64_t`；
- 作为层级 key 在 Masstree 中路由。

### 3.2 名字归属原则（已定稿）

- **名字属于 directory entry，不属于 inode**。
- inode 只表示对象本体元数据。
- `rename` 主要修改目录映射，不修改 inode 本体。

---

## 4. 元数据布局（定稿）

文件：`metadata_layout.hh`

### 4.1 固定参数

- `metadata block size = 16KB`
- `directory buffer size = 16KB`
- `inode size = 256B`

### 4.2 已定结构

- `metadata_common_block_header`：64B
- `superblock_payload`：64B
- `checkpoint_payload`：64B
- `allocator_state_payload`：64B
- `inode_disk`：256B
- `inode_block_header`：192B
- `directory_entry_disk_header`：24B（后接变长名字）
- `directory_block_header`：64B

### 4.3 inode block 组织

- `64B common header`
- `192B inode_block_header`
- `63 * 256B inode slots`
- 总计 16KB

### 4.4 directory block 组织

- `64B common header`
- `64B directory_block_header`
- `16256B payload`（顺序存放变长目录项）

---

## 5. 当前持久化实现状态（已落地）

- `persistent_store` 已接入，后端文件：`/mnt/batchtest/lhm/lhm_namespace.meta`；
- 采用 `O_DIRECT` + 16KB 固定块 I/O；
- 已实现：`superblock`、inode block 读写、directory block 读写、顺序块分配；
- 根目录 inode 已持久化并在内存中缓存 `root_persistent_ref`。

### 5.1 inode 分配策略（最新）

- 已从“每次分配全表线性扫描”改为“线性顺序分配”；
- 运行时只维护：
  - `active_inode_block_id`
  - `next_inode_slot_index`
- 单次 `AllocateInodeSlot` 不再扫描历史 inode blocks；
- 仅在 `OpenOrCreate` 时做一次游标重建扫描（恢复下一分配位置）。

---

## 6. 当前接口流程（按已实现代码）

### 6.1 `stat(path)` / `lookup(path)`

1. 解析路径；
2. 只定位一次父目录 `root`（`locate_directory_root(parent)`）；
3. 只查一次最后一级槽位（`lookup_child_from_parent_root`）；
4. 按 `name_length + memcmp` 做名字确认；
5. 返回 `namespace_entry`（文件或目录）。

要点：已去掉旧路径中的“整路径按目录判定 + 再查 value”的双遍历。

### 6.2 `ls(path)`

1. 解析路径；
2. 对非根目录仅执行一次 `locate_directory_root` 并校验目录元数据名字；
3. 从该层 root 左到右扫描当前层目录项；
4. 命中目录边只恢复目录项，不下钻子目录。

要点：当前 `ls` 只扫当前层，不做全表扫描。

### 6.3 `create(path)` / `mkdir(path)`

1. 解析路径并一次性解析父目录上下文：
  - `parent_root`
  - `parent_ref`
2. 线性分配 inode slot（无全表扫描）；
3. 在 `parent_root` 上单次游标创建：
  - 文件：`find_insert` 后插入（不再维护冲突链）；
  - 目录：`create_layer_with_meta`；
4. 持久化 inode（`persist_create_success`），不再重复查父 inode。

### 6.4 `delete(path)` / `rmdir(path)`

1. 解析路径；
2. 定位父目录 root；
3. 单次游标删除最后一级槽位（目录会做非空检查）；
4. 返回被删 entry；
5. inode 持久化 tombstone 更新（`persist_delete_success`）。

补充：测试接口 `remove_path_for_test` 已改为“直接删 + 直接持久化”，不再先 `lookup` 再删。

### 6.5 `rename(old, new)`

- 文件：创建新路径项 + 删除旧路径项；持久化时更新目标父 inode。
- 目录：沿用 O(1) layer attach/detach 路线（不重写整棵子树）；目录元数据名在新位置更新。

### 6.6 哈希碰撞策略（当前）

- 当前采用“忽略哈希碰撞”模式：同父目录下同 `component_hash` 只保留单槽位。
- 已移除 `conflict_bucket` 及 `entry_kind::conflict_chain` 路线。
- 语义影响：极低概率哈希碰撞时，无法同时保留两个同 hash 不同名条目（行为由现有 create 路径决定）。

---

## 7. 一致性与恢复边界

- 当前以“结构可持久化 + 路径性能收紧”为主；
- 已有恢复相关字段预留（`seq/version/checksum/checkpoint refs`）；
- 完整崩溃恢复流程（replay/rebuild/orphan cleanup）尚未实现。

---

## 8. 本轮性能相关改动摘要

- inode 分配：全表扫描 -> 线性顺序分配；
- `stat`：双遍历 -> 父目录一次定位 + 一次孩子槽位查询；
- `create/mkdir`：去掉重复父目录查找与重复父 inode 查找；
- 删除冲突桶路径：不再维护 `conflict_bucket/conflict_chain`；
- `create` 持久化：去掉创建后额外 inode 读取；
- `ls`：保持当前层扫描，不下钻；
- `delete` 测试路径：去掉预先 `lookup`。

---

## 9. 下一步

1. 完成目录块落盘与内存命名空间的一致化规则（含刷盘策略）。
2. 增加恢复流程（checkpoint + replay）。
3. 在 SSD 持久化版本上对比 InfiniFS/SingularFS（RocksDB 映射）口径。

---

## 10. 对比实验口径（后续）

与 RocksDB 映射方案对比时，统一关注：

- `stat / ls / create / delete / rename` 延迟
- 写放大
- 内存放大
- 恢复时间（恢复实现后）

当前文档仅保留现行设计，不再记录中间迭代过程。
