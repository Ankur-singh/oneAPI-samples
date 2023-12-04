//==============================================================
// Copyright Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================
#include <sycl/sycl.hpp>
#include <sycl/ext/intel/fpga_extensions.hpp>
#include <iomanip>
#include <iostream>

#include "exception_handler.hpp"

using namespace sycl;

// Matrix dimensions
constexpr size_t kNumRows = 4;
constexpr size_t kNumCols = 4;
constexpr size_t kNumElements = kNumRows * kNumCols;

// Forward declare the kernel name in the global scope.
// This FPGA best practice reduces name mangling in the optimization reports.
template <int N> class KernelCompute;

// The kernel implements a matrix multiplication.
// This is not meant to be a high performance implementation on FPGA!
// It's just a simple kernel with nested loops to illustrate loop coalescing.
template <int coalesce_factor>
void MatrixMultiply(const std::vector<float> &matrix_a,
                    const std::vector<float> &matrix_b,
                    std::vector<float> &res) {
#if FPGA_SIMULATOR
  auto selector = sycl::ext::intel::fpga_simulator_selector_v;
#elif FPGA_HARDWARE
  auto selector = sycl::ext::intel::fpga_selector_v;
#else  // #if FPGA_EMULATOR
  auto selector = sycl::ext::intel::fpga_emulator_selector_v;
#endif

  try {
    auto prop_list = property_list{property::queue::enable_profiling()};

    queue q(selector, fpga_tools::exception_handler, prop_list);

    auto device = q.get_device();

    std::cout << "Running on device: "
              << device.get_info<sycl::info::device::name>().c_str()
              << std::endl;

    buffer buffer_in_a(matrix_a);
    buffer buffer_in_b(matrix_b);
    buffer buffer_out(res);

    event e = q.submit([&](handler &h) {
      accessor accessor_matrix_a(buffer_in_a, h, read_only);
      accessor accessor_matrix_b(buffer_in_b, h, read_only);
      accessor accessor_res(buffer_out, h, write_only, no_init);

      // The kernel_args_restrict promises the compiler that this kernel's
      // accessor arguments won't alias (i.e. non-overlapping memory regions).
      h.single_task<class KernelCompute<coalesce_factor>>(
                                       [=]() [[intel::kernel_args_restrict]] {
        size_t idx = 0;
        float a[kNumRows][kNumCols];
        float b[kNumRows][kNumCols];
        float tmp[kNumRows][kNumCols];

        // The loop_coalesce instructs the compiler to attempt to "merge"
        // coalesce_factor loop levels of this nested loop together.
        // For example, a coalesce_factor of 2 turns this into a single loop.
        [[intel::loop_coalesce(coalesce_factor)]]
        for (size_t i = 0; i < kNumRows; ++i) {
          for (size_t j = 0; j < kNumCols; ++j) {
            a[i][j] = accessor_matrix_a[idx];
            b[i][j] = accessor_matrix_b[idx];
            tmp[i][j] = 0.0;
            idx++;
          }
        }

        // Applying loop_coalesce to the outermost loop of a deeply nested
        // loop results coalescing from the outside in.
        // For example, a coalesce_factor of 2 coalesces the "i" and "j" loops,
        // making a doubly nested loop.
        [[intel::loop_coalesce(coalesce_factor)]]
        for (size_t i = 0; i < kNumRows; ++i) {
          for (size_t j = 0; j < kNumCols; ++j) {
            float sum = 0.0f;
            for (size_t k = 0; k < kNumCols; ++k) {
              sum += a[i][k] * b[k][j];
            }
            tmp[i][j] = sum;
          }
        }

        idx = 0;
        [[intel::loop_coalesce(coalesce_factor)]]
        for (size_t i = 0; i < kNumRows; ++i) {
          for (size_t j = 0; j < kNumCols; ++j) {
            accessor_res[idx] = tmp[i][j];
            idx++;
          }
        }

      });
    });

  } catch (exception const &exc) {
    std::cerr << "Caught synchronous SYCL exception:\n" << exc.what() << '\n';
    if (exc.code().value() == CL_DEVICE_NOT_FOUND) {
      std::cerr << "If you are targeting an FPGA, please ensure that your "
                   "system has a correctly configured FPGA board.\n";
      std::cerr << "Run sys_check in the oneAPI root directory to verify.\n";
      std::cerr << "If you are targeting the FPGA emulator, compile with "
                   "-DFPGA_EMULATOR.\n";
    }
    std::terminate();
  }
}

int main() {
  std::vector<float> matrix_a(kNumElements);
  std::vector<float> matrix_b(kNumElements);
  std::vector<float> matrix_output_no_col(kNumElements);
  std::vector<float> matrix_output(kNumElements);

  // Specify the matrices to be multiplied
  for (size_t i = 0; i < kNumRows; i++) {
    size_t pos = i * kNumCols;
    // Initialize A as identity matrix
    matrix_a[i + pos] = 1.0;
    for (size_t j = 0; j < kNumCols; j++) {
      matrix_b[pos + j] = i * j + 1;
    }
  }

  // Two versions of the simple matrix multiply kernel will be enqueued:
  //  - with coalesce_factor=1 (i.e. no loop coalescing)
  //  - with coalesce_factor=2 (coalesce two nested levels)
  MatrixMultiply<1>(matrix_a, matrix_b, matrix_output_no_col);
  MatrixMultiply<2>(matrix_a, matrix_b, matrix_output);

  // Correctness check
  bool passed = true;
  for (size_t i = 0; i < kNumRows; i++) {
    size_t pos = i * kNumCols;
    for (size_t j = 0; j < kNumCols; j++) {
      float val_noCol = matrix_output_no_col[pos + j];
      float val = matrix_output[pos + j];
      if (val_noCol != i * j + 1 || val != i * j + 1) {
        std::cout << "FAILED: The results are incorrect\n";
        passed = false;
      }
    }
  }

  if (passed) {
    std::cout << "PASSED: The results are correct\n";
    return 0;
  } else {
    std::cout << "FAILED\n";
    return -1;
  }
}
