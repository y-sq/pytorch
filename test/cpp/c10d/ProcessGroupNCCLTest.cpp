#include <chrono>
#include <iostream>

#include <torch/csrc/distributed/c10d/FileStore.hpp>
#include <torch/csrc/distributed/c10d/ProcessGroupNCCL.hpp>
#include "CUDATest.hpp"
#include "TestUtils.hpp"
#include "c10d/Types.hpp"

#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/CUDAStream.h>
#include <c10/util/irange.h>

#include <gtest/gtest.h>
#include <torch/csrc/autograd/profiler.h>

using namespace c10d::test;

using at::cuda::CUDAStream;

class NCCLTestBase {
 public:
  NCCLTestBase(
      const std::string& path,
      const std::chrono::milliseconds pgTimeout =
          c10d::kProcessGroupNCCLDefaultTimeout)
      : path_(path), pgTimeout_(pgTimeout) {}

  NCCLTestBase(NCCLTestBase&& other) {
    path_ = std::move(other.path_);
    pg_ = std::move(other.pg_);
  }

  std::shared_ptr<::c10d::ProcessGroupNCCL> getProcessGroup() {
    return pg_;
  }

  ::c10::intrusive_ptr<::c10d::Store>& getProcessGroupStore() {
    return store_;
  }

  void initialize(
      int rank,
      int size,
      c10::optional<::std::shared_ptr<::c10d::ProcessGroupNCCL>> split_from =
          c10::nullopt) {
    store_ = c10::make_intrusive<::c10d::FileStore>(path_, size);

    c10::intrusive_ptr<c10d::ProcessGroupNCCL::Options> opts =
        c10::make_intrusive<c10d::ProcessGroupNCCL::Options>();
    opts->timeout = pgTimeout_;
    setenv(
        c10d::TORCH_ENABLE_NCCL_HEALTH_CHECK[0].c_str(),
        "1",
        /* overwrite */ 1);
#ifdef NCCL_HAS_COMM_SPLIT
    if (split_from) {
      opts->split_from = *split_from;
      opts->split_color = ++color_;
    }
#endif
    pg_ = std::unique_ptr<::c10d::ProcessGroupNCCL>(
        new ::c10d::ProcessGroupNCCL(store_, rank, size, std::move(opts)));
  }

 protected:
  std::string path_;
  std::shared_ptr<::c10d::ProcessGroupNCCL> pg_;
  std::chrono::milliseconds pgTimeout_;
  ::c10::intrusive_ptr<::c10d::Store> store_;
  int color_{1};
};

class NCCLTest : public NCCLTestBase {
 public:
  NCCLTest(
      const std::string& path,
      int worldSize,
      std::chrono::milliseconds pgTimeout =
          c10d::kProcessGroupNCCLDefaultTimeout,
      int inputDim = 3)
      : NCCLTestBase(path, pgTimeout),
        numDevices_(cudaNumDevices()),
        worldSize_(worldSize) {
    // Each device has a single tensor to perf the NCCL op
    ::at::globalContext().lazyInitCUDA();
    tensors_.resize(numDevices_);
    inputs_.resize(numDevices_);
    outputs_.resize(numDevices_);
    at::cuda::OptionalCUDAGuard deviceGuard;
    for (const auto i : c10::irange(numDevices_)) {
      deviceGuard.set_index(i);
      tensors_[i] = at::empty({inputDim, inputDim}, at::kCUDA);
      inputs_[i].resize(worldSize_ * numDevices_);
      outputs_[i].resize(worldSize_ * numDevices_);
      for (auto j = 0; j < worldSize_ * numDevices_; ++j) {
        inputs_[i][j] = at::empty({inputDim, inputDim}, at::kCUDA);
        outputs_[i][j] = at::empty({inputDim, inputDim}, at::kCUDA);
      }
    }

    // Allocate a stream per device.
    //
    // The "current stream" is set globally per device in THC, so we
    // can't make two tensors on the same device use different streams
    // and pass this along to the collective (since it uses the THC
    // getters to retrieve the current stream).
    //
    streams_.reserve(numDevices_);
    for (const auto i : c10::irange(numDevices_)) {
      deviceGuard.set_index(i);
      streams_.push_back(at::cuda::getStreamFromPool());
    }
  }

  void wait(
      c10::intrusive_ptr<c10d::Work>& work,
      std::chrono::milliseconds timeout = kNoTimeout) {
    c10::cuda::CUDAMultiStreamGuard guard(streams_);
    work->wait(timeout);
  }

  std::vector<at::Tensor> getTensors() {
    std::vector<at::Tensor> outputs(numDevices_);

    // For the duration of this function, make THC use our streams
    c10::cuda::CUDAMultiStreamGuard guard(streams_);

    // Copy inputs to outputs
    for (const auto i : c10::irange(numDevices_)) {
      C10_CUDA_CHECK(cudaStreamSynchronize(streams_[i].stream()));
      outputs[i] = tensors_[i].cpu();
    }

    return outputs;
  }

  std::vector<std::vector<at::Tensor>> getInputTensors() {
    return getTensorLists(inputs_);
  }
  std::vector<std::vector<at::Tensor>> getOutputTensors() {
    return getTensorLists(outputs_);
  }

  int numDevices() const {
    return numDevices_;
  }

 private:
  std::vector<std::vector<at::Tensor>> getTensorLists(
      std::vector<std::vector<at::Tensor>>& tensor_lists) {
    std::vector<std::vector<at::Tensor>> outputs(numDevices_);
    for (auto& output : outputs) {
      output = std::vector<at::Tensor>(worldSize_ * numDevices_);
    }

    // For the duration of this function, make THC use our streams
    c10::cuda::CUDAMultiStreamGuard guard(streams_);

    // Copy inputs to outputs
    for (const auto i : c10::irange(numDevices_)) {
      C10_CUDA_CHECK(cudaStreamSynchronize(streams_[i].stream()));
      for (auto j = 0; j < worldSize_ * numDevices_; ++j) {
        outputs[i][j] = tensor_lists[i][j].cpu();
      }
    }
    return outputs;
  }

 protected:
  // Launches sleep on every CUDA device
  void launchDeviceSleep() {
    at::cuda::OptionalCUDAGuard deviceGuard;
    for (const auto i : c10::irange(numDevices_)) {
      deviceGuard.set_index(i);
      cudaSleep(streams_[i], 2000 * 1000 * 1000);
    }
  }

  // Launches value initialization for every tensor
  void valueInitialization() {
    at::cuda::OptionalCUDAGuard deviceGuard;
    for (const auto i : c10::irange(numDevices_)) {
      deviceGuard.set_index(i);
      tensors_[i].fill_(pg_->getRank() * numDevices_ + i);
    }
  }

  at::Tensor to_sparse_row_indices_format(at::Tensor& tensor) {
    // Get the indices of all non-zero elements in the dense tensor
    // Get the unique row indices of the non-zero elements
    auto row_indices = std::get<0>(
        at::_unique(tensor.nonzero().select(/*dim=*/1, /*index=*/0)));
    at::Tensor sparse_values = tensor.index_select(
        /*dim=*/0, row_indices); // get the values at the non-zero indices
    return at::sparse_coo_tensor(
               row_indices.unsqueeze(0), sparse_values, tensor.sizes())
        .to(tensor.device());
  }

  // Launches value initialization for every sparse tensor
  void valueInitializationForSparse() {
    at::cuda::OptionalCUDAGuard deviceGuard;
    for (const auto i : c10::irange(numDevices_)) {
      deviceGuard.set_index(i);
      tensors_[i].fill_(pg_->getRank() * numDevices_ + i + 1);
      // Convert the dense tensor to a sparse tensor in COO row format
      tensors_[i] = to_sparse_row_indices_format(tensors_[i]);
    }
  }

  const int numDevices_;
  int worldSize_;
  std::vector<at::Tensor> tensors_;
  std::vector<std::vector<at::Tensor>> inputs_;
  std::vector<std::vector<at::Tensor>> outputs_;
  std::vector<CUDAStream> streams_;
};

class AllreduceNCCLTest : public NCCLTest {
 public:
  AllreduceNCCLTest(const std::string& path, int worldSize)
      : NCCLTest(path, worldSize) {}

  c10::intrusive_ptr<c10d::Work> run() {
    // For the duration of this function, make THC use our streams
    c10::cuda::CUDAMultiStreamGuard guard(streams_);

    launchDeviceSleep();
    valueInitialization();

    using namespace torch::autograd::profiler;
    // Make sure enabling profile does not make any issue. Note, in single
    // process multi-device mode we do not expect any events be populated for
    // collective operations, since profiling for that mode is not supported.
    enableProfilerLegacy(ProfilerConfig(ProfilerState::CPU));
    auto results = pg_->allreduce(tensors_);
    disableProfilerLegacy();
    return results;
  }
};

class SparseAllreduceNCCLTest : public NCCLTest {
 public:
  SparseAllreduceNCCLTest(const std::string& path, int worldSize, int inputDim)
      : NCCLTest(
            path,
            worldSize,
            c10d::kProcessGroupNCCLDefaultTimeout,
            inputDim) {}

  c10::intrusive_ptr<c10d::Work> run() {
    // For the duration of this function, make THC use our streams
    c10::cuda::CUDAMultiStreamGuard guard(streams_);
    launchDeviceSleep();
    valueInitializationForSparse();
    auto results = pg_->allreduce_sparse(tensors_);
    return results;
  }
};

class BroadcastNCCLTest : public NCCLTest {
 public:
  BroadcastNCCLTest(const std::string& path, int worldSize)
      : NCCLTest(path, worldSize) {}

  c10::intrusive_ptr<c10d::Work> run(int rootRank, int rootTensor) {
    // For the duration of this function, make THC use our streams
    c10::cuda::CUDAMultiStreamGuard guard(streams_);

    launchDeviceSleep();
    valueInitialization();

    ::c10d::BroadcastOptions options;
    options.rootRank = rootRank;
    options.rootTensor = rootTensor;
    return pg_->broadcast(tensors_, options);
  }
};

class ReduceNCCLTest : public NCCLTest {
 public:
  ReduceNCCLTest(const std::string& path, int worldSize)
      : NCCLTest(path, worldSize) {}

  c10::intrusive_ptr<c10d::Work> run(int rootRank, int rootTensor) {
    // For the duration of this function, make THC use our streams
    c10::cuda::CUDAMultiStreamGuard guard(streams_);

    launchDeviceSleep();
    valueInitialization();

    ::c10d::ReduceOptions options;
    options.rootRank = rootRank;
    options.rootTensor = rootTensor;
    return pg_->reduce(tensors_, options);
  }
};

class AllgatherNCCLTest : public NCCLTest {
 public:
  AllgatherNCCLTest(const std::string& path, int worldSize)
      : NCCLTest(path, worldSize) {}

  c10::intrusive_ptr<c10d::Work> run() {
    // For the duration of this function, make THC use our streams
    c10::cuda::CUDAMultiStreamGuard guard(streams_);

    launchDeviceSleep();
    valueInitialization();

    return pg_->allgather(outputs_, tensors_);
  }
};

class AllgatherBaseNCCLTest : public NCCLTest {
 public:
  AllgatherBaseNCCLTest(const std::string& path, int worldSize)
      : NCCLTest(path, worldSize) {
    output_tensor_ = at::empty({worldSize_, 3, 3}, at::kCUDA);
  }

  c10::intrusive_ptr<c10d::Work> run() {
    // For the duration of this function, make THC use our streams
    c10::cuda::CUDAMultiStreamGuard guard(streams_);

    launchDeviceSleep();
    valueInitialization();
    // contains at least one element otherwise wouldn't run.
    // this is a flattened allgather, hence one rank contributes
    // only 1 tensor, regardless of number of devices
    return pg_->_allgather_base(output_tensor_, tensors_[0]);
  }

  at::Tensor getOutputTensor() {
    c10::cuda::CUDAMultiStreamGuard guard(streams_);
    return output_tensor_.cpu();
  }

  at::Tensor getInputTensor() {
    c10::cuda::CUDAMultiStreamGuard guard(streams_);
    return tensors_[0].cpu();
  }

 private:
  at::Tensor output_tensor_;
};

struct ReduceScatterNCCLTest : NCCLTest {
  ReduceScatterNCCLTest(const std::string& path, int worldSize)
      : NCCLTest(path, worldSize) {}

  c10::intrusive_ptr<c10d::Work> run() {
    // For the duration of this function, make THC use our streams
    c10::cuda::CUDAMultiStreamGuard guard(streams_);

    at::cuda::OptionalCUDAGuard deviceGuard;
    launchDeviceSleep();

    // Launch value initialization for every tensor
    for (const auto i : c10::irange(numDevices_)) {
      deviceGuard.set_index(i);
      for (auto j = 0; j < worldSize_ * numDevices_; ++j) {
        inputs_[i][j].fill_(
            pg_->getRank() * numDevices_ * worldSize_ + i * worldSize_ + j);
      }
    }

    return pg_->reduce_scatter(tensors_, inputs_);
  }
};

class ReduceScatterBaseNCCLTest : public NCCLTest {
 public:
  ReduceScatterBaseNCCLTest(const std::string& path, int worldSize)
      : NCCLTest(path, worldSize) {
    output_tensor_ = at::empty({1}, at::kCUDA);
    input_tensor_ = at::empty({worldSize}, at::kCUDA);
    for (const auto i : c10::irange(worldSize)) {
      input_tensor_[i] = i;
    }
  }

  c10::intrusive_ptr<c10d::Work> run() {
    // For the duration of this function, make THC use our streams
    at::cuda::CUDAMultiStreamGuard guard(streams_);

    launchDeviceSleep();
    return pg_->_reduce_scatter_base(output_tensor_, input_tensor_);
  }

  at::Tensor getOutputTensor() {
    at::cuda::CUDAMultiStreamGuard guard(streams_);
    return output_tensor_.cpu();
  }

  at::Tensor getInputTensor() {
    at::cuda::CUDAMultiStreamGuard guard(streams_);
    return input_tensor_.cpu();
  }

 private:
  at::Tensor output_tensor_;
  at::Tensor input_tensor_;
};

void testAllreduce(const std::string& path, int rank, int size) {
  auto test = AllreduceNCCLTest(path, size);
  test.initialize(rank, size);
  auto work = test.run();
  // Wait for work to finish
  test.wait(work);

  // Validation
  const int totalNumGPUs = test.numDevices() * size;
  const auto expected = (totalNumGPUs * (totalNumGPUs - 1)) / 2;
  const auto tensors = test.getTensors();
  for (const auto& tensor : tensors) {
    const auto* const data = tensor.data_ptr<float>();
    for (const auto k : c10::irange(tensor.numel())) {
      EXPECT_EQ(data[k], expected)
          << "Allreduce outputs do not match expected outputs";
    }
  }
}

void testSparseAllreduce(const std::string& path, int rank, int size) {
  const int inputDim = 3;
  auto test = SparseAllreduceNCCLTest(path, size, inputDim);
  test.initialize(rank, size);
  auto work = test.run();
  // Wait for work to finish
  test.wait(work);

  const auto input_tensors = test.getTensors();

  // validate the work output is same as tensor
  auto output_tensor = work->result();
  // Validation
  int totalNumGPUs = test.numDevices() * size;
  // Add one since we are seeding with an additional 1 to prevent empty tensors
  totalNumGPUs++;
  const auto expected = (totalNumGPUs * (totalNumGPUs - 1)) / 2;
  for (const auto i : c10::irange(input_tensors.size())) {
    const auto& tensor = input_tensors[i];

    // validate the tensor is sparse
    EXPECT_EQ(tensor.is_sparse(), true);

    auto indices = tensor._indices();
    auto values = tensor._values();

    // validate indices are expected size
    auto sizes = indices.sizes();
    EXPECT_EQ(sizes.size(), 2);
    if (sizes[0] == 1) {
      // row indices
      EXPECT_EQ(sizes[1], inputDim);
    } else if (sizes[0] == 2) {
      // coordinate indices
      EXPECT_EQ(sizes[1], inputDim * inputDim);
    }

    // validate all tensor values are expected value
    const auto* const data = values.data_ptr<float>();
    for (const auto k : c10::irange(values.numel())) {
      EXPECT_EQ(data[k], expected)
          << "Allreduce outputs do not match expected outputs";
    }

    // expect the input and output tensors should be the same
    auto input_dense = tensor.to_dense();
    auto output_dense = output_tensor[i].to(input_dense.device()).to_dense();
    EXPECT_TRUE(input_dense.allclose(output_dense));
  }
}

void testSparseAllreduceLarge(const std::string& path, int rank, int size) {
  const int inputDim = 2500;
  auto test = SparseAllreduceNCCLTest(path, size, inputDim);
  test.initialize(rank, size);
  auto work = test.run();
  // Wait for work to finish
  test.wait(work);

  const auto input_tensors = test.getTensors();

  // validate the work output is same as tensor
  auto output_tensor = work->result();
  // Validation
  int totalNumGPUs = test.numDevices() * size;
  // Add one since we are seeding with an additional 1 to prevent empty tensors
  totalNumGPUs++;
  const auto expected = (totalNumGPUs * (totalNumGPUs - 1)) / 2;
  for (const auto i : c10::irange(input_tensors.size())) {
    const auto& tensor = input_tensors[i];

    // validate the tensor is sparse
    EXPECT_EQ(tensor.is_sparse(), true);

    auto indices = tensor._indices();
    auto values = tensor._values();

    // validate indices are expected size
    auto sizes = indices.sizes();
    EXPECT_EQ(sizes.size(), 2);
    if (sizes[0] == 1) {
      // row indices
      EXPECT_EQ(sizes[1], inputDim);
    } else if (sizes[0] == 2) {
      // coordinate indices
      EXPECT_EQ(sizes[1], inputDim * inputDim);
    }

    // validate all tensor values are expected value
    const auto* const data = values.data_ptr<float>();
    for (const auto k : c10::irange(values.numel())) {
      EXPECT_EQ(data[k], expected)
          << "Allreduce outputs do not match expected outputs";
    }

    // expect the input and output tensors should be the same
    auto input_dense = tensor.to_dense();
    auto output_dense = output_tensor[i].to(input_dense.device()).to_dense();
    EXPECT_TRUE(input_dense.allclose(output_dense));
  }
}

void testBroadcast(const std::string& path, int rank, int size) {
  auto test = BroadcastNCCLTest(path, size);
  test.initialize(rank, size);

  const int numDevices = test.numDevices();
  // try every permutation of root rank and root tensor
  for (const auto rootRank : c10::irange(size)) {
    for (const auto rootTensor : c10::irange(numDevices)) {
      auto work = test.run(rootRank, rootTensor);

      // wait for work to complete
      test.wait(work);

      // Check results
      const auto expected = (rootRank * numDevices + rootTensor);
      const auto tensors = test.getTensors();
      for (const auto& tensor : tensors) {
        const auto* const data = tensor.data_ptr<float>();
        for (const auto k : c10::irange(tensor.numel())) {
          EXPECT_EQ(data[k], expected)
              << "Broadcast outputs do not match expected outputs";
        }
      }
    }
  }
}

void testReduce(const std::string& path, int rank, int size) {
  auto test = ReduceNCCLTest(path, size);
  test.initialize(rank, size);

  const int numDevices = test.numDevices();
  // try every permutation of root rank and root tensor
  for (const auto rootRank : c10::irange(size)) {
    for (const auto rootTensor : c10::irange(numDevices)) {
      auto work = test.run(rootRank, rootTensor);

      // wait for work to complete
      test.wait(work);

      // Validation
      const int totalNumGPUs = numDevices * size;
      const auto expected = (totalNumGPUs * (totalNumGPUs - 1)) / 2;
      auto tensors = test.getTensors();
      if (rank == rootRank) {
        auto& tensor = tensors[rootTensor];
        auto data = tensor.data_ptr<float>();
        for (const auto k : c10::irange(tensor.numel())) {
          EXPECT_EQ(data[k], expected)
              << "Reduce outputs do not match expected outputs";
        }
      }
    }
  }
}

void testAllgather(const std::string& path, int rank, int size) {
  auto test = AllgatherNCCLTest(path, size);
  test.initialize(rank, size);
  auto work = test.run();
  // Wait for work to finish
  test.wait(work);

  // Validation
  auto tensors = test.getOutputTensors();
  // device index
  for (auto& device : tensors) {
    // rank index
    for (const auto j : c10::irange(device.size())) {
      const auto expected = j;
      auto& tensor = device[j];
      auto data = tensor.data_ptr<float>();
      for (const auto k : c10::irange(tensor.numel())) {
        EXPECT_EQ(data[k], expected)
            << "Allgather outputs do not match expected outputs";
      }
    }
  }
}

void testAllgatherBase(const std::string& path, int rank, int size) {
  auto test = AllgatherBaseNCCLTest(path, size);
  test.initialize(rank, size);
  auto work = test.run();
  // Wait for work to finish
  test.wait(work);
  // Validation
  auto output_tensor = test.getOutputTensor();
  auto input_tensor = test.getInputTensor();

  auto data = output_tensor.data_ptr<float>();

  // Rank index
  for (const auto i : c10::irange(output_tensor.numel())) {
    // expected is i // input.numel() <- rank, and each rank contributed rank *
    // num_gpu
    const auto expected = (i / input_tensor.numel()) * test.numDevices();
    EXPECT_EQ(data[i], expected)
        << "Allgather_base outputs do not match expected outputs";
  }
}
void testReduceScatterBase(const std::string& path, int rank, int size) {
  auto test = ReduceScatterBaseNCCLTest(path, size);
  test.initialize(rank, size);
  auto work = test.run();
  // Wait for work to finish
  test.wait(work);
  // Validation
  auto output_tensor = test.getOutputTensor();
  auto input_tensor = test.getInputTensor();

  auto data = output_tensor.data_ptr<float>();

  // Rank index
  for (const auto i : c10::irange(output_tensor.numel())) {
    // expected is i * input.numel() <- rank, and each rank contributed rank *
    // num_gpu
    const auto expected = size * rank * test.numDevices();
    EXPECT_EQ(data[i], expected)
        << "Reducescatter_base outputs do not match expected outputs";
  }
}

void testReduceScatter(const std::string& path, int rank, int size) {
  auto test = ReduceScatterNCCLTest(path, size);
  test.initialize(rank, size);
  auto work = test.run();
  // Wait for work to finish
  test.wait(work);

  const auto participants = test.numDevices() * size;
  const auto base = (participants * (participants - 1)) / 2;

  // Validation
  auto tensors = test.getTensors();
  // device index
  for (const auto i : c10::irange(tensors.size())) {
    const auto modifier = participants * (rank * participants + i);
    const auto expected = base + modifier;
    auto& tensor = tensors[i];
    auto data = tensor.data_ptr<float>();
    for (const auto j : c10::irange(tensor.numel())) {
      EXPECT_EQ(data[j], expected)
          << "ReduceScatter outputs do not match expected outputs!";
    }
  }
}

void testProcessGroupNCCLHealthCheckFailHelper(
    const std::string& path,
    bool timeout) {
  // simulate world_size > 1 here via threads.
  const int worldSize = 4;
  std::unordered_set<uint64_t> nums;
  auto runTest = [&](int i) {
    NCCLTest test(path, worldSize, std::chrono::milliseconds(3000));
    // Catch error relating to health check failure
    bool error_caught = false;
    try {
      test.initialize(timeout ? 0 : -1, worldSize);
    } catch (const std::exception& e) {
      std::string errMsg = e.what();
      const std::string kTimeoutErr =
          "Failed to initialize NCCL communicator on rank";
      const std::string kInvalidRankErr = "Invalid rank";
      std::string expectedSubstr = timeout ? kTimeoutErr : kInvalidRankErr;
      bool cond = errMsg.find(expectedSubstr) != std::string::npos;
      EXPECT_TRUE(cond);
      error_caught = true;
    }
    EXPECT_TRUE(error_caught);
  };
  std::vector<std::thread> threads;
  threads.reserve(worldSize);
  for (const auto r : c10::irange(worldSize)) {
    threads.emplace_back(std::thread([=]() { runTest(r); }));
  }
  for (auto& t : threads) {
    t.join();
  }
}

void testProcessGroupNCCLHealthCheckFailException(
    const std::string& path,
    int /* unused */,
    int /* unused */) {
  testProcessGroupNCCLHealthCheckFailHelper(path, /* timeout */ false);
}

void testProcessGroupNCCLHealthCheckFailTimeout(
    const std::string& path,
    int /* unused */,
    int /* unused */) {
  testProcessGroupNCCLHealthCheckFailHelper(path, /* timeout */ true);
}

void testSequenceNumInit(
    const std::string& path,
    int /* unused */,
    int /* unused */) {
  // Note: ProcessGroupNCCLTest doesn't support multiprocess testing. So we
  // simulate world_size > 1 here via threads.
  const int worldSize = 2;
  std::mutex m;
  std::unordered_set<uint64_t> nums;
  auto runTest = [&](int i) {
    NCCLTest test(path, worldSize);
    test.initialize(i, worldSize);
    test.getProcessGroup()->setSequenceNumberForGroup();
    std::lock_guard<std::mutex> lock(m);
    auto seqNum = test.getProcessGroup()->getSequenceNumberForGroup();
    nums.insert(seqNum);
  };
  std::vector<std::thread> threads;
  threads.reserve(worldSize);
  for (const auto r : c10::irange(worldSize)) {
    threads.emplace_back(std::thread([=]() { runTest(r); }));
  }
  for (auto& t : threads) {
    t.join();
  }
  EXPECT_EQ(nums.size(), 1);
}

class ProcessGroupNCCLTest : public ::testing::Test {
 protected:
  void SetUp() override {
    c10::initLogging();
    // Use WORLD_SIZE and RANK environmental variables to do multi-node
    // distributed testing
    auto sizeEnv = std::getenv("WORLD_SIZE");
    auto rankEnv = std::getenv("RANK");

    if (sizeEnv && rankEnv) {
      size_ = std::stoi(std::string(sizeEnv));
      rank_ = std::stoi(std::string(rankEnv));
    }
    LOG(INFO) << "Multi-node world size: " << size_ << " rank: " << rank_;
  }

  void TearDown() override {
    // Reset NCCL_BLOCKING_WAIT environment variable after each run.
    ASSERT_TRUE(setenv(c10d::TORCH_NCCL_BLOCKING_WAIT[0].c_str(), "0", 1) == 0);
  }

  bool skipTest() {
    // Skip tests if CUDA is not available.
    if (!at::cuda::is_available()) {
      LOG(INFO) << "CUDA not available, skipping test";
      return true;
    }
    return false;
  }

  int size_{1};
  int rank_{0};
};

TEST_F(ProcessGroupNCCLTest, testAllreduce) {
  if (skipTest()) {
    return;
  }
  {
    TemporaryFile file;
    testAllreduce(file.path, rank_, size_);
  }
}

TEST_F(ProcessGroupNCCLTest, testBroadcast) {
  if (skipTest()) {
    return;
  }
  {
    TemporaryFile file;
    testBroadcast(file.path, rank_, size_);
  }
}

TEST_F(ProcessGroupNCCLTest, testReduce) {
  if (skipTest()) {
    return;
  }
  {
    TemporaryFile file;
    testReduce(file.path, rank_, size_);
  }
}

TEST_F(ProcessGroupNCCLTest, testAllgather) {
  if (skipTest()) {
    return;
  }
  {
    TemporaryFile file;
    testAllgather(file.path, rank_, size_);
  }
}

TEST_F(ProcessGroupNCCLTest, testAllgatherBase) {
  if (skipTest()) {
    return;
  }
  {
    TemporaryFile file;
    testAllgatherBase(file.path, rank_, size_);
  }
}

TEST_F(ProcessGroupNCCLTest, testReduceScatter) {
  if (skipTest()) {
    return;
  }
  {
    TemporaryFile file;
    testReduceScatter(file.path, rank_, size_);
  }
}

TEST_F(ProcessGroupNCCLTest, testSequenceNumInit) {
  if (skipTest()) {
    return;
  }
  {
    TemporaryFile file;
    testSequenceNumInit(file.path, rank_, size_);
  }
}

TEST_F(ProcessGroupNCCLTest, testProcessGroupNCCLHealthCheckFailTimeout) {
  if (skipTest()) {
    return;
  }
  {
    TemporaryFile file;
    testProcessGroupNCCLHealthCheckFailTimeout(file.path, rank_, size_);
  }
}

TEST_F(ProcessGroupNCCLTest, testProcessGroupNCCLHealthCheckFailException) {
  if (skipTest()) {
    return;
  }
  {
    TemporaryFile file;
    testProcessGroupNCCLHealthCheckFailException(file.path, rank_, size_);
  }
}

TEST_F(ProcessGroupNCCLTest, testReduceScatterBase) {
  if (skipTest()) {
    return;
  }
  {
    TemporaryFile file;
    testReduceScatterBase(file.path, rank_, size_);
  }
}

TEST_F(ProcessGroupNCCLTest, testBackendName) {
  if (skipTest()) {
    return;
  }
  {
    TemporaryFile file;
    auto test = NCCLTestBase(file.path);
    test.initialize(rank_, size_);
    EXPECT_EQ(
        test.getProcessGroup()->getBackendName(),
        std::string(c10d::NCCL_BACKEND_NAME));
  }
}

TEST_F(ProcessGroupNCCLTest, testSplittingCommunicator) {
  if (skipTest()) {
    return;
  }
  TemporaryFile file;
  auto test1 = BroadcastNCCLTest(file.path, size_);
  test1.initialize(rank_, size_);

  auto test2 = BroadcastNCCLTest(file.path, size_);
  test2.initialize(rank_, size_, test1.getProcessGroup());

  // Steal the broadcast test and issue it for both of our groups.
  // This ensures consistent full collective communication.  TODO:
  // maybe refactor the guts rather than copy-pasta, but it may not be
  // worth it.
  for (auto test : {&test1, &test2}) {
    const int numDevices = test->numDevices();
    // try every permutation of root rank and root tensor
    for (const auto rootRank : c10::irange(size_)) {
      for (const auto rootTensor : c10::irange(numDevices)) {
        auto work = test->run(rootRank, rootTensor);
        test->wait(work);

        // Check results
        const auto expected = (rootRank * numDevices + rootTensor);
        const auto tensors = test->getTensors();
        for (const auto& tensor : tensors) {
          const auto* const data = tensor.data_ptr<float>();
          for (const auto k : c10::irange(tensor.numel())) {
            EXPECT_EQ(data[k], expected)
                << "Broadcast outputs do not match expected outputs";
          }
        }
      }
    }
  }

  // Now that we've run full operations on both the original and split process
  // group, ensure we saw exactly as many splits as we expected: 0 in the
  // original process group, and one per device in the second.
  EXPECT_EQ(test2.getProcessGroup()->getCommSplitCounter(), 0);
  EXPECT_EQ(test1.getProcessGroup()->getCommSplitCounter(), test1.numDevices());
}

#ifdef IS_NCCL_EXP
TEST_F(ProcessGroupNCCLTest, testSparseAllreduce) {
  if (skipTest()) {
    return;
  }
  {
    TemporaryFile file;
    testSparseAllreduce(file.path, rank_, size_);
    testSparseAllreduceLarge(file.path, rank_, size_);
  }
}
#endif
