/**
 * Copyright (c) 2017-present, Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "glow/Backends/DeviceManager.h"

#include "../../lib/Backends/CPU/CPUDeviceManager.h"
#include "glow/ExecutionEngine/ExecutionEngine.h"

#include "gtest/gtest.h"

#include <chrono>
#include <future>

using namespace glow;
using namespace glow::runtime;

class DeviceManagerTest : public ::testing::TestWithParam<BackendKind> {
public:
  void SetUp() { backendKind = GetParam(); }

  BackendKind backendKind;
};

std::unique_ptr<Module> makeBasicModule(std::string functionName = "main") {
  std::unique_ptr<Module> module = llvm::make_unique<Module>();
  std::unique_ptr<Context> ctx = llvm::make_unique<Context>();

  Function *F = module->createFunction(functionName);
  auto *input = module->createPlaceholder(ElemKind::FloatTy, {1, 32, 32, 3},
                                          functionName + "_input", false);

  auto *FC = F->createFullyConnected(*ctx, "fc", input, 10);
  auto *RU = F->createRELU("relu", FC);
  F->createSave("ret", RU);

  return module;
}

FunctionMapTy
compileFunctions(BackendKind backendKind, Module *module,
                 std::vector<std::unique_ptr<CompiledFunction>> &backing) {
  FunctionMapTy results;
  auto *backend = createBackend(backendKind);
  for (auto *F : module->getFunctions()) {
    backend->optimizeFunction(CompilationMode::Infer, F);
    auto f = backend->compile(F);
    backing.push_back(std::move(f));
    results.emplace(F->getName(), backing.back().get());
  }

  delete backend;
  return results;
}

template <typename ResultType>
std::pair<std::promise<ResultType>, std::future<ResultType>> getFutureHelper() {
  std::promise<ResultType> promise;
  auto future = promise.get_future();
  return std::make_pair(std::move(promise), std::move(future));
}

template <typename ResultType>
void callbackHelper(std::promise<ResultType> &promise, ResultType res,
                    ResultCode result, ResultCode expected) {
  promise.set_value(result == expected ? std::move(res) : ResultType());
}

TEST_P(DeviceManagerTest, Basic) {
  auto module = makeBasicModule();
  std::vector<std::unique_ptr<CompiledFunction>> backing;
  FunctionMapTy functions =
      compileFunctions(backendKind, module.get(), backing);

  auto *device = DeviceManager::createDeviceManager(backendKind, "Basic");
  device->init();

  std::promise<const Module *> promise;
  std::future<const Module *> future;
  std::tie(promise, future) = getFutureHelper<const Module *>();

  device->addNetwork(module.get(), std::move(functions),
                     [&promise](const Module *module, ResultCode result) {
                       callbackHelper(promise, module, result,
                                      ResultCode::Ready);
                     });

  future.wait_for(std::chrono::seconds(2));
  EXPECT_EQ(future.get(), module.get());

  std::unique_ptr<Context> ctx = llvm::make_unique<Context>();
  ctx->allocate(module->getPlaceholders());

  Tensor inputs(ElemKind::FloatTy, {1, 32, 32, 3});
  updateInputPlaceholders(*ctx, {module->getPlaceholderByName("main_input")},
                          {&inputs});

  std::promise<std::unique_ptr<Context>> runPromise;
  std::future<std::unique_ptr<Context>> runFuture;

  std::tie(runPromise, runFuture) = getFutureHelper<std::unique_ptr<Context>>();
  device->runFunction("main", std::move(ctx),
                      [&runPromise](RunIdentifierTy, ResultCode result,
                                    std::unique_ptr<Context> ctx_) {
                        callbackHelper(runPromise, std::move(ctx_), result,
                                       ResultCode::Executed);
                      });

  runFuture.wait_for(std::chrono::seconds(2));

  EXPECT_NE(runFuture.get(), nullptr);
}

TEST_P(DeviceManagerTest, MultiRun) {
  auto module = makeBasicModule();
  std::vector<std::unique_ptr<CompiledFunction>> backing;
  FunctionMapTy functions =
      compileFunctions(backendKind, module.get(), backing);

  auto *device = DeviceManager::createDeviceManager(backendKind, "MultiRun");
  device->init();

  std::promise<const Module *> promise;
  std::future<const Module *> future;
  std::tie(promise, future) = getFutureHelper<const Module *>();
  device->addNetwork(module.get(), std::move(functions),
                     [&promise](const Module *module, ResultCode result) {
                       callbackHelper(promise, module, result,
                                      ResultCode::Ready);
                     });
  future.wait_for(std::chrono::seconds(2));
  EXPECT_EQ(future.get(), module.get());

  std::unique_ptr<Context> ctx1 = llvm::make_unique<Context>();
  std::unique_ptr<Context> ctx2 = llvm::make_unique<Context>();
  ctx1->allocate(module->getPlaceholders());
  ctx2->allocate(module->getPlaceholders());

  PseudoRNG PRNG;
  Tensor inputs1(ElemKind::FloatTy, {1, 32, 32, 3});
  Tensor inputs2(ElemKind::FloatTy, {1, 32, 32, 3});
  inputs1.getHandle().randomize(-12.0, 13.0, PRNG);
  inputs2.getHandle().randomize(-12.0, 13.0, PRNG);

  updateInputPlaceholders(*ctx1, {module->getPlaceholderByName("main_input")},
                          {&inputs1});
  updateInputPlaceholders(*ctx2, {module->getPlaceholderByName("main_input")},
                          {&inputs2});

  std::promise<std::unique_ptr<Context>> runP1, runP2;
  std::future<std::unique_ptr<Context>> runF1, runF2;
  std::tie(runP1, runF1) = getFutureHelper<std::unique_ptr<Context>>();
  std::tie(runP2, runF2) = getFutureHelper<std::unique_ptr<Context>>();

  device->runFunction("main", std::move(ctx1),
                      [&runP1](RunIdentifierTy, ResultCode result,
                               std::unique_ptr<Context> ctx_) {
                        callbackHelper(runP1, std::move(ctx_), result,
                                       ResultCode::Executed);
                      });

  device->runFunction("main", std::move(ctx2),
                      [&runP2](RunIdentifierTy, ResultCode result,
                               std::unique_ptr<Context> ctx_) {
                        callbackHelper(runP2, std::move(ctx_), result,
                                       ResultCode::Executed);
                      });

  ctx1 = runF1.get();
  ctx2 = runF2.get();
  EXPECT_NE(ctx1, ctx2);
}

TEST_P(DeviceManagerTest, MultiFunction) {
  auto module = makeBasicModule("func1");

  std::unique_ptr<Context> ctx1 = llvm::make_unique<Context>();

  Function *F = module->createFunction("func2");
  auto *input = module->getPlaceholderByName("func1_input");
  auto *C = F->createConv(*ctx1, "conv2a", input, 64, 4, 1, 0, 1);
  ctx1->get(llvm::cast<Placeholder>(C->getFilter()))->getHandle().clear(0.3);
  ctx1->get(llvm::cast<Placeholder>(C->getBias()))->getHandle().clear(0.4);
  F->createSave("ret2", C);
  ctx1->allocate(module->getPlaceholders());

  std::vector<std::unique_ptr<CompiledFunction>> backing;
  FunctionMapTy functions =
      compileFunctions(backendKind, module.get(), backing);
  EXPECT_EQ(functions.size(), 2);

  auto *device =
      DeviceManager::createDeviceManager(backendKind, "MultiFunction");
  device->init();

  std::promise<const Module *> promise;
  std::future<const Module *> future;
  std::tie(promise, future) = getFutureHelper<const Module *>();
  device->addNetwork(module.get(), std::move(functions),
                     [&promise](const Module *module, ResultCode result) {
                       callbackHelper(promise, module, result,
                                      ResultCode::Ready);
                     });
  future.wait_for(std::chrono::seconds(2));
  EXPECT_EQ(future.get(), module.get());

  Tensor inputs(ElemKind::FloatTy, {1, 32, 32, 3});
  updateInputPlaceholders(*ctx1, {module->getPlaceholderByName("func1_input")},
                          {&inputs});

  std::unique_ptr<Context> ctx2 = llvm::make_unique<Context>(ctx1->clone());

  std::promise<std::unique_ptr<Context>> runP1, runP2;
  std::future<std::unique_ptr<Context>> runF1, runF2;
  std::tie(runP1, runF1) = getFutureHelper<std::unique_ptr<Context>>();
  std::tie(runP2, runF2) = getFutureHelper<std::unique_ptr<Context>>();

  device->runFunction("func1", std::move(ctx1),
                      [&runP1](RunIdentifierTy, ResultCode result,
                               std::unique_ptr<Context> ctx_) {
                        callbackHelper(runP1, std::move(ctx_), result,
                                       ResultCode::Executed);
                      });

  device->runFunction("func2", std::move(ctx2),
                      [&runP2](RunIdentifierTy, ResultCode result,
                               std::unique_ptr<Context> ctx_) {
                        callbackHelper(runP2, std::move(ctx_), result,
                                       ResultCode::Executed);
                      });

  ctx1 = runF1.get();
  ctx2 = runF2.get();
  EXPECT_NE(ctx1, ctx2);
}

TEST_P(DeviceManagerTest, MultiModule) {
  auto module1 = makeBasicModule("func1");
  auto module2 = makeBasicModule("func2");

  std::vector<std::unique_ptr<CompiledFunction>> backing;
  FunctionMapTy functions1 =
      compileFunctions(backendKind, module1.get(), backing);
  FunctionMapTy functions2 =
      compileFunctions(backendKind, module2.get(), backing);

  auto *device = DeviceManager::createDeviceManager(backendKind, "MultiModule");
  device->init();

  std::promise<const Module *> promise;
  std::future<const Module *> future;
  std::tie(promise, future) = getFutureHelper<const Module *>();
  device->addNetwork(module1.get(), std::move(functions1),
                     [&promise](const Module *module, ResultCode result) {
                       callbackHelper(promise, module, result,
                                      ResultCode::Ready);
                     });
  future.wait_for(std::chrono::seconds(2));
  EXPECT_EQ(future.get(), module1.get());

  std::tie(promise, future) = getFutureHelper<const Module *>();
  device->addNetwork(module2.get(), std::move(functions2),
                     [&promise](const Module *module, ResultCode result) {
                       callbackHelper(promise, module, result,
                                      ResultCode::Ready);
                     });
  future.wait_for(std::chrono::seconds(2));
  EXPECT_EQ(future.get(), module2.get());

  std::unique_ptr<Context> ctx1 = llvm::make_unique<Context>();
  ctx1->allocate(module1->getPlaceholders());
  Tensor inputs(ElemKind::FloatTy, {1, 32, 32, 3});
  updateInputPlaceholders(*ctx1, {module1->getPlaceholderByName("func1_input")},
                          {&inputs});

  std::unique_ptr<Context> ctx2 = llvm::make_unique<Context>();
  ctx2->allocate(module2->getPlaceholders());
  updateInputPlaceholders(*ctx2, {module2->getPlaceholderByName("func2_input")},
                          {&inputs});

  std::promise<std::unique_ptr<Context>> runP1, runP2;
  std::future<std::unique_ptr<Context>> runF1, runF2;
  std::tie(runP1, runF1) = getFutureHelper<std::unique_ptr<Context>>();
  std::tie(runP2, runF2) = getFutureHelper<std::unique_ptr<Context>>();

  device->runFunction("func1", std::move(ctx1),
                      [&runP1](RunIdentifierTy, ResultCode result,
                               std::unique_ptr<Context> ctx_) {
                        callbackHelper(runP1, std::move(ctx_), result,
                                       ResultCode::Executed);
                      });

  device->runFunction("func2", std::move(ctx2),
                      [&runP2](RunIdentifierTy, ResultCode result,
                               std::unique_ptr<Context> ctx_) {
                        callbackHelper(runP2, std::move(ctx_), result,
                                       ResultCode::Executed);
                      });

  ctx1 = runF1.get();
  ctx2 = runF2.get();
  EXPECT_NE(ctx1, ctx2);
}

TEST_P(DeviceManagerTest, ReuseModule) {
  auto module = makeBasicModule("func1");

  std::unique_ptr<Context> ctx1 = llvm::make_unique<Context>();

  Function *F = module->createFunction("func2");
  auto *input = module->getPlaceholderByName("func1_input");
  auto *C = F->createConv(*ctx1, "conv2a", input, 64, 4, 1, 0, 1);
  ctx1->get(llvm::cast<Placeholder>(C->getFilter()))->getHandle().clear(0.3);
  ctx1->get(llvm::cast<Placeholder>(C->getBias()))->getHandle().clear(0.4);
  F->createSave("ret2", C);
  ctx1->allocate(module->getPlaceholders());

  std::vector<std::unique_ptr<CompiledFunction>> backing;
  FunctionMapTy functions =
      compileFunctions(backendKind, module.get(), backing);
  EXPECT_EQ(functions.size(), 2);

  // Split the function map into two parts.
  FunctionMapTy functions2;
  functions2.emplace("func2", std::move(functions["func2"]));
  functions.erase("func2");
  EXPECT_EQ(functions.size(), 1);
  EXPECT_EQ(functions2.size(), 1);

  auto *device = DeviceManager::createDeviceManager(backendKind, "ReuseModule");
  device->init();

  std::promise<const Module *> promise;
  std::future<const Module *> future;
  std::tie(promise, future) = getFutureHelper<const Module *>();
  device->addNetwork(module.get(), std::move(functions),
                     [&promise](const Module *module, ResultCode result) {
                       callbackHelper(promise, module, result,
                                      ResultCode::Ready);
                     });
  future.wait_for(std::chrono::seconds(2));
  EXPECT_EQ(future.get(), module.get());

  std::tie(promise, future) = getFutureHelper<const Module *>();
  device->addNetwork(module.get(), std::move(functions2),
                     [&promise](const Module *module, ResultCode result) {
                       callbackHelper(promise, module, result,
                                      ResultCode::Ready);
                     });
  future.wait_for(std::chrono::seconds(2));
  EXPECT_EQ(future.get(), module.get());

  Tensor inputs(ElemKind::FloatTy, {1, 32, 32, 3});
  updateInputPlaceholders(*ctx1, {module->getPlaceholderByName("func1_input")},
                          {&inputs});

  std::unique_ptr<Context> ctx2 = llvm::make_unique<Context>(ctx1->clone());

  std::promise<std::unique_ptr<Context>> runP1, runP2;
  std::future<std::unique_ptr<Context>> runF1, runF2;
  std::tie(runP1, runF1) = getFutureHelper<std::unique_ptr<Context>>();
  std::tie(runP2, runF2) = getFutureHelper<std::unique_ptr<Context>>();

  device->runFunction("func1", std::move(ctx1),
                      [&runP1](RunIdentifierTy, ResultCode result,
                               std::unique_ptr<Context> ctx_) {
                        callbackHelper(runP1, std::move(ctx_), result,
                                       ResultCode::Executed);
                      });

  device->runFunction("func2", std::move(ctx2),
                      [&runP2](RunIdentifierTy, ResultCode result,
                               std::unique_ptr<Context> ctx_) {
                        callbackHelper(runP2, std::move(ctx_), result,
                                       ResultCode::Executed);
                      });

  ctx1 = runF1.get();
  ctx2 = runF2.get();
  EXPECT_NE(ctx1, ctx2);
}

TEST_P(DeviceManagerTest, AvailableMemory) {
  std::vector<std::unique_ptr<CompiledFunction>> backing;
  std::promise<const Module *> promise;
  std::future<const Module *> future;

  CPUDeviceManager cpuCoreDevice("AvailableMemoryTest", 1);
  cpuCoreDevice.init();

  uint64_t expectedBytes = 1;
  EXPECT_EQ(cpuCoreDevice.getMaximumMemory(), expectedBytes);
  EXPECT_EQ(cpuCoreDevice.getAvailableMemory(), expectedBytes);
  EXPECT_TRUE(cpuCoreDevice.isMemoryAvailable(expectedBytes));
  EXPECT_FALSE(cpuCoreDevice.isMemoryAvailable(expectedBytes + 1));

  auto module = makeBasicModule();
  std::tie(promise, future) = getFutureHelper<const Module *>();
  cpuCoreDevice.addNetwork(
      module.get(), compileFunctions(backendKind, module.get(), backing),
      [&promise](const Module *module, ResultCode result) {
        callbackHelper(promise, module, result, ResultCode::Ready);
      });

  future.wait_for(std::chrono::seconds(2));
  EXPECT_EQ(future.get(), module.get());

  EXPECT_EQ(cpuCoreDevice.getMaximumMemory(), expectedBytes);
  EXPECT_EQ(cpuCoreDevice.getAvailableMemory(), 0);
  EXPECT_FALSE(cpuCoreDevice.isMemoryAvailable(expectedBytes));
  EXPECT_FALSE(cpuCoreDevice.isMemoryAvailable(1));

  // Let's try again.
  auto module2 = makeBasicModule();
  std::tie(promise, future) = getFutureHelper<const Module *>();
  cpuCoreDevice.addNetwork(
      module2.get(), compileFunctions(backendKind, module2.get(), backing),
      [&promise](const Module *module, ResultCode result) {
        callbackHelper(promise, module, result, ResultCode::Ready);
      });

  future.wait_for(std::chrono::seconds(2));
  auto *resultModule = future.get();
  EXPECT_NE(resultModule, module2.get());
  EXPECT_NE(resultModule, module.get());
  EXPECT_EQ(resultModule, nullptr);

  EXPECT_EQ(cpuCoreDevice.getMaximumMemory(), expectedBytes);
  EXPECT_EQ(cpuCoreDevice.getAvailableMemory(), 0);

  // Evict the first network.
  cpuCoreDevice.evictNetwork("main");

  // And try again, this time with available space.
  std::tie(promise, future) = getFutureHelper<const Module *>();
  cpuCoreDevice.addNetwork(
      module2.get(), compileFunctions(backendKind, module2.get(), backing),
      [&promise](const Module *module, ResultCode result) {
        callbackHelper(promise, module, result, ResultCode::Ready);
      });

  future.wait_for(std::chrono::seconds(2));
  EXPECT_EQ(future.get(), module2.get());

  EXPECT_EQ(cpuCoreDevice.getMaximumMemory(), expectedBytes);
  EXPECT_EQ(cpuCoreDevice.getAvailableMemory(), 0);
}

INSTANTIATE_TEST_CASE_P(Interpreter, DeviceManagerTest,
                        ::testing::Values(BackendKind::Interpreter));

#ifdef GLOW_WITH_CPU
INSTANTIATE_TEST_CASE_P(CPU, DeviceManagerTest,
                        ::testing::Values(BackendKind::CPU));
#endif // GLOW_WITH_CPU
