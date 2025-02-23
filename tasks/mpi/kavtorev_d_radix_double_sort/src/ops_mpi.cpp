#include "mpi/kavtorev_d_radix_double_sort/include/ops_mpi.hpp"

#include <boost/mpi.hpp>
#include <cmath>
#include <queue>

namespace mpi = boost::mpi;
using namespace kavtorev_d_radix_double_sort;

bool RadixSortSequential::PreProcessingImpl() {
  data_.resize(n);
  auto* arr = reinterpret_cast<double*>(task_data->inputs[1]);
  std::copy(arr, arr + n, data_.begin());

  return true;
}

bool RadixSortSequential::ValidationImpl() {
  bool is_valid = true;
  n = *(reinterpret_cast<int*>(task_data->inputs[0]));
  if (task_data->inputs_count[0] != 1 || task_data->inputs_count[1] != static_cast<size_t>(n) ||
      task_data->outputs_count[0] != static_cast<size_t>(n)) {
    is_valid = false;
  }

  return is_valid;
}

bool RadixSortSequential::RunImpl() {
  radix_sort_doubles(data_);
  return true;
}

bool RadixSortSequential::PostProcessingImpl() {
  auto* out = reinterpret_cast<double*>(task_data->outputs[0]);
  std::copy(data_.begin(), data_.end(), out);
  return true;
}

void RadixSortSequential::radix_sort_doubles(std::vector<double>& data_) {
  size_t n_ = data_.size();
  std::vector<uint64_t> keys_(n_);
  for (size_t i = 0; i < n_; ++i) {
    uint64_t u;
    std::memcpy(&u, &data_[i], sizeof(double));
    if ((u & 0x8000000000000000ULL) != 0) {
      u = ~u;
    } else {
      u |= 0x8000000000000000ULL;
    }
    keys_[i] = u;
  }

  radix_sort_uint64(keys_);

  for (size_t i = 0; i < n_; ++i) {
    uint64_t u = keys_[i];
    if ((u & 0x8000000000000000ULL) != 0) {
      u &= ~0x8000000000000000ULL;
    } else {
      u = ~u;
    }
    std::memcpy(&data_[i], &u, sizeof(double));
  }
}

void RadixSortSequential::radix_sort_uint64(std::vector<uint64_t>& keys_) {
  const int BITS = 64;
  const int RADIX = 256;
  std::vector<uint64_t> temp(keys_.size());

  for (int shift = 0; shift < BITS; shift += 8) {
    size_t count[RADIX + 1] = {0};
    for (size_t i = 0; i < keys_.size(); ++i) {
      uint8_t byte = (keys_[i] >> shift) & 0xFF;
      ++count[byte + 1];
    }
    for (int i = 0; i < RADIX; ++i) {
      count[i + 1] += count[i];
    }
    for (size_t i = 0; i < keys_.size(); ++i) {
      uint8_t byte = (keys_[i] >> shift) & 0xFF;
      temp[count[byte]++] = keys_[i];
    }
    keys_.swap(temp);
  }
}

bool RadixSortParallel::PreProcessingImpl() {
  if (world.rank() == 0) {
    data_.resize(n);
    auto* arr = reinterpret_cast<double*>(task_data->inputs[1]);
    std::copy(arr, arr + n, data_.begin());
  }

  return true;
}

bool RadixSortParallel::ValidationImpl() {
  bool is_valid = true;
  if (world.rank() == 0) {
    n = *(reinterpret_cast<int*>(task_data->inputs[0]));
    if (task_data->inputs_count[0] != 1 || task_data->inputs_count[1] != static_cast<size_t>(n) ||
        task_data->outputs_count[0] != static_cast<size_t>(n)) {
      is_valid = false;
    }
  }
  mpi::broadcast(world, is_valid, 0);
  mpi::broadcast(world, n, 0);
  return is_valid;
}

bool RadixSortParallel::RunImpl() {
  int rank = world.rank();
  int size = world.size();
  int local_n = n / size;
  int remainder = n % size;

  std::vector<int> counts(size);
  std::vector<int> displs(size);
  if (rank == 0) {
    for (int i = 0; i < size; ++i) {
      counts[i] = local_n + (i < remainder ? 1 : 0);
    }
    displs[0] = 0;
    for (int i = 1; i < size; ++i) {
      displs[i] = displs[i - 1] + counts[i - 1];
    }
  }

  mpi::broadcast(world, counts, 0);
  mpi::broadcast(world, displs, 0);

  std::vector<double> local_data(counts[rank]);
  mpi::scatterv(world, (rank == 0 ? data_.data() : (double*)nullptr), counts, displs, local_data.data(), counts[rank],
                0);
  radix_sort_doubles(local_data);

  int steps = 0;
  {
    int tmp = size;
    while (tmp > 1) {
      tmp = (tmp + 1) / 2;
      steps++;
    }
  }

  int group_size = 1;
  for (int step = 0; step < steps; ++step) {
    int partner_rank = rank + group_size;
    int group_step_size = group_size * 2;
    bool is_merger = (rank % group_step_size == 0);
    bool has_partner = (partner_rank < size);

    if (is_merger && has_partner) {
      int partner_size;
      world.recv(partner_rank, 0, partner_size);

      std::vector<double> partner_data(partner_size);
      world.recv(partner_rank, 1, partner_data.data(), partner_size);

      std::vector<double> merged;
      merged.reserve(local_data.size() + partner_data.size());
      std::merge(local_data.begin(), local_data.end(), partner_data.begin(), partner_data.end(),
                 std::back_inserter(merged));
      local_data.swap(merged);
    } else if (!is_merger && (rank % group_step_size == group_size)) {
      int receiver = rank - group_size;
      int my_size = (int)local_data.size();
      world.send(receiver, 0, my_size);
      world.send(receiver, 1, local_data.data(), my_size);
      local_data.clear();
    }

    group_size *= 2;
  }

  if (rank == 0) {
    data_.swap(local_data);
  }

  return true;
}

bool RadixSortParallel::PostProcessingImpl() {
  if (world.rank() == 0) {
    auto* out = reinterpret_cast<double*>(task_data->outputs[0]);
    std::copy(data_.begin(), data_.end(), out);
  }

  return true;
}

void RadixSortParallel::radix_sort_doubles(std::vector<double>& data_) {
  size_t n_ = data_.size();
  std::vector<uint64_t> keys_(n_);
  for (size_t i = 0; i < n_; ++i) {
    uint64_t u;
    std::memcpy(&u, &data_[i], sizeof(double));
    if ((u & 0x8000000000000000ULL) != 0) {
      u = ~u;
    } else {
      u |= 0x8000000000000000ULL;
    }
    keys_[i] = u;
  }

  radix_sort_uint64(keys_);

  for (size_t i = 0; i < n_; ++i) {
    uint64_t u = keys_[i];
    if ((u & 0x8000000000000000ULL) != 0) {
      u &= ~0x8000000000000000ULL;
    } else {
      u = ~u;
    }
    std::memcpy(&data_[i], &u, sizeof(double));
  }
}

void RadixSortParallel::radix_sort_uint64(std::vector<uint64_t>& keys_) {
  const int BITS = 64;
  const int RADIX = 256;
  std::vector<uint64_t> temp(keys_.size());

  for (int shift = 0; shift < BITS; shift += 8) {
    size_t count[RADIX + 1] = {0};
    for (size_t i = 0; i < keys_.size(); ++i) {
      uint8_t byte = (keys_[i] >> shift) & 0xFF;
      ++count[byte + 1];
    }
    for (int i = 0; i < RADIX; ++i) {
      count[i + 1] += count[i];
    }
    for (size_t i = 0; i < keys_.size(); ++i) {
      uint8_t byte = (keys_[i] >> shift) & 0xFF;
      temp[count[byte]++] = keys_[i];
    }
    keys_.swap(temp);
  }
}