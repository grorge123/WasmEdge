#include "executor/coredump.h"
#include "ast/section.h"
#include "common/errcode.h"
#include "common/types.h"
#include "loader/serialize.h"
#include "runtime/stackmgr.h"
#include "spdlog/spdlog.h"
#include <cstdint>
#include <cstring>
#include <vector>

#define ENCODE_LEB128(Content, Value)                                          \
  {                                                                            \
    auto TempValue = Value;                                                    \
    do {                                                                       \
      uint8_t Byte = TempValue & 0x7F;                                         \
      TempValue >>= 7;                                                         \
      if (TempValue != 0) {                                                    \
        Byte |= 0x80;                                                          \
      }                                                                        \
      Content.push_back(Byte);                                                 \
    } while (TempValue != 0);                                                  \
  }

namespace WasmEdge {
namespace Coredump {
void generateCoredump(const Runtime::StackManager &StackMgr) noexcept {
  spdlog::info("Generating coredump...");
  // Generate coredump.
  const auto *CurrentInstance = StackMgr.getModule();
  AST::Module Module{};
  std::vector<Byte> &Magic = Module.getMagic();
  std::string MagicStr("\0asm", 4);
  Magic.insert(Magic.begin(), MagicStr.begin(), MagicStr.end());
  std::vector<Byte> &Version = Module.getVersion();
  // Version must be 1 for support Wasmgdb
  Version.insert(Version.begin(), {0x01, 0x00, 0x00, 0x00});

  Module.getCustomSections().emplace_back(createCore());
  Module.getCustomSections().emplace_back(
      createCorestack(StackMgr.getFramesSpan(), StackMgr.getValueSpan()));
  // Module.getDataSection() =
  //     createData(CurrentInstance->getOwnedDataInstances());
  // Module.getCustomSections().emplace_back(createCoremodules());
  // Module.getCustomSections().emplace_back(createCoreinstances());
  Module.getMemorySection() =
      createMemory(CurrentInstance->getMemoryInstances());
  Module.getGlobalSection() =
      createGlobals(CurrentInstance->getGlobalInstances());
  const Configure Config;
  Loader::Serializer Ser(Config);
  auto Res = Ser.serializeModule(Module);
  if (Res.has_value()) {
    spdlog::info("Coredump generated.");
    std::time_t Time = std::time(nullptr);
    std::string CoredumpPath = "coredump." + std::to_string(Time);
    std::ofstream File(CoredumpPath, std::ios::out | std::ios::binary);
    if (File.is_open()) {
      File.write(reinterpret_cast<const char *>(Res->data()),
                 static_cast<uint32_t>(Res->size()));
      File.close();
    } else {
      spdlog::error("Failed to generate coredump.");
      assumingUnreachable();
    }
  } else {
    spdlog::error("Failed to serialize coredump.");
    assumingUnreachable();
  }

  return;
}
AST::CustomSection createCore() {
  AST::CustomSection Core;
  Core.setName("core");
  auto &Content = Core.getContent();
  Content.insert(Content.begin(), {0x00, 0x00});
  return Core;
}

AST::CustomSection
createCorestack(Span<const Runtime::StackManager::Frame> Frames,
                Span<const Runtime::StackManager::Value> ValueStack) {
  AST::CustomSection CoreStack;
  CoreStack.setName("corestack");
  auto &Content = CoreStack.getContent();
  // thread-info type 0x00 for wasmedbg
  Content.push_back(0x00);

  // Thread name size
  Content.push_back(0x04);

  std::string ThreadName = "main";
  Content.insert(Content.end(), ThreadName.begin(), ThreadName.end());
  auto FramesSize = Frames.size() - 1;
  ENCODE_LEB128(Content, FramesSize)
  for (size_t Idx = FramesSize; Idx > 0; Idx--) {
    if (Frames[Idx].Module == nullptr) {
      continue;
    }
    // frame type 0x00 for wasmedbg
    Content.push_back(0x00);
    // TODO: fix main Funcidx is 0
    auto Funcidx = Frames[Idx].From->getTargetIndex();
    auto Codeoffset = Frames[Idx].From->getOffset();
    uint32_t Lstart = Frames[Idx].VPos - Frames[Idx].Locals;
    uint32_t Lend = Frames[Idx].VPos;
    uint32_t Vstart = Frames[Idx].VPos;
    uint32_t Vend = (Idx != FramesSize)
                        ? Frames[Idx + 1].VPos - Frames[Idx + 1].Locals
                        : static_cast<uint32_t>(ValueStack.size());

    uint32_t Lsize = Lend - Lstart;
    uint32_t Vsize = Vend - Vstart;
    assuming(Lstart + Lsize <= ValueStack.size());
    auto Locals = Span<const Runtime::StackManager::Value>(
        ValueStack.begin() + Lstart, Lsize);
    assuming(Vstart + Vsize <= ValueStack.size());
    // auto Stacks = Span<const Runtime::StackManager::Value>(
    //     ValueStack.begin() + Vstart, Vsize);
    ENCODE_LEB128(Content, Funcidx)
    ENCODE_LEB128(Content, Codeoffset)
    // locals size
    ENCODE_LEB128(Content, Frames[Idx].Locals)
    // stack size
    ENCODE_LEB128(Content, Vsize)
    for (auto &Iter : Locals) {
      // 0x7F implies i32, since it doesn't support i128 and wasmgdb not support
      // i64
      Content.push_back(0x7F);
      auto Value = Iter.unwrap();
      std::vector<Byte> ValueBytes(4);
      std::memcpy(ValueBytes.data(), &Value, sizeof(int64_t));
      Content.insert(Content.end(), ValueBytes.begin(), ValueBytes.end());
    }
    // Stacks is not supported by wasmedbg
    // for (auto &Iter : Stacks) {
    // 0x7F implies i32, since it doesn't support i128 and wasmgdb not support
    // i64
    // Content.push_back(0x7F); auto Value = Iter.unwrap();
    //   std::vector<Byte> ValueBytes(4);
    //   std::memcpy(ValueBytes.data(), &Value, sizeof(int64_t));
    //   ValueBytes[0],
    //                ValueBytes[1], ValueBytes[2], ValueBytes[3]);
    //   Content.insert(Content.end(), ValueBytes.begin(), ValueBytes.end());
    // }
  }
  return CoreStack;
}

AST::DataSection
createData(Span<const Runtime::Instance::DataInstance *const> DataInstances) {
  AST::DataSection Data;
  AST::DataSegment Seg;
  auto &Content = Seg.getData();
  for (auto &Data : DataInstances) {
    Content.insert(Content.end(), Data->getData().begin(),
                   Data->getData().end());
  }
  Data.getContent().push_back(Seg);
  return Data;
}
AST::GlobalSection createGlobals(
    Span<const Runtime::Instance::GlobalInstance *const> GlobalInstances) {
  AST::GlobalSection Globals;
  for (auto &Global : GlobalInstances) {
    AST::GlobalSegment Seg;
    Seg.getGlobalType() = Global->getGlobalType();
    Seg.getExpr().getInstrs() = {
        WasmEdge::AST::Instruction(WasmEdge::OpCode::End)};
    Globals.getContent().push_back(Seg);
  }
  return Globals;
}
AST::MemorySection createMemory(
    Span<const Runtime::Instance::MemoryInstance *const> MemoryInstances) {
  AST::MemorySection Memory;
  auto &Content = Memory.getContent();
  Content.push_back(MemoryInstances[0]->getMemoryType());
  return Memory;
}

AST::CustomSection createCoremodules() {
  AST::CustomSection CoreModules;
  CoreModules.setName("coremodules");
  auto &Content = CoreModules.getContent();
  Content.push_back(0x00);
  return CoreModules;
}

AST::CustomSection createCoreinstances() {
  AST::CustomSection CoreInstances;
  CoreInstances.setName("coreinstances");
  auto &Content = CoreInstances.getContent();
  Content.push_back(0x00);
  return CoreInstances;
}

} // namespace Coredump
} // namespace WasmEdge
