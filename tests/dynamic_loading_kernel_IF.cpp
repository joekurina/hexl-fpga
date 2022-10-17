// Copyright (C) 2020-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "dynamic_loading_kernel_IF.h"

DynamicIF::DynamicIF(const std::string& libName) 
    : m_lib_name_(libName) {
    std::cout << "Using FPGA shared library: " << m_lib_name_ << std::endl;
    m_lib_handle_ = dlopen(m_lib_name_.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!m_lib_handle_) {
        std::cout << "Failed to open dynamic library: " << m_lib_name_
                  << std::endl;
        std::cout << "dlopen error: " << dlerror() << std::endl;
        exit(-1);
    } else {
        std::cout << "Successfull Opened : " << m_lib_name_ << std::endl;
    }
}

DynamicIF::~DynamicIF() {
    if (m_lib_handle_) dlclose(m_lib_handle_);
}

void* DynamicIF::loadKernel(const char* kernelName) const {
    void* temp = dlsym(m_lib_handle_, kernelName);
    if (!temp)
        std::cerr << "Cannot load symbol function " << kernelName << std::endl;
#if defined(FPGA_EMULATOR)
    std::cout << "load function " << kernelName << " successfully.\n";
#endif

    return temp;
}

std::string DynamicIF::getLibName() const { return m_lib_name_; }

MultLowLvlDynaimcIF::MultLowLvlDynaimcIF(const std::string& lib) 
    : DynamicIF(lib),
      launch_intt1(nullptr),
      launch_intt2(nullptr),
      launch_ntt1(nullptr),
      launch_ntt2(nullptr),
      BringToSetLoad(nullptr),
      BringToSetLoad2(nullptr),
      TensorProductStore0(nullptr),
      TensorProductStore12(nullptr),
      BringToSet(nullptr),
      BringToSet2(nullptr),
      TensorProduct(nullptr) {
    
    launch_intt1 = (void (*)(std::vector<uint64_t> &primes))loadKernel("launch_intt1_IF");

    launch_intt2 = (void (*)(std::vector<uint64_t> &primes))loadKernel("launch_intt2_IF");

    launch_ntt1 = (void (*)(std::vector<uint64_t> &primes))loadKernel("launch_ntt1_IF");

    launch_ntt2 = (void (*)(std::vector<uint64_t> &primes))loadKernel("launch_ntt2_IF");

    BringToSetLoad = (sycl::event (*)(sycl::queue&, sycl::event&, 
                                 sycl::buffer<uint64_t>&,
                                 sycl::buffer<uint8_t>&))loadKernel("BringToSetLoad_IF");
    
    BringToSetLoad2 = (sycl::event (*)(sycl::queue&, sycl::event&, 
                                 sycl::buffer<uint64_t>&,
                                 sycl::buffer<uint8_t>&))loadKernel("BringToSetLoad2_IF");

    TensorProductStore0 = (sycl::event (*)(sycl::queue&,
                                       sycl::buffer<ulong>&))loadKernel("TensorProductStore0_IF");
    
    TensorProductStore12 = (sycl::event (*)(sycl::queue&,
                                        sycl::buffer<ulong>&,
                                        sycl::buffer<ulong>&))loadKernel("TensorProductStore12_IF");
    
    BringToSet = (sycl::event (*)(sycl::queue&, uint32_t,
                             sycl::buffer<ulong2>&, uint32_t,
                             uint32_t, uint, uint64_t))loadKernel("BringToSet_IF");
    
    BringToSet2 = (sycl::event (*)(sycl::queue&, uint32_t,
                               sycl::buffer<ulong2>&, uint32_t,
                               uint32_t, uint, uint64_t))loadKernel("BringToSet2_IF");
    
    TensorProduct = (sycl::event (*)(sycl::queue&, sycl::buffer<ulong4>&))loadKernel("TensorProduct_IF");

}