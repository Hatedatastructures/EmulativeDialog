#pragma once

#include "Source/Memory/ConversationMemory.hpp"
#include "Source/Storage/SqliteStore.hpp"

namespace EmulativeDialog::Memory {

class SqliteMemoryStorageAdapter final : public MemoryStoragePort {
public:
  explicit SqliteMemoryStorageAdapter(Storage::SqliteStore& Store) noexcept;

  [[nodiscard]] MemoryStorageResult<std::monostate> ApplyAtomically(
      const MemoryMutationBatch& Batch) override;
  [[nodiscard]] MemoryStorageResult<std::vector<StoredMemory>> ListActive(
      const StoredMemory& ScopeTemplate,
      std::chrono::sys_seconds Now) const override;
  [[nodiscard]] MemoryStorageResult<std::vector<StoredMemory>> RecallBackup(
      const StoredMemory& ScopeTemplate,
      std::chrono::sys_seconds Now) const override;

private:
  Storage::SqliteStore& Store_;
};

} // namespace EmulativeDialog::Memory
