// Copyright (C) 2020-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include <dlfcn.h>
#include <string.h>

#include <cmath>
#include <iomanip>
#include <iostream>
#include <unordered_map>
#include "fpga.h"
#include "fpga_assert.h"
#include "number_theory_util.h"
#include <NTL/ZZ.h>
#include <hexl/hexl.hpp>
#include <hexl/ntt/ntt.hpp>
#include "../../device/multlowlvl/include/L2/utils.h"

using namespace L2::helib::bgv;

namespace intel {
namespace hexl {
namespace fpga {

// helper function to explicitly copy host data to device.
static sycl::event copy_buffer_to_device(sycl::queue& q,
                                         sycl::buffer<uint64_t>& buf) {
    sycl::host_accessor host_acc(buf);
    uint64_t* host_ptr = host_acc.get_pointer();
    sycl::event e = q.submit([&](sycl::handler& h) {
        auto acc_dev = buf.get_access<sycl::access::mode::discard_write>(h);
        h.copy(host_ptr, acc_dev);
    });
    return e;
}

// utility function for copying input data batch for KeySwitch

const char* keyswitch_kernel_name[] = {"load", "store"};
unsigned int Object::g_wid_ = 0;
Object::Object(kernel_t type, bool fence)
    : ready_(false), type_(type), fence_(fence) {
    id_ = Object::g_wid_++;
}
Object_DyadicMultiply::Object_DyadicMultiply(uint64_t* results,
                                             const uint64_t* operand1,
                                             const uint64_t* operand2,
                                             uint64_t n, const uint64_t* moduli,
                                             uint64_t n_moduli, bool fence)
    : Object(kernel_t::DYADIC_MULTIPLY, fence),
      results_(results),
      operand1_(operand1),
      operand2_(operand2),
      n_(n),
      moduli_(moduli),
      n_moduli_(n_moduli) {}
Object_NTT::Object_NTT(uint64_t* coeff_poly,
                       const uint64_t* root_of_unity_powers,
                       const uint64_t* precon_root_of_unity_powers,
                       uint64_t coeff_modulus, uint64_t n, bool fence)
    : Object(kernel_t::NTT, fence),
      coeff_poly_(coeff_poly),
      root_of_unity_powers_(root_of_unity_powers),
      precon_root_of_unity_powers_(precon_root_of_unity_powers),
      coeff_modulus_(coeff_modulus),
      n_(n) {}
Object_INTT::Object_INTT(uint64_t* coeff_poly,
                         const uint64_t* inv_root_of_unity_powers,
                         const uint64_t* precon_inv_root_of_unity_powers,
                         uint64_t coeff_modulus, uint64_t inv_n,
                         uint64_t inv_n_w, uint64_t n, bool fence)
    : Object(kernel_t::INTT, fence),
      coeff_poly_(coeff_poly),
      inv_root_of_unity_powers_(inv_root_of_unity_powers),
      precon_inv_root_of_unity_powers_(precon_inv_root_of_unity_powers),
      coeff_modulus_(coeff_modulus),
      inv_n_(inv_n),
      inv_n_w_(inv_n_w),
      n_(n) {}
Object_KeySwitch::Object_KeySwitch(
    uint64_t* result, const uint64_t* t_target_iter_ptr, uint64_t n,
    uint64_t decomp_modulus_size, uint64_t key_modulus_size,
    uint64_t rns_modulus_size, uint64_t key_component_count,
    const uint64_t* moduli, const uint64_t** k_switch_keys,
    const uint64_t* modswitch_factors, const uint64_t* twiddle_factors,
    bool fence)
    : Object(kernel_t::KEYSWITCH, fence),
      result_(result),
      t_target_iter_ptr_(t_target_iter_ptr),
      n_(n),
      decomp_modulus_size_(decomp_modulus_size),
      key_modulus_size_(key_modulus_size),
      rns_modulus_size_(rns_modulus_size),
      key_component_count_(key_component_count),
      moduli_(moduli),
      k_switch_keys_(k_switch_keys),
      modswitch_factors_(modswitch_factors),
      twiddle_factors_(twiddle_factors) {}

Object_MultLowLvl::Object_MultLowLvl(
    uint64_t* a0, uint64_t* a1, uint64_t a_primes_len, uint8_t* a_primes_index, 
    uint64_t* b0, uint64_t* b1, uint64_t b_primes_len, uint8_t* b_primes_index,
    uint64_t plainText, uint64_t coeff_count, 
    uint64_t* c0, uint64_t* c1, uint64_t* c2, 
    uint64_t c_primes_len, uint8_t* output_primes_index, 
    uint64_t all_primes_len, uint64_t* all_primes, bool fence)
    : Object(kernel_t::MULTLOWLVL, fence),
    a0_(a0),
    a1_(a1),
    a_primes_len_(a_primes_len),
    a_primes_index_(a_primes_index),
    b0_(b0),
    b1_(b1),
    b_primes_len_(b_primes_len),
    b_primes_index_(b_primes_index),
    plainText_(plainText),
    coeff_count_(coeff_count),
    c0_(c0),
    c1_(c1),
    c2_(c2),
    c_primes_len_(c_primes_len),
    output_primes_index_(output_primes_index),
    all_primes_len_(all_primes_len),
    all_primes_(all_primes) {}


Object_ReLinearize::Object_ReLinearize(uint64_t* all_primes, size_t all_primes_len, 
                uint64_t* keys1, uint64_t* keys2,
                uint64_t* keys3, uint64_t* keys4, size_t keys_len,
                uint64_t* c2, size_t c2_len,
                uint64_t* pi, size_t pi_len,
                unsigned* num_designed_digits_primes, size_t digits_primes_len,
                size_t num_special_primes, uint8_t* primes_index, size_t primes_index_len,
                uint64_t* output, size_t output_len, bool fence = false)
                : Object(kernel_t::RELINEARIZE, fence),
                all_primes_(all_primes),
                all_primes_len_(all_primes_len),
                keys1_(keys1),
                keys2_(keys2),
                keys3_(keys3),
                keys4_(keys4),
                keys_len_(keys_len),
                c2_(c2),
                c2_len_(c2_len),
                pi_(pi),
                pi_len_(pi_len),
                num_designed_digits_primes_(num_designed_digits_primes),
                digits_primes_len_(digits_primes_len),
                num_special_primes_(num_special_primes),
                primes_index_(primes_index),
                primes_index_len_(primes_index_len),
                output_(output),
                output_len_(output_len) {}


Object* Buffer::front() const {
    Object* obj = buffer_.front();
    return obj;
}
Object* Buffer::back() const {
    Object* obj = buffer_.back();
    return obj;
}
void Buffer::push(Object* obj) {
    std::unique_lock<std::mutex> locker(mu_);
    cond_.wait(locker, [this]() { return buffer_.size() < capacity_; });
    buffer_.push_back(obj);
    locker.unlock();
    cond_.notify_all();
}
std::vector<Object*> Buffer::pop() {
    std::unique_lock<std::mutex> locker(mu_);

    uint64_t work_size = 1;

    Object_DyadicMultiply* obj_dyadic_multiply = nullptr;
    Object_NTT* obj_ntt = nullptr;
    Object_INTT* obj_intt = nullptr;
    Object_KeySwitch* obj_KeySwitch = nullptr;
    Object_MultLowLvl* object_MultLowLvl = nullptr;

    if (buffer_.size() > 0) {
        Object* object = buffer_.front();
        if (object) {
            switch (object->type_) {
            case kernel_t::DYADIC_MULTIPLY:
                obj_dyadic_multiply =
                    dynamic_cast<Object_DyadicMultiply*>(object);
                if (obj_dyadic_multiply) {
                    work_size = get_worksize_int_DyadicMultiply();
                }
                break;
            case kernel_t::INTT:
                obj_intt = dynamic_cast<Object_INTT*>(object);
                if (obj_intt) {
                    work_size = get_worksize_int_INTT();
                }
                break;
            case kernel_t::NTT:
                obj_ntt = dynamic_cast<Object_NTT*>(object);
                if (obj_ntt) {
                    work_size = get_worksize_int_NTT();
                }
                break;
            case kernel_t::KEYSWITCH:
                obj_KeySwitch = dynamic_cast<Object_KeySwitch*>(object);
                if (obj_KeySwitch) {
                    work_size = get_worksize_int_KeySwitch();
                }
                break;
            case kernel_t::MULTLOWLVL:
                object_MultLowLvl = dynamic_cast<Object_MultLowLvl*>(object);
                if (object_MultLowLvl) {
                    work_size = get_worksize_int_MultLowLvl();
                }
                break;
            default:
                FPGA_ASSERT(0, "Invalid kernel!")
                break;
            }
        }
    }
    cond_.wait(locker,
               [this, &work_size]() { return buffer_.size() >= work_size; });
    FPGA_ASSERT(work_size > 0);
    std::vector<Object*> objs;
    uint64_t batch = 0;
    while (batch < work_size) {
        Object* obj = buffer_.front();
        if (obj->fence_ && (batch > 0)) {
            break;
        }
        objs.emplace_back(obj);
        buffer_.pop_front();
        batch++;
    }
    if (obj_dyadic_multiply) {
        update_DyadicMultiply_work_size(batch);
    } else if (obj_ntt) {
        update_NTT_work_size(batch);
    } else if (obj_intt) {
        update_INTT_work_size(batch);
    } else if (obj_KeySwitch) {
        update_KeySwitch_work_size(batch);
    } else if (object_MultLowLvl) {
        update_MultLowLvl_work_size(batch);
    }

    locker.unlock();
    cond_.notify_all();

    return objs;
}

uint64_t Buffer::size() {
    std::unique_lock<std::mutex> locker(mu_size_);

    uint64_t buf_size = buffer_.size();

    locker.unlock();

    return buf_size;
}

std::atomic<int> FPGAObject::g_tag_(0);

FPGAObject::FPGAObject(sycl::queue& p_q, uint64_t n_batch, kernel_t type,
                       bool fence)
    : m_q(p_q),
      tag_(-1),
      n_batch_(n_batch),
      batch_size_(n_batch),
      type_(type),
      fence_(fence) {}

void FPGAObject::recycle() {
    tag_ = -1;
    fence_ = false;
    in_objs_.resize(0);
}

FPGAObject_NTT::FPGAObject_NTT(sycl::queue& p_q, uint64_t coeff_count,
                               uint64_t batch_size)
    : FPGAObject(p_q, batch_size, kernel_t::NTT),

      n_(coeff_count) {
    uint64_t data_size = batch_size * coeff_count;
    coeff_poly_in_svm_ = sycl::malloc_shared<uint64_t>(data_size, m_q);
    root_of_unity_powers_in_svm_ =
        sycl::malloc_shared<uint64_t>(coeff_count, m_q);
    precon_root_of_unity_powers_in_svm_ =
        sycl::malloc_shared<uint64_t>(coeff_count, m_q);
    coeff_modulus_in_svm_ = sycl::malloc_shared<uint64_t>(1, m_q);
}

FPGAObject_INTT::FPGAObject_INTT(sycl::queue& p_q, uint64_t coeff_count,
                                 uint64_t batch_size)
    : FPGAObject(p_q, batch_size, kernel_t::INTT),

      n_(coeff_count) {
    uint64_t data_size = batch_size * coeff_count;
    coeff_poly_in_svm_ = sycl::malloc_shared<uint64_t>(data_size, m_q);
    inv_n_in_svm_ = sycl::malloc_shared<uint64_t>(1, m_q);
    inv_n_w_in_svm_ = sycl::malloc_shared<uint64_t>(1, m_q);
    inv_root_of_unity_powers_in_svm_ =
        sycl::malloc_shared<uint64_t>(coeff_count, m_q);
    precon_inv_root_of_unity_powers_in_svm_ =
        sycl::malloc_shared<uint64_t>(coeff_count, m_q);
    coeff_modulus_in_svm_ = sycl::malloc_shared<uint64_t>(1, m_q);
}

FPGAObject_DyadicMultiply::FPGAObject_DyadicMultiply(sycl::queue& p_q,
                                                     uint64_t coeff_size,
                                                     uint32_t modulus_size,
                                                     uint64_t batch_size)
    : FPGAObject(p_q, batch_size, kernel_t::DYADIC_MULTIPLY),
      n_(coeff_size),
      n_moduli_(0) {
    uint64_t n = batch_size * modulus_size * coeff_size;
    operand1_in_svm_ = sycl::malloc_shared<uint64_t>(n * 2, m_q);
    operand2_in_svm_ = sycl::malloc_shared<uint64_t>(n * 2, m_q);
    moduli_info_ =
        sycl::malloc_shared<moduli_info_t>(batch_size * modulus_size, m_q);
    operands_in_ddr_ = sycl::malloc_device<uint64_t>(n * 4, m_q);
    results_out_ddr_ = sycl::malloc_device<uint64_t>(n * 3, m_q);
}

FPGAObject_KeySwitch::FPGAObject_KeySwitch(sycl::queue& p_q,
                                           uint64_t batch_size)
    : FPGAObject(p_q, batch_size, kernel_t::KEYSWITCH),
      n_(0),
      decomp_modulus_size_(0),
      key_modulus_size_(0),
      rns_modulus_size_(0),
      key_component_count_(0),
      moduli_(nullptr),
      k_switch_keys_(nullptr),
      modswitch_factors_(nullptr),
      twiddle_factors_(nullptr) {
    size_t size_in = batch_size * H_MAX_COEFF_COUNT * H_MAX_KEY_MODULUS_SIZE;
    size_t size_out = size_in * H_MAX_KEY_COMPONENT_SIZE;
    ms_output_ = static_cast<uint64_t*>(
        aligned_alloc(HOST_MEM_ALIGNMENT, size_out * sizeof(uint64_t)));
    mem_t_target_iter_ptr_ = new sycl::buffer<uint64_t>(
        sycl::range(size_in),
        {sycl::property::buffer::mem_channel{MEM_CHANNEL_K1}});
    mem_KeySwitch_results_ = new sycl::buffer<sycl::ulong2>(
        sycl::range(size_out / 2),
        {sycl::property::buffer::mem_channel{MEM_CHANNEL_K1}});
    mem_t_target_iter_ptr_->set_write_back(false);
    mem_KeySwitch_results_->set_write_back(false);
}

FPGAObject_MultLowLvl::FPGAObject_MultLowLvl(sycl::queue& p_q, 
                                             uint64_t batch_size,
                                             uint64_t coeff_count,
                                             uint64_t plainText,
                                             uint64_t a_primes_len,
                                             uint64_t b_primes_len,
                                             uint64_t c_primes_len,
                                             uint64_t all_primes_len)
    : FPGAObject(p_q, batch_size, kernel_t::MULTLOWLVL),
    a_primes_len_(a_primes_len),
    b_primes_len_(b_primes_len),
    plainText_(plainText),
    coeff_count_(coeff_count),
    c_primes_len_(c_primes_len),
    all_primes_len_(all_primes_len) {
    
    a0_buf_ = new sycl::buffer<uint64_t>(batch_size * coeff_count_ * a_primes_len_);
    a1_buf_ = new sycl::buffer<uint64_t>(batch_size * coeff_count_ * a_primes_len_);
    b0_buf_ = new sycl::buffer<uint64_t>(batch_size * coeff_count_ * b_primes_len_);
    b1_buf_ = new sycl::buffer<uint64_t>(batch_size * coeff_count_ * b_primes_len_);

    a_primes_index_ = static_cast<uint8_t*>(std::malloc(batch_size * a_primes_len_ * sizeof(uint8_t)));
    b_primes_index_ = static_cast<uint8_t*>(std::malloc(batch_size * b_primes_len_ * sizeof(uint8_t)));
    output_primes_index_ = static_cast<uint8_t*>(std::malloc(batch_size * c_primes_len_ * sizeof(uint8_t)));
    all_primes_ = static_cast<uint64_t*>(std::malloc(batch_size * all_primes_len_ * sizeof(uint64_t)));

    mem_output1_ = static_cast<uint64_t*>(std::malloc(batch_size * coeff_count_ * c_primes_len_ * sizeof(uint64_t)));
    mem_output2_ = static_cast<uint64_t*>(std::malloc(batch_size * coeff_count_ * c_primes_len_ * sizeof(uint64_t)));
    mem_output3_ = static_cast<uint64_t*>(std::malloc(batch_size * coeff_count_ * c_primes_len_ * sizeof(uint64_t)));
}

FPGAObject_ReLinearize::FPGAObject_ReLinearize(sycl::queue& p_q,
                                               uint64_t batch_size,
                                               uint64_t all_primes_len,
                                               uint64_t keys_len,
                                               uint64_t c2_len,
                                               uint64_t pi_len,
                                               uint64_t digits_primes_len,
                                               uint64_t primes_index_len,
                                               uint64_t output_len)
    : FPGAObject(p_q, batch_size, kernel_t::RELINEARIZE),
    all_primes_len_(all_primes_len),
    keys_len_(keys_len),
    c2_len_(c2_len),
    pi_len_(pi_len),
    digits_primes_len_(digits_primes_len),
    primes_index_len_(primes_index_len),
    output_len_(output_len) {
    
    c2_buf_ = new sycl::buffer<uint64_t>(batch_size * c2_len_);
    pi_ = static_cast<uint64_t*>(std::malloc(batch_size * pi_len_ * sizeof(uint64_t)));
    num_designed_digits_primes_ = static_cast<unsigned*>(std::malloc(batch_size * num_special_primes_ * sizeof(unsigned)));
    primes_index_ = static_cast<uint8_t*>(std::malloc(batch_size * primes_index_len_ * sizeof(uint8_t)));
    output_ = static_cast<uint64_t*>(std::malloc(batch_size * output_len_ * sizeof(uint64_t)));

    all_primes_ = static_cast<uint64_t*>(std::malloc(batch_size * all_primes_len_ * sizeof(uint64_t)));
    keys1_ = static_cast<uint64_t*>(std::malloc(batch_size * keys_len_ * sizeof(uint64_t)));
    keys2_ = static_cast<uint64_t*>(std::malloc(batch_size * keys_len_ * sizeof(uint64_t)));
    keys3_ = static_cast<uint64_t*>(std::malloc(batch_size * keys_len_ * sizeof(uint64_t)));
    keys4_ = static_cast<uint64_t*>(std::malloc(batch_size * keys_len_ * sizeof(uint64_t)));
}


FPGAObject_ReLinearize::~FPGAObject_ReLinearize() {
    if (keys4_) {
        std::free(keys4_);
    }
    if (keys3_) {
        std::free(keys4_);
    }
    if (keys2_) {
        std::free(keys4_);
    }
    if (keys1_) {
        std::free(keys4_);
    }
    if (all_primes_) {
        std::free(all_primes_);
    }
    if (output_) {
        std::free(output_);
    }
    if (primes_index_) {
        std::frer(primes_index_);
    }
    if (num_designed_digits_primes_) {
        std::free(num_designed_digits_primes_);
    }
    if (pi_) {
        std::free(pi_);
    }
    if (c2_buf_) {
        delete c2_buff_;
    }
}


FPGAObject_MultLowLvl::~FPGAObject_MultLowLvl() {
    if (a_primes_index_) {
        std::free(a_primes_index_);
    }
    if (b_primes_index_) {
        std::free(b_primes_index_);
    }
    if (output_primes_index_) {
        std::free(output_primes_index_);
    }
    if (all_primes_) {
        std::free(all_primes_);
    }
    if (mem_output1_) {
        std::free(mem_output1_);
    }
    if (mem_output2_) {
        std::free(mem_output2_);
    }
    if (mem_output3_) {
        std::free(mem_output3_);
    }
    if (a0_buf_) {
        delete a0_buf_;
    }
    if (a1_buf_) {
        delete a1_buf_;
    }
    if (b0_buf_) {
        delete b0_buf_;
    }
    if (b1_buf_) {
        delete b1_buf_;
    }
}

FPGAObject_KeySwitch::~FPGAObject_KeySwitch() {
    if (ms_output_) {
        free(ms_output_);
    }
    if (mem_KeySwitch_results_) {
        delete mem_KeySwitch_results_;
    }
}
FPGAObject_DyadicMultiply::~FPGAObject_DyadicMultiply() {
    free(operand1_in_svm_, m_q);
    operand1_in_svm_ = nullptr;
    free(operand2_in_svm_, m_q);
    operand2_in_svm_ = nullptr;
    free(moduli_info_, m_q);
    moduli_info_ = nullptr;
    if (operands_in_ddr_) {
        free(operands_in_ddr_, m_q);
    }
    if (results_out_ddr_) {
        free(results_out_ddr_, m_q);
    }
}
FPGAObject_NTT::~FPGAObject_NTT() {
    free(coeff_poly_in_svm_, m_q);
    coeff_poly_in_svm_ = nullptr;
    free(root_of_unity_powers_in_svm_, m_q);
    root_of_unity_powers_in_svm_ = nullptr;
    free(precon_root_of_unity_powers_in_svm_, m_q);
    precon_root_of_unity_powers_in_svm_ = nullptr;
    free(coeff_modulus_in_svm_, m_q);
    coeff_modulus_in_svm_ = nullptr;
}

FPGAObject_INTT::~FPGAObject_INTT() {
    free(coeff_poly_in_svm_, m_q);
    coeff_poly_in_svm_ = nullptr;
    free(inv_root_of_unity_powers_in_svm_, m_q);
    inv_root_of_unity_powers_in_svm_ = nullptr;
    free(precon_inv_root_of_unity_powers_in_svm_, m_q);
    precon_inv_root_of_unity_powers_in_svm_ = nullptr;
    free(coeff_modulus_in_svm_, m_q);
    coeff_modulus_in_svm_ = nullptr;
    free(inv_n_in_svm_, m_q);
    inv_n_in_svm_ = nullptr;
    free(inv_n_w_in_svm_, m_q);
    inv_n_w_in_svm_ = nullptr;
}


void FPGAObject_Relinearize::fill_in_data(const std::vector<Object*> &objs) {
    std::cout << __func__ << " objs.size(): " << objs.size() << std::endl;

    sycl::host_accessor c2_acc(*a2_buf_);
    uint64_t batch = 0;
    for (const auto &obj_in : objs) {
        Object_ReLinearize* obj = dynamic_cast<Object_ReLinearize*>(obj_in);
        FPGA_ASSERT(obj);
        in_objs_.emplace_back(obj);

        fence_ |= obj->fence_;
        all_primes_len_ = obj->all_primes_len_;
        keys_len_ = obj->keys_len_;
        c2_len_ = obj->c2_len_;
        pi_len_ = obj->pi_len_;
        digits_primes_len_ = obj->digits_primes_len_;
        num_special_primes_ = obj->num_special_primes_;
        output_len_ = obj->output_len_;
        primes_index_len_ = obj->primes_index_len_;


        std::memcpy(c2_acc.get_pointer() + batch * c2_len_ * sizeof(uint64_t), obj->c2_, c2_len_ * sizeof(uint64_t));
        std::memcpy(pi_ + batch * pi_len_ * sizeof(uint64_t), obj->pi_, pi_len_ * sizeof(uint64_t));
        std::memcpy(num_designed_digits_primes_ + batch * digits_primes_len_ * sizeof(unsigned), obj->num_designed_digits_primes_,
                    digits_primes_len_ * sizeof(unsigned));
        std::memcpy(primes_index_ + batch * primes_index_len_ * sizeof(uint8_t), obj->primes_index_,
                    primes_index_len_ * sizeof(uint8_t));
        
        std::memcpy(keys1_ + batch * keys_len_ * sizeof(uint64_t), obj->keys1_, 
                    keys_len_ * sizeof(uint64_t));
        std::memcpy(keys2_ + batch * keys_len_ * sizeof(uint64_t), obj->keys2_, 
                    keys_len_ * sizeof(uint64_t));
        std::memcpy(keys3_ + batch * keys_len_ * sizeof(uint64_t), obj->keys3_, 
                    keys_len_ * sizeof(uint64_t));
        std::memcpy(keys4_ + batch * keys_len_ * sizeof(uint64_t), obj->keys4_, 
                    keys_len_ * sizeof(uint64_t));

        batch++;
    }

    n_batch_ = batch;
    tag_ = g_tag_++;
}


void FPGAObject_MultLowLvl::fill_in_data(const std::vector<Object*>& objs) {

    std::cout << __func__ << " objs.size(): " << objs.size() << std::endl;

    uint64_t batch = 0;
    fence_ = false;
    for (const auto& obj_in : objs) {
        Object_MultLowLvl* obj = dynamic_cast<Object_MultLowLvl*>(obj_in);
        FPGA_ASSERT(obj);
        in_objs_.emplace_back(obj);
        
        fence_ |= obj->fence_;
        coeff_count_ = obj->coeff_count_;
        a_primes_len_ = obj->a_primes_len_;
        b_primes_len_ = obj->b_primes_len_;
        c_primes_len_ = obj->c_primes_len_;
        all_primes_len_ = obj->all_primes_len_;

        batch++;
    }

    std::cout << __func__ << " a_primes_len: " << a_primes_len_ << std::endl;
    std::cout << __func__ << " b_primes_len: " << b_primes_len_ << std::endl;
    std::cout << __func__ << " c_primes_len: " << c_primes_len_ << std::endl;

    int frame = 0;
    sycl::host_accessor a0_acc(*a0_buf_);
    sycl::host_accessor b0_acc(*b0_buf_);
    sycl::host_accessor a1_acc(*a1_buf_);
    sycl::host_accessor b1_acc(*b1_buf_);
    for (const auto& obj_in : objs) {
        Object_MultLowLvl* obj = dynamic_cast<Object_MultLowLvl*>(obj_in);
        memcpy(a0_acc.get_pointer() + frame * coeff_count_ * a_primes_len_, obj->a0_,
                coeff_count_ * a_primes_len_ * sizeof(uint64_t));
        memcpy(a1_acc.get_pointer() + frame * coeff_count_ * a_primes_len_, obj->a1_,
                coeff_count_ * a_primes_len_ * sizeof(uint64_t));
        memcpy(b0_acc.get_pointer() + frame * coeff_count_ * b_primes_len_, obj->b0_,
                coeff_count_ * b_primes_len_ * sizeof(uint64_t));
        memcpy(b1_acc.get_pointer() + frame * coeff_count_ * b_primes_len_, obj->b1_,
                coeff_count_ * b_primes_len_ * sizeof(uint64_t));
        
        memcpy(a_primes_index_ + frame * a_primes_len_, obj->a_primes_index_, 
                a_primes_len_ * sizeof(uint8_t));
        
        memcpy(b_primes_index_ + frame * b_primes_len_, obj->b_primes_index_, 
                b_primes_len_ * sizeof(uint8_t));
        
        memcpy(output_primes_index_ + frame * c_primes_len_, obj->output_primes_index_,
                c_primes_len_ * sizeof(uint8_t));

        memcpy(all_primes_ + frame * all_primes_len_, obj->all_primes_, 
                all_primes_len_ * sizeof(uint64_t));
        
        frame++;
    }

    n_batch_ = batch;
    tag_ = g_tag_++;
}


void FPGAObject_KeySwitch::fill_in_data(const std::vector<Object*>& objs) {
    uint64_t batch = 0;
    fence_ = false;
    for (const auto& obj_in : objs) {
        Object_KeySwitch* obj = dynamic_cast<Object_KeySwitch*>(obj_in);
        FPGA_ASSERT(obj);
        in_objs_.emplace_back(obj);

        fence_ |= obj->fence_;
        n_ = obj->n_;
        decomp_modulus_size_ = obj->decomp_modulus_size_;
        key_modulus_size_ = obj->key_modulus_size_;
        rns_modulus_size_ = obj->rns_modulus_size_;
        key_component_count_ = obj->key_component_count_;
        moduli_ = const_cast<uint64_t*>(obj->moduli_);
        k_switch_keys_ = const_cast<uint64_t**>(obj->k_switch_keys_);
        modswitch_factors_ = const_cast<uint64_t*>(obj->modswitch_factors_);
        twiddle_factors_ = const_cast<uint64_t*>(obj->twiddle_factors_);

        batch++;
    }
    n_batch_ = batch;

    tag_ = g_tag_++;
}

void FPGAObject_DyadicMultiply::fill_in_data(const std::vector<Object*>& objs) {
    uint64_t batch = 0;
    for (const auto& obj_in : objs) {
        Object_DyadicMultiply* obj =
            dynamic_cast<Object_DyadicMultiply*>(obj_in);
        FPGA_ASSERT(obj);
        in_objs_.emplace_back(obj);

        n_moduli_ = obj->n_moduli_;
        n_ = obj->n_;

        for (uint64_t i = 0; i < n_moduli_; i++) {
            uint64_t modulus = obj->moduli_[i];
            uint64_t len = uint64_t(floorl(std::log2l(modulus)) - 1);
            fpga_uint128_t n = fpga_uint128_t(1) << (len + 64);
            uint64_t barr_lo = uint64_t(n / modulus);
            moduli_info_[batch * n_moduli_ + i] =
                (moduli_info_t){modulus, len, barr_lo};
        }
        batch++;
    }

    n_batch_ = batch;

    // assuming the batch of operand1s and operand2s are in contiguous space
    // respectively
    uint64_t n_data = n_moduli_ * n_ * 2;
    Object_DyadicMultiply* obj =
        dynamic_cast<Object_DyadicMultiply*>(in_objs_.front());
    FPGA_ASSERT(obj);
    memcpy(operand1_in_svm_, obj->operand1_,
           n_batch_ * n_data * sizeof(uint64_t));
    memcpy(operand2_in_svm_, obj->operand2_,
           n_batch_ * n_data * sizeof(uint64_t));

    tag_ = g_tag_++;
}
void FPGAObject_NTT::fill_in_data(const std::vector<Object*>& objs) {
    uint64_t batch = 0;
    for (const auto& obj_in : objs) {
        Object_NTT* obj = dynamic_cast<Object_NTT*>(obj_in);
        FPGA_ASSERT(obj);
        in_objs_.emplace_back(obj);
        n_ = obj->n_;
        batch++;
    }
    n_batch_ = batch;
    uint64_t coeff_count = n_;
    Object_NTT* obj = dynamic_cast<Object_NTT*>(in_objs_.front());
    FPGA_ASSERT(obj);
    memcpy(coeff_poly_in_svm_, obj->coeff_poly_,
           n_batch_ * coeff_count * sizeof(uint64_t));
    coeff_modulus_in_svm_[0] = obj->coeff_modulus_;
    memcpy(root_of_unity_powers_in_svm_, obj->root_of_unity_powers_,
           coeff_count * sizeof(uint64_t));
    memcpy(precon_root_of_unity_powers_in_svm_,
           obj->precon_root_of_unity_powers_, coeff_count * sizeof(uint64_t));
    tag_ = g_tag_++;
}

void FPGAObject_INTT::fill_in_data(const std::vector<Object*>& objs) {
    uint64_t batch = 0;
    for (const auto& obj_in : objs) {
        Object_INTT* obj = dynamic_cast<Object_INTT*>(obj_in);
        FPGA_ASSERT(obj);
        in_objs_.emplace_back(obj);
        n_ = obj->n_;
        batch++;
    }
    n_batch_ = batch;
    uint64_t coeff_count = n_;
    Object_INTT* obj = dynamic_cast<Object_INTT*>(in_objs_.front());
    FPGA_ASSERT(obj);
    memcpy(coeff_poly_in_svm_, obj->coeff_poly_,
           n_batch_ * coeff_count * sizeof(uint64_t));
    coeff_modulus_in_svm_[0] = obj->coeff_modulus_;
    memcpy(inv_root_of_unity_powers_in_svm_, obj->inv_root_of_unity_powers_,
           coeff_count * sizeof(uint64_t));
    memcpy(precon_inv_root_of_unity_powers_in_svm_,
           obj->precon_inv_root_of_unity_powers_,
           coeff_count * sizeof(uint64_t));
    *inv_n_in_svm_ = obj->inv_n_;
    *inv_n_w_in_svm_ = obj->inv_n_w_;
    tag_ = g_tag_++;
}


void FPGAObject_ReLinearize::fill_out_data(uint64_t* output) {
    uint64_t batch = 0;
    for (auto &obj : in_objs_) {
        Object_ReLinearize* obj_ReLinearize = dynamic_cast<Object_ReLinearize*>(obj);
        FPGA_ASSERT(obj_ReLinearize);
        std::memcpy(obj_ReLinearize->output_, output + batch * output_len_ * sizeof(uint64_t), 
                    output_len_ * sizeof(uint64_t));
        batch++;
        obj->ready_ = true;
    }
    FPGA_ASSERT(batch == n_batch_);
}


void FPGAObject_MultLowLvl::fill_out_data(uint64_t* output) {
    uint64_t batch = 0;
    for (auto &obj : in_objs_) {
        Object_MultLowLvl* obj_MultLowLvl = dynamic_cast<Object_MultLowLvl*>(obj);
        FPGA_ASSERT(obj_MultLowLvl);
        memcpy(obj_MultLowLvl->c0_, 
                mem_output1_ + batch * coeff_count_ * c_primes_len_,
                coeff_count_ * c_primes_len_ * sizeof(uint64_t));
        memcpy(obj_MultLowLvl->c1_, 
                mem_output2_ + batch  * coeff_count_ * c_primes_len_,
                coeff_count_ * c_primes_len_ * sizeof(uint64_t));
        memcpy(obj_MultLowLvl->c2_, 
                mem_output3_ + batch * coeff_count_ * c_primes_len_,
                coeff_count_ * c_primes_len_ * sizeof(uint64_t));
        batch++;
        obj->ready_= true;
    }
    FPGA_ASSERT(batch == n_batch_);
}


void FPGAObject_KeySwitch::fill_out_data(uint64_t* output) {
    uint64_t batch = 0;
    for (auto& obj : in_objs_) {
        Object_KeySwitch* obj_KeySwitch = dynamic_cast<Object_KeySwitch*>(obj);
        FPGA_ASSERT(obj_KeySwitch);
        size_t size_out =
            batch * decomp_modulus_size_ * n_ * key_component_count_;
        FPGA_ASSERT(key_component_count_ == 2);
        for (size_t i = 0; i < decomp_modulus_size_; i++) {
            uint64_t modulus = moduli_[i];
            for (size_t j = 0; j < n_; j++) {
                size_t k = i * n_ + j;
                obj_KeySwitch->result_[k] += output[size_out + 2 * k];
                obj_KeySwitch->result_[k] =
                    (obj_KeySwitch->result_[k] >= modulus)
                        ? (obj_KeySwitch->result_[k] - modulus)
                        : obj_KeySwitch->result_[k];

                obj_KeySwitch->result_[k + n_ * decomp_modulus_size_] +=
                    output[size_out + 2 * k + 1];
                obj_KeySwitch->result_[k + n_ * decomp_modulus_size_] =
                    (obj_KeySwitch->result_[k + n_ * decomp_modulus_size_] >=
                     modulus)
                        ? (obj_KeySwitch
                               ->result_[k + n_ * decomp_modulus_size_] -
                           modulus)
                        : (obj_KeySwitch
                               ->result_[k + n_ * decomp_modulus_size_]);
            }
        }
        obj->ready_ = true;
        batch++;
    }
    FPGA_ASSERT(batch == n_batch_);
}

void FPGAObject_DyadicMultiply::fill_out_data(uint64_t* results_in_svm) {
    uint64_t n_data = n_moduli_ * n_ * 3;
    Object_DyadicMultiply* obj_dyadic_multiply =
        dynamic_cast<Object_DyadicMultiply*>(in_objs_.front());
    FPGA_ASSERT(obj_dyadic_multiply);
    memcpy(obj_dyadic_multiply->results_, results_in_svm,
           n_batch_ * n_data * sizeof(uint64_t));
    uint64_t frame_number = 0;
    for (auto& obj : in_objs_) {
        obj->ready_ = true;
        frame_number++;
    }
    FPGA_ASSERT(frame_number == n_batch_);
}

void FPGAObject_NTT::fill_out_data(uint64_t* results_in_svm_) {
    uint64_t coeff_count = n_;
    Object_NTT* obj_NTT = dynamic_cast<Object_NTT*>(in_objs_.front());
    FPGA_ASSERT(obj_NTT);
    memcpy(obj_NTT->coeff_poly_, results_in_svm_,
           n_batch_ * coeff_count * sizeof(uint64_t));
    uint64_t batch = 0;
    for (auto& obj : in_objs_) {
        obj->ready_ = true;
        batch++;
    }
    FPGA_ASSERT(batch == n_batch_);
}

void FPGAObject_INTT::fill_out_data(uint64_t* results_in_svm_) {
    uint64_t coeff_count = n_;
    Object_INTT* obj_INTT = dynamic_cast<Object_INTT*>(in_objs_.front());
    FPGA_ASSERT(obj_INTT);
    memcpy(obj_INTT->coeff_poly_, results_in_svm_,
           n_batch_ * coeff_count * sizeof(uint64_t));
    uint64_t batch = 0;
    for (auto& obj : in_objs_) {
        obj->ready_ = true;
        batch++;
    }
    FPGA_ASSERT(batch == n_batch_);
}

int Device::device_id_ = 0;

const std::unordered_map<std::string, kernel_t> Device::kernels_ =
    std::unordered_map<std::string, kernel_t>{
        {"DYADIC_MULTIPLY", kernel_t::DYADIC_MULTIPLY},
        {"NTT", kernel_t::NTT},
        {"INTT", kernel_t::INTT},
        {"KEYSWITCH", kernel_t::KEYSWITCH},
        {"DYADIC_MULTIPLY_KEYSWITCH", kernel_t::DYADIC_MULTIPLY_KEYSWITCH},
        {"MULTLOWLVL", kernel_t::MULTLOWLVL}};

kernel_t Device::get_kernel_type() {
    kernel_t kernel = kernel_t::DYADIC_MULTIPLY_KEYSWITCH;  // default
    const char* env_kernel = getenv("FPGA_KERNEL");
    if (env_kernel) {
        auto found = kernels_.find(std::string(env_kernel));
        if (found != kernels_.end()) {
            kernel = found->second;
        }
    }
    return kernel;
}

void Device::copyKeySwitchBatch(FPGAObject_KeySwitch* fpga_obj, int obj_id) {
    size_t size_in = fpga_obj->n_ * fpga_obj->decomp_modulus_size_;
    uint64_t frame_number = 0;
    sycl::host_accessor host_access_t_target_iter_ptr_(
        *(fpga_obj->mem_t_target_iter_ptr_));
    for (const auto& obj : fpga_obj->in_objs_) {
        Object_KeySwitch* obj_KeySwitch = dynamic_cast<Object_KeySwitch*>(obj);
        FPGA_ASSERT(obj_KeySwitch);
        memcpy(host_access_t_target_iter_ptr_.get_pointer() +
                   (frame_number * size_in),
               obj_KeySwitch->t_target_iter_ptr_, size_in * sizeof(uint64_t));
        frame_number++;
    }
}

std::string Device::get_bitstream_name() {
    const char* bitstream = getenv("FPGA_BITSTREAM");
    if (bitstream) {
        std::string s(bitstream);
        return s;
    }

    switch (kernel_type_) {
    case kernel_t::DYADIC_MULTIPLY:
        return std::string("libdyadic_multiply.so");
    case kernel_t::NTT:
        return std::string("libfwd_ntt.so");
    case kernel_t::INTT:
        return std::string("libinv_ntt.so");
    case kernel_t::KEYSWITCH:
        return std::string("libkeyswitch.so");
    case kernel_t::DYADIC_MULTIPLY_KEYSWITCH:
        return std::string("libdyadic_multiply_keyswitch.so");
    case kernel_t::MULTLOWLVL:
        return std::string("libmultlowlvl.so");
    default:
        FPGA_ASSERT(0);
        return std::string("bad");
    }
}

Device::Device(sycl::device& p_device, Buffer& buffer,
               std::shared_future<bool> exit_signal, uint64_t coeff_size,
               uint32_t modulus_size, uint64_t batch_size_dyadic_multiply,
               uint64_t batch_size_ntt, uint64_t batch_size_intt,
               uint64_t batch_size_KeySwitch, uint64_t batch_size_MultLowLvl,
               uint32_t debug)
    : device_(p_device),
      buffer_(buffer),
      credit_(CREDIT),
      future_exit_(exit_signal),
      dyadic_multiply_results_out_svm_(nullptr),
      dyadic_multiply_tag_out_svm_(nullptr),
      dyadic_multiply_results_out_valid_svm_(nullptr),
      NTT_coeff_poly_svm_(nullptr),
      INTT_coeff_poly_svm_(nullptr),
      KeySwitch_mem_root_of_unity_powers_(nullptr),
      KeySwitch_load_once_(false),
      root_of_unity_powers_ptr_(nullptr),
      modulus_meta_{},
      invn_{},
      KeySwitch_id_(0),
      keys_map_iter_{},
      debug_(debug),
      ntt_kernel_container_(nullptr),
      intt_kernel_container_(nullptr),
      dyadicmult_kernel_container_(nullptr),
      KeySwitch_kernel_container_(nullptr) {
    
    pi_reorder_primes_index_.resize(2);
    id_ = device_id_++;
    context_ = sycl::context(p_device);
    std::cout << "Creating Command Qs/Acquiring Device ... " << id_
              << std::endl;
    kernel_type_ = get_kernel_type();
    FPGA_ASSERT(kernel_type_ != kernel_t::NONE,
                "Invalid value of env(FPGA_KERNEL)");
    load_kernel_symbols();
    if ((kernel_type_ == kernel_t::DYADIC_MULTIPLY_KEYSWITCH) ||
        (kernel_type_ == kernel_t::DYADIC_MULTIPLY)) {
#ifdef SYCL_DISABLE_PROFILING
        auto cl_queue_properties = sycl::property_list{};
#else
        auto cl_queue_properties =
            sycl::property_list{sycl::property::queue::enable_profiling()};
#endif
        dyadic_multiply_input_queue_ =
            sycl::queue(context_, device_, cl_queue_properties);
        dyadic_multiply_output_queue_ =
            sycl::queue(context_, device_, cl_queue_properties);
        uint64_t size =
            batch_size_dyadic_multiply * 3 * modulus_size * coeff_size;
        dyadic_multiply_results_out_svm_ =
            sycl::malloc_shared<uint64_t>(size, dyadic_multiply_output_queue_);
        dyadic_multiply_tag_out_svm_ =
            sycl::malloc_shared<int>(1, dyadic_multiply_output_queue_);
        dyadic_multiply_results_out_valid_svm_ =
            sycl::malloc_shared<int>(1, dyadic_multiply_output_queue_);
        (*(dyadicmult_kernel_container_->submit_autorun_kernels))(
            dyadic_multiply_input_queue_);
    }

    if (kernel_type_ == kernel_t::INTT) {
#ifdef SYCL_ENABLE_PROFILING
        auto cl_queue_properties =
            sycl::property_list{sycl::property::queue::enable_profiling()};
#else
        auto cl_queue_properties = sycl::property_list{};
#endif
        intt_load_queue_ = sycl::queue(context_, context_.get_devices()[0],
                                       cl_queue_properties);
        intt_store_queue_ = sycl::queue(context_, context_.get_devices()[0],
                                        cl_queue_properties);
        uint64_t size = batch_size_intt * 16834;
        INTT_coeff_poly_svm_ =
            sycl::malloc_shared<uint64_t>(size, intt_load_queue_);
        (*(intt_kernel_container_->inv_ntt))(intt_load_queue_);
    }
    if (kernel_type_ == kernel_t::NTT) {
#ifdef SYCL_ENABLE_PROFILING
        auto cl_queue_properties =
            sycl::property_list{sycl::property::queue::enable_profiling()};
#else
        auto cl_queue_properties = sycl::property_list{};
#endif
        ntt_load_queue_ = sycl::queue(context_, context_.get_devices()[0],
                                      cl_queue_properties);
        ntt_store_queue_ = sycl::queue(context_, context_.get_devices()[0],
                                       cl_queue_properties);
        uint64_t size = batch_size_ntt * 16834;
        NTT_coeff_poly_svm_ =
            (uint64_t*)sycl::malloc_shared<uint64_t>(size, ntt_load_queue_);
        (*(ntt_kernel_container_->fwd_ntt))(ntt_load_queue_);
    }

    if ((kernel_type_ == kernel_t::DYADIC_MULTIPLY_KEYSWITCH) ||
        (kernel_type_ == kernel_t::KEYSWITCH)) {
#ifdef SYCL_ENABLE_PROFILING
        auto cl_queue_properties =
            sycl::property_list{sycl::property::queue::enable_profiling()};
#else
        auto cl_queue_properties = sycl::property_list{};
#endif
        for (int k = 0; k < KEYSWITCH_NUM_KERNELS; ++k) {
            // Create the command queue.
            keyswitch_queues_[k] = sycl::queue(
                context_, context_.get_devices()[0], cl_queue_properties);
        }

        (*(KeySwitch_kernel_container_->launchAllAutoRunKernels))(
            keyswitch_queues_[KEYSWITCH_LOAD]);
    }

    if (kernel_type_ == kernel_t::MULTLOWLVL) {
        std::cout << __func__ << " creating queues for multlowlvl,\n";
#ifdef SYCL_ENABLE_PROFILING
        auto cl_queue_properties =
            sycl::property_list{sycl::property::queue::enable_profiling()};
#else
        auto cl_queue_properties = sycl::property_list{};
#endif
        for (int k = 0; k < MULTLOWLVL_NUM_KERNELS; ++k) {
            // create command queues.
            multlowlvl_queues_[k] = sycl::queue(context_, context_.get_devices()[0],
                cl_queue_properties);
        }
    }

    // DYADIC_MULTIPLY: [0, CREDIT)
    for (int i = 0; i < CREDIT; i++) {
        fpga_objects_.emplace_back(new FPGAObject_DyadicMultiply(
            dyadic_multiply_input_queue_, coeff_size, modulus_size,
            batch_size_dyadic_multiply));
    }
    // INTT: CREDIT
    fpga_objects_.emplace_back(
        new FPGAObject_INTT(intt_load_queue_, 16384, batch_size_intt));
    // NTT:  CREDIT + 1
    fpga_objects_.emplace_back(
        new FPGAObject_NTT(ntt_load_queue_, 16384, batch_size_ntt));
    // KEYSWITCH: CREDIT + 2 and CREDIT + 2 + 1
    for (size_t i = 0; i < 2; i++) {
        fpga_objects_.emplace_back(new FPGAObject_KeySwitch(
            keyswitch_queues_[KEYSWITCH_LOAD], batch_size_KeySwitch));
    }

    // MultlowLvl CREDIT + 4
    fpga_objects_.emplace_back(
        new FPGAObject_MultLowLvl(multlowlvl_queues_[MULTLOWLVL_TENSORPRODUCT], 
        1, 65536, 65537, 20, 20, 16, 26)); // TODO, remove additional params in officiall version.
}

void Device::load_kernel_symbols() {
    std::string bitstream = get_bitstream_name();

    if (kernel_type_ == kernel_t::NTT) {
        ntt_kernel_container_ = new NTTDynamicIF(bitstream);
    } else if (kernel_type_ == kernel_t::INTT) {
        intt_kernel_container_ = new INTTDynamicIF(bitstream);
    } else if (kernel_type_ == kernel_t::DYADIC_MULTIPLY) {
        dyadicmult_kernel_container_ = new DyadicMultDynamicIF(bitstream);
    } else if (kernel_type_ == kernel_t::KEYSWITCH) {
        KeySwitch_kernel_container_ = new KeySwitchDynamicIF(bitstream);
    } else if (kernel_type_ == kernel_t::DYADIC_MULTIPLY_KEYSWITCH) {
        dyadicmult_kernel_container_ = new DyadicMultDynamicIF(bitstream);
        KeySwitch_kernel_container_ = new KeySwitchDynamicIF(bitstream);
    } else if (kernel_type_ == kernel_t::MULTLOWLVL) {
        MultLowLvl_kernel_container_ = new MultLowLvlDynamicIF(bitstream);
    }
}

Device::~Device() {
    device_id_ = 0;
    if (ntt_kernel_container_) delete ntt_kernel_container_;
    if (intt_kernel_container_) delete intt_kernel_container_;
    if (dyadicmult_kernel_container_) delete dyadicmult_kernel_container_;
    if (KeySwitch_kernel_container_) delete KeySwitch_kernel_container_;
    if (MultLowLvl_kernel_container_) delete MultLowLvl_kernel_container_;
    for (auto& fpga_obj : fpga_objects_) {
        if (fpga_obj) {
            delete fpga_obj;
            fpga_obj = nullptr;
        }
    }
    fpga_objects_.clear();

    for (auto& km : keys_map_) {
        delete km.second;
    }
    keys_map_.clear();

    // DYADIC_MULTIPLY section
    if ((kernel_type_ == kernel_t::DYADIC_MULTIPLY_KEYSWITCH) ||
        (kernel_type_ == kernel_t::DYADIC_MULTIPLY)) {
        free(dyadic_multiply_results_out_valid_svm_,
             dyadic_multiply_output_queue_);
        dyadic_multiply_results_out_valid_svm_ = nullptr;
        free(dyadic_multiply_tag_out_svm_, dyadic_multiply_output_queue_);
        dyadic_multiply_tag_out_svm_ = nullptr;
        free(dyadic_multiply_results_out_svm_, dyadic_multiply_output_queue_);
        dyadic_multiply_results_out_svm_ = nullptr;
    }
    // NTT section
    if (kernel_type_ == kernel_t::NTT) {
        free(NTT_coeff_poly_svm_, ntt_load_queue_);
        NTT_coeff_poly_svm_ = nullptr;
    }
    // INTT section
    if (kernel_type_ == kernel_t::INTT) {
        free(INTT_coeff_poly_svm_, context_);
        INTT_coeff_poly_svm_ = nullptr;
    }

    if ((kernel_type_ == kernel_t::DYADIC_MULTIPLY_KEYSWITCH) ||
        (kernel_type_ == kernel_t::KEYSWITCH)) {
        if (root_of_unity_powers_ptr_) {
            // free(root_of_unity_powers_ptr_, context_);
            free(root_of_unity_powers_ptr_);
        }
    }

    if (kernel_type_ == kernel_t::MULTLOWLVL) {
        // TODO, free pointers.
    }

}

void Device::process_blocking_api() {
    process_input(CREDIT);
    process_output_INTT();
}

void Device::run() {
    kernel_t processed_type = kernel_t::NONE;

    while (future_exit_.wait_for(std::chrono::milliseconds(0)) ==
           std::future_status::timeout) {
        if (buffer_.size()) {
            Object* front = buffer_.front();
            processed_type = front->type_;
            switch (front->type_) {
            case kernel_t::DYADIC_MULTIPLY:
                if ((credit_ > 0) && process_input(CREDIT - credit_)) {
                    credit_ -= 1;
                }
                if ((credit_ == 0) && process_output()) {
                    credit_ += 1;
                }
                break;
            case kernel_t::INTT:
                process_input(CREDIT);
                process_output_INTT();
                break;
            case kernel_t::NTT:
                process_input(CREDIT + 1);
                process_output_NTT();
                break;
            case kernel_t::KEYSWITCH:
#ifdef __DEBUG_KS_RUNTIME
                uint64_t lat_start =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
#endif

                process_input(CREDIT + 2 + KeySwitch_id_ % 2);

#ifdef __DEBUG_KS_RUNTIME
                uint64_t lat_in =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
#endif
                process_output_KeySwitch();

#ifdef __DEBUG_KS_RUNTIME
                uint64_t lat_end =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
                std::cout << "KeySwitch output function Latency: "
                          << (lat_end - lat_in) / 1e6 << std::endl;
                std::cout << "KeySwitch input function latency: "
                          << (lat_in - lat_start) / 1e6 << std::endl;
#endif
                KeySwitch_id_++;
                break;
            case kernel_t::MULTLOWLVL:
                std::cout << "FPGA kernel multlowlvl.\n";
                process_input(CREDIT + 4);
                process_output_MultLowLvl();
                break;
            case kernel_t::RELINEARIZE:
                std::cout << "FPGA kernel relinearize.\n";
                process_input(CREDIT + 5);
                process_output_ReLinearize();
                break;
            default:
                FPGA_ASSERT(0, "Invalid kernel!");
                break;
            }
        } else {
            switch (processed_type) {
            case kernel_t::DYADIC_MULTIPLY:
                if ((credit_ < CREDIT) && process_output()) {
                    credit_ += 1;
                }
                break;
            case kernel_t::KEYSWITCH:
                if (KeySwitch_id_ > 0) {
                    FPGAObject* obj = fpga_objects_[CREDIT + 2];
                    FPGAObject_KeySwitch* fpga_obj =
                        dynamic_cast<FPGAObject_KeySwitch*>(obj);
                    uint64_t batch = (buffer_.get_worksize_KeySwitch() +
                                      fpga_obj->batch_size_ - 1) /
                                     fpga_obj->batch_size_;
                    if (batch == KeySwitch_id_) {
                        KeySwitch_read_output();
                        KeySwitch_id_ = 0;
                    }
                }
                break;
            default:
                break;
            }
        }
    }
    std::cout << "Releasing Device ... " << device_id() << std::endl;
}


bool Device::process_input(int credit_id) {

    //std::cout << __func__ << " credit_id: " << credit_id << std::endl;

    std::vector<Object*> objs = buffer_.pop();

    if (objs.empty()) {
        std::cout << "objs empty true.\n";
        return false;
    }

    FPGAObject* fpga_obj = fpga_objects_[credit_id];

    const auto& start_io = std::chrono::high_resolution_clock::now();
    fpga_obj->fill_in_data(objs);  // poylmorphic call
    const auto& end_io = std::chrono::high_resolution_clock::now();

    //std::cout << __func__ << " enquque_input data.\n " << std::endl;
    enqueue_input_data(fpga_obj);

    if (debug_ == 2) {
        const auto& end_api = std::chrono::high_resolution_clock::now();
        const auto& duration_io =
            std::chrono::duration_cast<std::chrono::duration<double>>(end_io -
                                                                      start_io);
        const auto& duration_api =
            std::chrono::duration_cast<std::chrono::duration<double>>(end_api -
                                                                      start_io);

        std::string kernel;
        FPGAObject_DyadicMultiply* fpga_obj_dyadic_multiply =
            dynamic_cast<FPGAObject_DyadicMultiply*>(fpga_obj);
        if (fpga_obj_dyadic_multiply) {
            kernel = "DYADIC_MULTIPLY";
        }
        FPGAObject_NTT* fpga_obj_NTT = dynamic_cast<FPGAObject_NTT*>(fpga_obj);
        if (fpga_obj_NTT) {
            kernel = "NTT";
        }
        FPGAObject_INTT* fpga_obj_INTT =
            dynamic_cast<FPGAObject_INTT*>(fpga_obj);
        if (fpga_obj_INTT) {
            kernel = "INTT";
        }
        FPGAObject_KeySwitch* fpga_obj_KeySwitch =
            dynamic_cast<FPGAObject_KeySwitch*>(fpga_obj);
        if (fpga_obj_KeySwitch) {
            kernel = "KEYSWITCH";
        }

        FPGAObject_MultLowLvl* fpga_obj_MultLowLvl = 
            dynamic_cast<FPGAObject_MultLowLvl*>(fpga_obj);
        if (fpga_obj_MultLowLvl) {
            kernel = "MULTLOWLVL";
        }

        FPGAObject_ReLinearize* fpga_obj_ReLinearize = 
            dynamic_cast<FPGAObject_ReLinearize*>(fpga_obj);
        if (fpga_obj_ReLinearize) {
            kernel = "RELINEARIZE";
        }

        double unit = 1.0e+6;  // microseconds
        std::cout << kernel << " input I/O   time taken: " << std::fixed
                  << std::setprecision(8) << duration_io.count() * unit << " us"
                  << std::endl;
        std::cout << kernel << " input API   time taken: " << std::fixed
                  << std::setprecision(8) << duration_api.count() * unit
                  << " us" << std::endl
                  << std::endl;
    }
    return true;
}

void Device::enqueue_input_data(FPGAObject* fpga_obj) {
    switch (fpga_obj->type_) {
    case kernel_t::DYADIC_MULTIPLY: {
        FPGAObject_DyadicMultiply* fpga_obj_dyadic_multiply =
            dynamic_cast<FPGAObject_DyadicMultiply*>(fpga_obj);
        if (fpga_obj_dyadic_multiply) {
            enqueue_input_data_dyadic_multiply(fpga_obj_dyadic_multiply);
        }
    } break;
    case kernel_t::NTT: {
        FPGAObject_NTT* fpga_obj_NTT = dynamic_cast<FPGAObject_NTT*>(fpga_obj);
        if (fpga_obj_NTT) {
            enqueue_input_data_NTT(fpga_obj_NTT);
        }
    } break;
    case kernel_t::INTT: {
        FPGAObject_INTT* fpga_obj_INTT =
            dynamic_cast<FPGAObject_INTT*>(fpga_obj);
        if (fpga_obj_INTT) {
            enqueue_input_data_INTT(fpga_obj_INTT);
        }
    } break;
    case kernel_t::KEYSWITCH: {
        FPGAObject_KeySwitch* fpga_obj_KeySwitch =
            dynamic_cast<FPGAObject_KeySwitch*>(fpga_obj);
        if (fpga_obj_KeySwitch) {
            enqueue_input_data_KeySwitch(fpga_obj_KeySwitch);
        }
    } break;
    case kernel_t::MULTLOWLVL: {
        FPGAObject_MultLowLvl* fpga_obj_MultLowLvl = 
            dynamic_cast<FPGAObject_MultLowLvl*>(fpga_obj);
        if (fpga_obj_MultLowLvl) {
            enqueue_input_data_MultLowLvl(fpga_obj_MultLowLvl);
        }
    } break;
    case kernel_t::RELINEARIZE: {
        FPGAObject_ReLinearize* fpga_obj_ReLinearize = 
            dynamic_cast<FPGAObject_ReLinearize*>(fpga_obj);
        if (fpga_obj_ReLinearize) {
            enqueue_input_data_ReLinearize(fpga_obj_ReLinearize);
        }
    } break;
    default:
        FPGA_ASSERT(0, "Invalid kernel!")
        break;
    }
}

void Device::enqueue_input_data_dyadic_multiply(
    FPGAObject_DyadicMultiply* fpga_obj) {
    const auto& start_ocl = std::chrono::high_resolution_clock::now();
    auto tempEvent = (*(dyadicmult_kernel_container_->input_fifo_usm))(
        dyadic_multiply_input_queue_, fpga_obj->operand1_in_svm_,
        fpga_obj->operand2_in_svm_, fpga_obj->n_, fpga_obj->moduli_info_,
        fpga_obj->n_moduli_, fpga_obj->tag_, fpga_obj->operands_in_ddr_,
        fpga_obj->results_out_ddr_, fpga_obj->n_batch_);

    if (debug_ == 1) {
        const auto& end_ocl = std::chrono::high_resolution_clock::now();
        const auto& duration_ocl =
            std::chrono::duration_cast<std::chrono::duration<double>>(
                end_ocl - start_ocl);
        double unit = 1.0e+6;  // microseconds
        std::cout << "DYADIC_MULTIPLY"
                  << " OCL-in      time taken: " << std::fixed
                  << std::setprecision(8) << duration_ocl.count() * unit
                  << " us" << std::endl;
        std::cout << "DYADIC_MULTIPLY"
                  << " OCL-in avg  time taken: " << std::fixed
                  << std::setprecision(8)
                  << duration_ocl.count() / fpga_obj->n_batch_ * unit << " us"
                  << std::endl;
    }
}

void Device::enqueue_input_data_INTT(FPGAObject_INTT* fpga_obj) {
    unsigned int batch = fpga_obj->n_batch_;
    const auto& start_ocl = std::chrono::high_resolution_clock::now();
    auto inttLoadEvent = (*(intt_kernel_container_->intt_input))(
        intt_load_queue_, batch, fpga_obj->coeff_poly_in_svm_,
        fpga_obj->coeff_modulus_in_svm_, fpga_obj->inv_n_in_svm_,
        fpga_obj->inv_n_w_in_svm_, fpga_obj->inv_root_of_unity_powers_in_svm_,
        fpga_obj->precon_inv_root_of_unity_powers_in_svm_);
    {
        const auto& end_ocl = std::chrono::high_resolution_clock::now();
        const auto& duration_ocl =
            std::chrono::duration_cast<std::chrono::duration<double>>(
                end_ocl - start_ocl);
        double unit = 1.0e+6;  // microseconds
        if (debug_) {
            std::cout << "INTT"
                      << " OCL-in      time taken: " << std::fixed
                      << std::setprecision(8) << duration_ocl.count() * unit
                      << " us" << std::endl;
            std::cout << "INTT"
                      << " OCL-in avg  time taken: " << std::fixed
                      << std::setprecision(8)
                      << duration_ocl.count() / fpga_obj->n_batch_ * unit
                      << " us" << std::endl;
        }
    }
}

void Device::enqueue_input_data_NTT(FPGAObject_NTT* fpga_obj) {
    unsigned int batch = fpga_obj->n_batch_;
    const auto& start_ocl = std::chrono::high_resolution_clock::now();
    auto nttLoadEvent = (*(ntt_kernel_container_->ntt_input))(
        ntt_load_queue_, batch, fpga_obj->coeff_poly_in_svm_,
        fpga_obj->coeff_poly_in_svm_, fpga_obj->coeff_modulus_in_svm_,
        fpga_obj->root_of_unity_powers_in_svm_,
        fpga_obj->precon_root_of_unity_powers_in_svm_);
    if (debug_ == 1) {
        const auto& end_ocl = std::chrono::high_resolution_clock::now();
        const auto& duration_ocl =
            std::chrono::duration_cast<std::chrono::duration<double>>(
                end_ocl - start_ocl);
        double unit = 1.0e+6;  // microseconds
        std::cout << "NTT"
                  << " OCL-in      time taken: " << std::fixed
                  << std::setprecision(8) << duration_ocl.count() * unit
                  << " us" << std::endl;
        std::cout << "NTT"
                  << " OCL-in avg  time taken: " << std::fixed
                  << std::setprecision(8)
                  << duration_ocl.count() / fpga_obj->n_batch_ * unit << " us"
                  << std::endl;
    }
}
uint64_t Device::precompute_modulus_k(uint64_t modulus) {
    uint64_t k = 0;
    for (uint64_t i = 64; i > 0; i--) {
        if ((1UL << i) >= modulus) {
            k = i;
        }
    }
    return k;
}

void Device::build_modulus_meta(FPGAObject_KeySwitch* obj) {
    for (uint64_t i = 0; i < obj->key_modulus_size_; i++) {
        sycl::ulong4 m;
        m.s0() = obj->moduli_[i];
        m.s1() = MultiplyFactor(1, 64, obj->moduli_[i]).BarrettFactor();
        uint64_t modulus = obj->moduli_[i];
        uint64_t twice_modulus = 2 * modulus;
        uint64_t four_times_modulus = 4 * modulus;
        uint64_t arg2 = obj->modswitch_factors_[i];
        const int InputModFactor = 8;
        arg2 = ReduceMod<InputModFactor>(arg2, modulus, &twice_modulus,
                                         &four_times_modulus);
        m.s2() = arg2;
        uint64_t k = precompute_modulus_k(obj->moduli_[i]);
        __int128 a = 1;
        uint64_t r = (a << (2 * k)) / obj->moduli_[i];
        m.s3() = (r << 8) | k;
        modulus_meta_.data[i] = m;
    }
}

void Device::build_invn_meta(FPGAObject_KeySwitch* obj) {
    for (uint64_t i = 0; i < obj->key_modulus_size_; i++) {
        sycl::ulong4 invn;
        uint64_t inv_n = InverseUIntMod(obj->n_, obj->moduli_[i]);
        uint64_t W_op =
            root_of_unity_powers_ptr_[i * obj->n_ * 4 + obj->n_ - 1];
        uint64_t inv_nw = MultiplyUIntMod(inv_n, W_op, obj->moduli_[i]);
        uint64_t y_barrett_n = DivideUInt128UInt64Lo(inv_n, 0, obj->moduli_[i]);
        uint64_t y_barrett_nw =
            DivideUInt128UInt64Lo(inv_nw, 0, obj->moduli_[i]);
        invn.s0() = inv_n;
        unsigned long k = precompute_modulus_k(obj->moduli_[i]);
        __int128 a = 1;
        unsigned long r = (a << (2 * k)) / obj->moduli_[i];
        invn.s1() = (r << 8) | k;
        invn.s2() = y_barrett_n;
        invn.s3() = y_barrett_nw;
        invn_.data[i] = invn;
    }
}

void Device::KeySwitch_load_twiddles(FPGAObject_KeySwitch* obj) {
    size_t roots_size = obj->n_ * obj->key_modulus_size_ * 4 * sizeof(uint64_t);
    root_of_unity_powers_ptr_ =
        (uint64_t*)aligned_alloc(HOST_MEM_ALIGNMENT, roots_size);
    if (obj->twiddle_factors_) {
        memcpy(root_of_unity_powers_ptr_, obj->twiddle_factors_, roots_size);
    } else {
        std::vector<uint64_t> ms;
        for (uint64_t i = 0; i < obj->key_modulus_size_; i++) {
            ms.push_back(MinimalPrimitiveRoot(2 * obj->n_, obj->moduli_[i]));
        }
        for (uint64_t i = 0; i < obj->key_modulus_size_; i++) {
            ComputeRootOfUnityPowers(
                obj->moduli_[i], obj->n_, Log2(obj->n_), ms[i],
                root_of_unity_powers_ptr_ + i * obj->n_ * 4,
                root_of_unity_powers_ptr_ + i * obj->n_ * 4 + obj->n_,
                root_of_unity_powers_ptr_ + i * obj->n_ * 4 + obj->n_ * 2,
                root_of_unity_powers_ptr_ + i * obj->n_ * 4 + obj->n_ * 3);
        }
    }

    uint64_t key_size = obj->n_ * obj->key_modulus_size_ * 4;
    KeySwitch_mem_root_of_unity_powers_ = new sycl::buffer<uint64_t>(
        root_of_unity_powers_ptr_, sycl::range(key_size),
        {sycl::property::buffer::use_host_ptr{},
         sycl::property::buffer::mem_channel{MEM_CHANNEL_K2}});
    KeySwitch_mem_root_of_unity_powers_->set_write_back(false);
    unsigned reload_twiddle_factors = 1;
    (*(KeySwitch_kernel_container_->launchConfigurableKernels))(
        keyswitch_queues_[KEYSWITCH_LOAD], KeySwitch_mem_root_of_unity_powers_,
        obj->n_, reload_twiddle_factors);
    KeySwitch_load_once_ = true;
}
template <typename t_type>
KeySwitchMemKeys<t_type>::KeySwitchMemKeys(sycl::buffer<t_type>* k1,
                                           sycl::buffer<t_type>* k2,
                                           sycl::buffer<t_type>* k3,
                                           t_type* host_k1, t_type* host_k2,
                                           t_type* host_k3)
    : k_switch_keys_1_(k1),
      k_switch_keys_2_(k2),
      k_switch_keys_3_(k3),
      host_k_switch_keys_1_(host_k1),
      host_k_switch_keys_2_(host_k2),
      host_k_switch_keys_3_(host_k3) {}
template <typename t_type>
KeySwitchMemKeys<t_type>::~KeySwitchMemKeys() {
    if (k_switch_keys_1_) {
        delete k_switch_keys_1_;
    }
    if (k_switch_keys_2_) {
        delete k_switch_keys_2_;
    }
    if (k_switch_keys_3_) {
        // delete k_switch_keys_3_;
    }
    if (host_k_switch_keys_1_) {
        delete host_k_switch_keys_1_;
    }
    if (host_k_switch_keys_2_) {
        delete host_k_switch_keys_2_;
    }
    if (host_k_switch_keys_3_) {
        delete host_k_switch_keys_3_;
    }
}

KeySwitchMemKeys<uint256_t>* Device::KeySwitch_check_keys(uint64_t** keys) {
    keys_map_iter_ = keys_map_.find(keys);
    if (keys_map_iter_ != keys_map_.end()) {
        return keys_map_iter_->second;
    } else {
        return nullptr;
    }
}

KeySwitchMemKeys<uint256_t>* Device::KeySwitch_load_keys(
    FPGAObject_KeySwitch* obj) {
    uint256_t* key_vector1 = (uint256_t*)aligned_alloc(
        HOST_MEM_ALIGNMENT,
        sizeof(uint256_t) * obj->decomp_modulus_size_ * obj->n_);
    uint256_t* key_vector2 = (uint256_t*)aligned_alloc(
        HOST_MEM_ALIGNMENT,
        sizeof(uint256_t) * obj->decomp_modulus_size_ * obj->n_);
    uint256_t* key_vector3 = (uint256_t*)aligned_alloc(
        HOST_MEM_ALIGNMENT,
        sizeof(uint256_t) * obj->decomp_modulus_size_ * obj->n_);

    size_t key_vector_index = 0;
    for (uint64_t k = 0; k < obj->decomp_modulus_size_; k++) {
        for (uint64_t j = 0; j < obj->n_; j++) {
            DyadmultKeys1_t k1 = {};
            DyadmultKeys2_t k2 = {};
            DyadmultKeys3_t k3 = {};
            for (uint64_t i = 0; i < obj->key_modulus_size_; i++) {
                uint64_t key1 = obj->k_switch_keys_[k][i * obj->n_ + j];
                uint64_t key2 =
                    obj->k_switch_keys_[k]
                                       [(i + obj->key_modulus_size_) * obj->n_ +
                                        j];
                if (i == 0) {
                    k1.key1 = key1;
                    k1.key2 = key2;
                } else if (i == 1) {
                    k1.key3 = key1;
                    k1.key4 = key2;
                } else if (i == 2) {
                    k1.key5 = key1 & BIT_MASK(48);
                    k2.key1 = (key1 >> 48) & BIT_MASK(4);
                    k2.key2 = key2;
                } else if (i == 3) {
                    k2.key3 = key1;
                    k2.key4 = key2;
                } else if (i == 4) {
                    k2.key5 = key1;
                    k2.key6 = key2 & BIT_MASK(44);
                    k3.key1 = (key2 >> 44) & BIT_MASK(8);
                } else if (i == 5) {
                    k3.key2 = key1;
                    k3.key3 = key2;
                } else if (i == 6) {
                    k3.key4 = key1;
                    k3.key5 = key2;
                    k3.NOT_USED = 0;
                } else {
                    FPGA_ASSERT(0, "NOT SUPPORTED KEYS");
                }
            }
            key_vector1[key_vector_index] = *((uint256_t*)(&k1));
            key_vector2[key_vector_index] = *((uint256_t*)(&k2));
            key_vector3[key_vector_index] = *((uint256_t*)(&k3));
            key_vector_index++;
        }
    }
    size_t key_size = obj->decomp_modulus_size_ * obj->n_;
    sycl::buffer<uint256_t>* k_switch_keys_1 =
        new sycl::buffer(key_vector1, sycl::range(key_size),
                         {sycl::property::buffer::use_host_ptr{},
                          sycl::property::buffer::mem_channel{MEM_CHANNEL_K2}});
    k_switch_keys_1->set_write_back(false);
    sycl::buffer<uint256_t>* k_switch_keys_2 =
        new sycl::buffer(key_vector2, sycl::range(key_size),
                         {sycl::property::buffer::use_host_ptr{},
                          sycl::property::buffer::mem_channel{MEM_CHANNEL_K3}});
    k_switch_keys_2->set_write_back(false);
    sycl::buffer<uint256_t>* k_switch_keys_3 =
        new sycl::buffer(key_vector3, sycl::range(key_size),
                         {sycl::property::buffer::use_host_ptr{},
                          sycl::property::buffer::mem_channel{MEM_CHANNEL_K4}});
    k_switch_keys_3->set_write_back(false);

    KeySwitchMemKeys<uint256_t>* keys = new KeySwitchMemKeys<uint256_t>(
        k_switch_keys_1, k_switch_keys_2, k_switch_keys_3, key_vector1,
        key_vector2, key_vector3);

    keys_map_.emplace(obj->k_switch_keys_, keys);
    return keys;
}

void Device::enqueue_input_data_KeySwitch(FPGAObject_KeySwitch* fpga_obj) {
    if (!KeySwitch_load_once_) {
        // info: compute and store roots of unity in a table
        // info: also create a sycl buffer
        KeySwitch_load_twiddles(fpga_obj);
    }

    if (fpga_obj->fence_) {
        build_modulus_meta(fpga_obj);
        build_invn_meta(fpga_obj);
    }

    // info: check if keys are already cached on device
    KeySwitchMemKeys<uint256_t>* keys =
        KeySwitch_check_keys(fpga_obj->k_switch_keys_);
    if (!keys) {
        keys = KeySwitch_load_keys(fpga_obj);
    }
    FPGA_ASSERT(keys);
    // info: todo: in latter versions replace with explicit data movement using
    // malloc/q.cpy info: current versions assumes that a buffer that is already
    // cached on device will not be moved by runtime again given that it
    // is marked read only by host run time.
    (*(KeySwitch_kernel_container_->launchStoreSwitchKeys))(
        keyswitch_queues_[KEYSWITCH_LOAD], *(keys->k_switch_keys_1_),
        *(keys->k_switch_keys_2_), *(keys->k_switch_keys_3_),
        fpga_obj->in_objs_.size());

    int obj_id = KeySwitch_id_ % 2;
    copyKeySwitchBatch(fpga_obj, obj_id);

    // copy_buffer_to_device() and wait() is a utility to force blocked write,
    // and to facilitate performance measure on FPGA.
    // The release is to support streaming, and blocking write will slow things
    // down.
    // KeySwitch_events_write_[obj_id][0] = copy_buffer_to_device(
    //     keyswitch_queues_[KEYSWITCH_LOAD],
    //     *(fpga_obj->mem_t_target_iter_ptr_));
    // KeySwitch_events_write_[obj_id][0].wait();

    // =============== Launch keyswitch kernel ==============================
    unsigned rmem = 0;
    if (RWMEM_FLAG) {
        rmem = 1;
    }
    const auto& start_ocl = std::chrono::high_resolution_clock::now();
    KeySwitch_events_enqueue_[obj_id][0] =
        (*(KeySwitch_kernel_container_->load))(
            keyswitch_queues_[KEYSWITCH_LOAD], nullptr,
            *(fpga_obj->mem_t_target_iter_ptr_), modulus_meta_, fpga_obj->n_,
            fpga_obj->decomp_modulus_size_, fpga_obj->n_batch_,
            (*(invn_t*)(void*)&invn_), rmem);

    if (debug_ == 1) {
        const auto& end_ocl = std::chrono::high_resolution_clock::now();
        const auto& duration_ocl =
            std::chrono::duration_cast<std::chrono::duration<double>>(
                end_ocl - start_ocl);
        double unit = 1.0e+6;  // microseconds
        std::cout << "KeySwitch"
                  << " OCL-in      time taken: " << std::fixed
                  << std::setprecision(8) << duration_ocl.count() * unit
                  << " us" << std::endl;
        std::cout << "KeySwitch"
                  << " OCL-in avg  time taken: " << std::fixed
                  << std::setprecision(8)
                  << duration_ocl.count() / fpga_obj->n_batch_ * unit << " us"
                  << std::endl;
    }
}

template <typename T>
void print_vec(std::vector<T> v) {
    for (auto &n : v) 
        std::cout << n << " ";
    std::cout << std::endl;
}

template <int engine>
void Device::LaunchBringToSet(FPGAObject_MultLowLvl* fpga_obj) {

    std::vector<uint64_t> pi;
    std::vector<uint64_t> qj;

    std::vector<uint8_t> qj_primes_index(fpga_obj->output_primes_index_, 
        fpga_obj->output_primes_index_ + fpga_obj->n_batch_ * fpga_obj->c_primes_len_);
    
    std::vector<uint8_t> pi_primes_index;
    FPGA_ASSERT(engine == 1 || engine == 2);
    if (engine == 1) {
        pi_primes_index = std::move(std::vector<uint8_t>(fpga_obj->a_primes_index_, 
                fpga_obj->a_primes_index_ + fpga_obj->n_batch_ * fpga_obj->a_primes_len_));
    } else {
        pi_primes_index = std::move(std::vector<uint8_t>(fpga_obj->b_primes_index_, 
                fpga_obj->b_primes_index_ + fpga_obj->n_batch_ * fpga_obj->b_primes_len_));
    }

    int num_dropped_primes = 0;
    for (auto prime_index : pi_primes_index) {
        num_dropped_primes += (std::find(qj_primes_index.begin(), qj_primes_index.end(), prime_index) ==
         qj_primes_index.end());
    }

    // reoder pi, put the dropped primes to the beginning
    for (size_t i = pi_primes_index.size() - num_dropped_primes;
        i < pi_primes_index.size(); i++) {
        pi_reorder_primes_index_[engine - 1].push_back(pi_primes_index[i]);
        pi.push_back(fpga_obj->all_primes_[pi_primes_index[i]]);
    }
    for (size_t i = 0; i < pi_primes_index.size() - num_dropped_primes; i++) {
        pi_reorder_primes_index_[engine - 1].push_back(pi_primes_index[i]);
        pi.push_back(fpga_obj->all_primes_[pi_primes_index[i]]);
    }

    // fill qj
    for (size_t i = 0; i < qj_primes_index.size(); i++) {
        qj.push_back(fpga_obj->all_primes_[qj_primes_index[i]]);
    }

    assert(pi_reorder_primes_index_[engine - 1].size() == pi_primes_index.size());

    size_t P, Q, I;
    std::vector<sycl::ulong2> scale_param_set;
    std::vector<uint64_t> empty_vec;

    // std::cout << "pi: "; print_vec(pi);
    // std::cout << "qj: "; print_vec(qj);
    // std::cout << "qj_primes_index: "; print_vec(qj_primes_index);


    PreComputeScaleParamSet<false, false>(pi, qj, qj_primes_index, fpga_obj->plainText_,
                                            empty_vec, P, Q, I, scale_param_set);
    
    auto scale_param_set_buf = new buffer<sycl::ulong2>(scale_param_set.size());
    assert(engine == 1 || engine == 2);

    if (engine == 1) {
        std::cout << "launching BringToSet.\n";
        queue_copy(multlowlvl_queues_[MULTLOWLVL_BRINGTOSET1],scale_param_set, scale_param_set_buf);
        MultLowLvl_kernel_container_->BringToSet(multlowlvl_queues_[MULTLOWLVL_BRINGTOSET1], COEFF_COUNT,
                               *scale_param_set_buf, P, Q, I, fpga_obj->plainText_);
    } else {
        std::cout << "launching BringToSet2.\n";
        queue_copy(multlowlvl_queues_[MULTLOWLVL_BRINGTOSET2], scale_param_set, scale_param_set_buf);
        MultLowLvl_kernel_container_->BringToSet2(multlowlvl_queues_[MULTLOWLVL_BRINGTOSET2], COEFF_COUNT,
                                *scale_param_set_buf, P, Q, I, fpga_obj->plainText_);
    }
}


template <int engine>
void Device::Load(FPGAObject_MultLowLvl* fpga_obj, sycl::buffer<uint64_t>* input_buf) {
    auto primes_index_buf = new sycl::buffer<uint8_t>(pi_reorder_primes_index_[engine - 1].size());

    // explicitly copy data from host to device.
    //copy_buffer_to_device(multlowlvl_queues_[MULTLOWLVL_LOAD], *input_buf).wait();

    // copy primes index into device.
    auto e_copy = queue_copy_async(multlowlvl_queues_[MULTLOWLVL_LOAD_A0], pi_reorder_primes_index_[engine - 1], primes_index_buf);

    assert(engine == 1 || engine == 2);
    if (engine == 1) {
        MultLowLvl_kernel_container_->BringToSetLoad(multlowlvl_queues_[MULTLOWLVL_LOAD_B0], e_copy, 
            *input_buf, *primes_index_buf);
    } else {
        MultLowLvl_kernel_container_->BringToSetLoad2(multlowlvl_queues_[MULTLOWLVL_LOAD_B1], e_copy, 
            *input_buf, *primes_index_buf);
    }
}

void Device::TensorProduct(FPGAObject_MultLowLvl* fpga_obj) {
    std::vector<uint8_t> output_primes_index(fpga_obj->output_primes_index_, 
        fpga_obj->output_primes_index_ + fpga_obj->n_batch_ * fpga_obj->c_primes_len_);
    
    std::vector<sycl::ulong4> primes_mulmod;
    for (auto prime_index : output_primes_index) {
        auto prime = fpga_obj->all_primes_[prime_index];
        primes_mulmod.push_back({prime, precompute_modulus_r(prime), precompute_modulus_k(prime), 0});
    }

    primes_mulmod_buf_ = new sycl::buffer<sycl::ulong4>(primes_mulmod.size());
    queue_copy(multlowlvl_queues_[MULTLOWLVL_TENSORPRODUCT], primes_mulmod, primes_mulmod_buf_);

    MultLowLvl_kernel_container_->TensorProduct(multlowlvl_queues_[MULTLOWLVL_TENSORPRODUCT],
                                                *primes_mulmod_buf_);
}

void Device::MultLowLvl_Init(FPGAObject_MultLowLvl* fpga_obj) {
    std::vector<uint64_t> primes(fpga_obj->all_primes_, fpga_obj->all_primes_ + fpga_obj->all_primes_len_);
    
    // launch intt.
    launch_intt<1>(multlowlvl_queues_[MULTLOWLVL_INTT1], fpga_obj->coeff_count_, primes);
    launch_intt<2>(multlowlvl_queues_[MULTLOWLVL_INTT2], fpga_obj->coeff_count_, primes);

    // launch ntt.
    launch_ntt<1>(multlowlvl_queues_[MULTLOWLVL_NTT1], fpga_obj->coeff_count_, primes);
    launch_ntt<2>(multlowlvl_queues_[MULTLOWLVL_NTT2], fpga_obj->coeff_count_, primes);

}


void Device::MultLowLvl_Store(FPGAObject_MultLowLvl* fpga_obj) {

    std::cout << __func__ << "n_batch_: " << fpga_obj->n_batch_ << "\n" <<
        "count_count_ " << fpga_obj->coeff_count_ << ", c_primes_len_ " << fpga_obj->c_primes_len_ << std::endl;

    output1_buf_ = new sycl::buffer<uint64_t>(fpga_obj->n_batch_ * fpga_obj->coeff_count_ * fpga_obj->c_primes_len_);
    for (uint i = 0; i < 2; i++) {
        output2_buf_[i] = new sycl::buffer<uint64_t>(fpga_obj->n_batch_ * fpga_obj->coeff_count_ * fpga_obj->c_primes_len_);
        output3_buf_[i] = new sycl::buffer<uint64_t>(fpga_obj->n_batch_ * fpga_obj->coeff_count_ * fpga_obj->c_primes_len_);
    }

    auto kernel_store0_event = MultLowLvl_kernel_container_->TensorProductStore0(multlowlvl_queues_[MULTLOWLVL_STORE], *output1_buf_);
    auto copy0_event = multlowlvl_queues_[MULTLOWLVL_STORE_COPY].submit([&](sycl::handler &h) {
        // copy
        h.depends_on(kernel_store0_event);
        h.copy(output1_buf_->template get_access<sycl::access::mode::read>(h),
            fpga_obj->mem_output1_);
    });

    copy0_event.wait();

    sycl::event kernel_event[2];
    kernel_event[0] = MultLowLvl_kernel_container_->TensorProductStore12(
      multlowlvl_queues_[MULTLOWLVL_STORE], *output2_buf_[0], *output3_buf_[0]);
    
    int last = 0;
    multlowlvl_queues_[MULTLOWLVL_STORE_COPY].submit([&](sycl::handler &h) {
      // copy
      h.depends_on(kernel_event[last]);
      h.copy(output2_buf_[last]->template get_access<sycl::access::mode::read>(h),
             fpga_obj->mem_output2_);
    });
    multlowlvl_queues_[MULTLOWLVL_STORE_COPY].submit([&](sycl::handler &h) {
      // copy
      h.copy(output3_buf_[last]->template get_access<sycl::access::mode::read>(h),
             fpga_obj->mem_output3_);
    });

    copy0_event.wait();
    kernel_event[0].wait();
    
}


void Device::enqueue_input_data_MultLowLvl(FPGAObject_MultLowLvl* fpga_obj) {
    // launch ntt and intt.
    MultLowLvl_Init(fpga_obj);

    // LaunchBringToSet
    LaunchBringToSet<1>(fpga_obj);
    LaunchBringToSet<2>(fpga_obj);

    // Load, a0, b0.
    Load<1>(fpga_obj, fpga_obj->a0_buf_);
    Load<2>(fpga_obj, fpga_obj->b0_buf_);

    // TensorProduct
    TensorProduct(fpga_obj);

    // Load a1, b1.
    Load<1>(fpga_obj, fpga_obj->a1_buf_);
    Load<2>(fpga_obj, fpga_obj->b1_buf_);

    // Store.
    MultLowLvl_Store(fpga_obj);
}


template <int id>
void Device::launch_ntt_config_tf(sycl::queue &q, uint64_t degree, 
                                  const std::vector<uint64_t> &primes) {
    
    // twiddle factors should be statis as the kernel need to access it
    static std::vector<uint64_t> rootOfUnityPowers;
    uint64_t VEC = MultLowLvl_kernel_container_->ntt_ops_obj[id-1]->get_VEC();
    for (long prime : primes) {
        // create a HEXL ntt instance to get the twiddle factors
        ::intel::hexl::NTT ntt_hexl(degree, prime);

        auto tfdata = ntt_hexl.GetRootOfUnityPowers();

        // push w^N/2, w^N/4, w^N/8, w^N/16 at index 1,2,4,8
        rootOfUnityPowers.push_back(tfdata[1]);
        rootOfUnityPowers.push_back(get_y_barret(tfdata[1], prime));
        rootOfUnityPowers.push_back(tfdata[2]);
        rootOfUnityPowers.push_back(get_y_barret(tfdata[2], prime));
        rootOfUnityPowers.push_back(tfdata[4]);
        rootOfUnityPowers.push_back(get_y_barret(tfdata[4], prime));
        rootOfUnityPowers.push_back(tfdata[8]);
        rootOfUnityPowers.push_back(get_y_barret(tfdata[8], prime));

        // ntt doesn't need to remove the first element as
        // the first VEC of NTT operation only relies on just one element
        // so that the first un-used element can be shifted out
        rootOfUnityPowers.push_back(prime);
        for (uint64_t i = 1; i < tfdata.size() / VEC; i++) {
            rootOfUnityPowers.push_back(tfdata[i * VEC]);
        }
    }

    FPGA_ASSERT(rootOfUnityPowers.size() == (degree / VEC + 8) * primes.size());

#ifdef DEBUG_MULT
    std::cout << "launching ntt_config_tf, id = " << id << std::endl;
#endif

    MultLowLvl_kernel_container_->ntt_ops_obj[id-1]->config_tf(q, rootOfUnityPowers);

}


template <int id>
void Device::launch_compute_forward(sycl::queue &q, uint64_t degree, 
                            const std::vector<uint64_t> &primes) {
    FPGA_ASSERT(primes.size() > 0);
    static std::vector<sycl::ulong4> ntt_configs;
    for (uint64_t prime : primes) {
        sycl::ulong4 config;
        config[0] = prime;
        __int128 a = 1;
        unsigned long k = precompute_modulus_k(prime);
        unsigned long r = (a << (2 * k)) / prime;
        config[1] = r;
        // compute N^{-1} mod prime
        config[2] = ::intel::hexl::InverseMod(degree, prime);
        config[3] = k;
        // std::cout << "r = " << r << ", k = " << k << std::endl;
        ntt_configs.push_back(config);
    }

    FPGA_ASSERT(ntt_configs.size() == primes.size(), "ntt_configs.size() must equals primes.size()");
#ifdef DEBUG_MULT
    std::cout << "launching ntt_compute_forward, id = " << id << std::endl;
#endif

    MultLowLvl_kernel_container_->ntt_ops_obj[id-1]->compute_forward(q, ntt_configs);

}


template <int id>
void Device::launch_ntt(sycl::queue &q, uint64_t degree, 
                        const std::vector<uint64_t> &primes) {
    static bool b_initialized = false;

    if (!b_initialized) {
        b_initialized = true;

        // config the twiddle factor factory kernel
        launch_ntt_config_tf<id>(q, degree, primes);
#ifdef DEBUG_MULT
        std::cout << "launch ntt read in function "<< __FUNCTION__ << ", id = " << id << std::endl; 
#endif
        MultLowLvl_kernel_container_->ntt_ops_obj[id-1]->read(q);

        MultLowLvl_kernel_container_->ntt_ops_obj[id-1]->write(q);

        launch_compute_forward<id>(q, degree, primes);
    }
}


template <int id>
void Device::launch_intt_config_tf(sycl::queue &q, uint64_t degree, 
                                   const std::vector<uint64_t> &primes) {
    static std::vector<uint64_t> invRootOfUnityPowers;
    int VEC = MultLowLvl_kernel_container_->ntt_ops_obj[id-1]->get_VEC();
    for (long prime : primes) {
        // create a HEXL ntt instance to get the twiddle factors
        ::intel::hexl::NTT ntt_hexl(degree, prime);

        // intt needs to remove the first ununsed element as the first VEC of the
        // twiddle factors needs all the elements, while ntt only use just one
        // elements so that it can be shifted out without removing the first unused
        // elements
        auto tmp = ntt_hexl.GetInvRootOfUnityPowers();

        // ignore the first one
        for (int i = 0; i < VEC - 1; i++) {
            auto val = tmp[tmp.size() - VEC + 1 + i];
            invRootOfUnityPowers.push_back(val);
            invRootOfUnityPowers.push_back(get_y_barret(val, prime));
        }
        // append the prime as the size of the twiddle factors should be N
        invRootOfUnityPowers.push_back(prime);
        // no y berrett needed
        invRootOfUnityPowers.push_back(prime);

        for (long i = 0; i < tmp.size() / VEC; i++) {
            invRootOfUnityPowers.push_back(tmp[1 + i * VEC]);
        }

        // The last group: 8,9,10,11,12,13,14,15,4,5,6,7,2,3,1,0
        // w^N/2, w^N/4, w^N/8, w^N/16 at index 1,2,4,8 -> 14,12,8,0
    }

    FPGA_ASSERT(invRootOfUnityPowers.size() == 
        (degree / VEC + VEC * 2) * primes.size(), "invRootOfUnityPowers.size() \
                        must equals (degree / VEC + VEC * 2) * primes.size()");
    
    MultLowLvl_kernel_container_->intt_ops_obj[id-1]->config_tf(q, invRootOfUnityPowers);

}


template <int id>
void Device::launch_compute_inverse(sycl::queue &q, uint64_t degree, 
                                    const std::vector<uint64_t> &primes) {
    static std::vector<sycl::ulong4> intt_configs;
    for (uint64_t prime : primes) {
        sycl::ulong4 intt_config;
        intt_config[0] = prime;
        __int128 a = 1;
        unsigned long k = precompute_modulus_k(prime);
        unsigned long r = (a << (2 * k)) / prime;
        intt_config[1] = r;
        // compute N^{-1} mod prime
        intt_config[2] = ::intel::hexl::InverseMod(degree, prime);
        intt_config[3] = k;
        // std::cout << "r = " << r << ", k = " << k << std::endl;
        intt_configs.push_back(intt_config);
    }

    FPGA_ASSERT(intt_configs.size() == primes.size(), "intt_configs.size() == primes.size()");

    MultLowLvl_kernel_container_->intt_ops_obj[id-1]->compute_inverse(q, intt_configs);
}

template <int id>
void Device::launch_intt(sycl::queue &q, uint64_t degree, 
                         const std::vector<uint64_t> &primes) {
    static bool b_initialized = false;

    if (!b_initialized) {
        b_initialized = true;

        // config the twiddle factor factory kernel
        launch_intt_config_tf<id>(q, degree, primes);

        MultLowLvl_kernel_container_->intt_ops_obj[id-1]->read(q);
        MultLowLvl_kernel_container_->intt_ops_obj[id-1]->write(q);
        MultLowLvl_kernel_container_->intt_ops_obj[id-1]->norm(q);

        launch_compute_inverse<id>(q, degree, primes);
    }
}


uint64_t Device::precompute_modulus_r(uint64_t modulus) {
    __int128 a = 1;
  unsigned long k = precompute_modulus_k(modulus);
  unsigned long r = (a << (2 * k)) / modulus;
  return r;
}


/****************************ReLinearize functions in Device class**************************/

void Device::PreComputeParams(std::vector<uint64_t> &pi,
                          std::vector<uint64_t> &all_primes,
                          std::vector<unsigned> num_designed_digits_primes,
                          unsigned num_special_primes,
                          std::vector<sycl::ulong2> &packed_precomputed_params,
                          std::vector<unsigned> &num_digits_primes) {
    
    
    std::vector<sycl::ulong4> pstar_inv;
    std::vector<sycl::ulong2> pstar_qj;
    std::vector<sycl::ulong> P_qj;

    /* packed parameters */
    // FORMAT:
    // pi and pi recip - all normal primes and special primes
    // pstar_inv and pstar_inv_recip - all normal primes
    // P_qj - num_digit_primes (normal_primes/2) + special primes
    long num_normal_primes = pi.size() - num_special_primes;

    // compute the actual prime size of each digit
    // the last digit size maybe smaller than designed digit size
    int i = 0;
    int num_left_primes = num_normal_primes;
    while (num_left_primes > 0) {
      num_digits_primes.push_back(
          std::min(num_designed_digits_primes[i], (unsigned)num_left_primes));
      num_left_primes -= num_designed_digits_primes[i];
      i++;
    }

    // compute the num of small primes
    long num_small_primes = 0;
    for (size_t i = 0; i < all_primes.size(); i++) {
      if (all_primes[i] == pi[0]) {
        // all the primes before the normal primes are small primes
        num_small_primes = i;
        break;
      }
    }

    // compute the offset of each digit
    std::vector<int> digits_offset;
    int lastDigitsOffset = num_small_primes;
    long num_digits = num_digits_primes.size();
    for (long i = 0; i < num_digits; i++) {
      digits_offset.push_back(lastDigitsOffset);
      lastDigitsOffset += num_digits_primes[i];
    }

    // compute the prod of each digit
    std::vector<NTL::ZZ> P;
    for (long i = 0; i < num_digits; i++) {
      NTL::ZZ tmp{1};
      for (int j = 0; j < num_digits_primes[i]; j++) {
        tmp *= all_primes[digits_offset[i] + j];
      }
      P.push_back(tmp);
    }

    // compute digitsQHatInv
    std::vector<NTL::ZZ> prodOfDesignedDigits;
    std::vector<NTL::ZZ> digitsQHatInv;

    for (long i = 0; i < num_digits; i++) {
      NTL::ZZ tmp{1};
      for (int j = 0; j < num_designed_digits_primes[i]; j++) {
        tmp *= all_primes[digits_offset[i] + j];
      }

      prodOfDesignedDigits.push_back(tmp);
    }

    // compute QHatInv
    for (long i = 0; i < num_digits; i++) {
      NTL::ZZ qhat{1};
      for (long j = 0; j < num_digits; j++) {
        if (j != i) {
          qhat *= prodOfDesignedDigits[j];
        }
      }
      auto qhat_inv =
          NTL::InvMod(qhat % prodOfDesignedDigits[i], prodOfDesignedDigits[i]);
      digitsQHatInv.push_back(qhat_inv);
    }

    // gererate the qj primes of each digits
    std::vector<uint64_t> digit_qj_primes[MAX_DIGITS];

    for (long j = 0; j < num_digits; j++) {
      for (long i = 0; i < pi.size(); i++) {
        if (i < digits_offset[j] ||
            i >= (digits_offset[j] + num_digits_primes[j]))
          digit_qj_primes[j].push_back(pi[i]);
      }
    }

    // pstar_inv has all the primes
    for (long j = 0; j < num_digits; j++) {
      for (long i = digits_offset[j];
           i < digits_offset[j] + num_digits_primes[j]; i++) {
        ulong p_star_inv_i = NTL::InvMod(NTL::rem(P[j] / pi[i], pi[i]), pi[i]);
        auto tmp = mulmod_y_ext(p_star_inv_i, pi[i]);
        auto tmp2 = mulmod_y_ext(NTL::InvMod(p_star_inv_i, pi[i]), pi[i]);

        pstar_inv.push_back({tmp.s0(), tmp.s1(), tmp2.s0(), tmp2.s1()});
      }
    }

    // std::cout << "pstar_inv.size() = " << pstar_inv.size() << std::endl;
    assert(num_normal_primes == pstar_inv.size());

    // compute pstar_qj
    for (int i = 0; i < MAX_SPECIAL_PRIMES + MAX_NORMAL_PRIMES / 2; i++) {
      for (long k = 0; k < num_digits; k++) {
        for (int j = 0; j < MAX_NORMAL_PRIMES / 2; j++) {
          // P* mod qj
          auto tmp = i < digit_qj_primes[k].size() && j < num_digits_primes[k]
                         ? NTL::rem(P[k] / all_primes[digits_offset[k] + j],
                                    digit_qj_primes[k][i])
                         : 0;
          pstar_qj.push_back(mulmod_y_ext(tmp, digit_qj_primes[k][i]));
        }
      }
    }

    // comput P_qj
    for (long j = 0; j < num_digits; j++)
      for (int i = 0; i < pi.size(); i++) {
        P_qj.push_back(i < digit_qj_primes[j].size()
                           ? NTL::rem(P[j], digit_qj_primes[j][i])
                           : 0);
      }

    // compute pi_recip
    std::vector<ulong2> pi_with_recip;
    for (int i = 0; i < pi.size(); i++) {
      double pi_recip = (double)1 / pi[i];
      pi_with_recip.push_back({pi[i], *(ulong *)&pi_recip});
    }

    // packing now
    // pi and pi recip - all normal primes and special primes
    for (size_t i = 0; i < pi.size(); i++) {
      packed_precomuted_params.push_back(pi_with_recip[i]);
    }

    // pstar_inv and pstar_inv_recip - all normal primes
    for (size_t i = 0; i < pstar_inv.size(); i++) {
      auto tmp = pstar_inv[i];
      packed_precomuted_params.push_back({tmp.s0(), tmp.s1()});
    }

    // pstar_inv_recip - all normal primes
    for (size_t i = 0; i < pstar_inv.size(); i++) {
      auto tmp = pstar_inv[i];
      packed_precomuted_params.push_back({tmp.s2(), tmp.s3()});
    }

    // P_qj - pi.size() * 2
    for (size_t i = 0; i < P_qj.size(); i++) {
      packed_precomuted_params.push_back({P_qj[i], 0});
    }

    for (long j = 0; j < num_digits; j++) {
      for (int i = 0; i < num_digits_primes[j]; i++) {
        auto pi = all_primes[digits_offset[j] + i];
        long qhat_inv_pi = NTL::rem(digitsQHatInv[j], pi);
        packed_precomuted_params.push_back(mulmod_y_ext(qhat_inv_pi, pi));
      }
    }

    for (long k = 0; k < num_digits; k++) {
      for (int i = 0; i < MAX_SPECIAL_PRIMES + MAX_NORMAL_PRIMES; i++) {
        for (int j = 0; j < MAX_DIGIT_SIZE; j++) {
          if (i < digit_qj_primes[k].size() && j < num_digits_primes[k]) {
            // P mod qj
            auto tmp = NTL::rem(P[k] / all_primes[digits_offset[k] + j],
                                digit_qj_primes[k][i]);
            packed_precomuted_params.push_back(
                mulmod_y_ext(tmp, digit_qj_primes[k][i]));
          } else {
            packed_precomuted_params.push_back({0, 0});
          }
        }
      }
    }

}

sycl::event Device::breakintodigits_ProcessInput(FPGAObject_ReLinearize* fpga_obj) {
    breakintodigits_store_events_[keyswitchdigits_buf_index_].wait();

    // *breakintodigits_queues_[BREAKINFODIGITS_LOAD].copy(fpga_obj->c2_buf_->template get_access<sycl::access::mode::read>(), 
    //     breakintodigits_input_buffer_[breakintodigits_buf_index_]->template get_access<sycl::access::mode::discard_write>());
    // *breakintodigits_queues_[BREAKINFODIGITS_LOAD].wait();

     queue_copy_buf2buf(*breakintodigits_queues_[BREAKINFODIGITS_LOAD_DATA], 
                        *fpga_obj->c2_buf_,
                        *breakintodigits_input_buffer_[breakintodigits_buf_index_]);

    ReLinearize_kernel_container_->breakintodigit_ops().load(*breakintodigits_queues_[BREAKINFODIGITS_LOAD],
                                                             *breakintodigits_input_buffer_[breakintodigits_buf_index_],
                                                             fpga_obj->c2_buf_->get_count());
    std::vector<sycl::ulong2> packed_precomuted_params;
    std::vector<unsigned> num_digits_primes;

    // construct vectors
    std::vector<uint64_t> pi(fpga_obj->pi_ fpga_obj->pi_ + fpga_obj->pi_len_);
    std::vector<uint64_t> all_primes(fpga_obj->all_primes_, fpga_obj->all_primes_ + fpga_obj->all_primes_len_);
    std::vector<unsigned> num_designed_digits_primes(fpga_obj->num_designed_digits_primes_,
                                            fpga_obj->num_designed_digits_primes_ + fpga_obj->digits_primes_len_);
    unsigned num_special_primes = fpga_obj->num_special_primes_;

    PreComputeParams(pi, all_primes, num_designed_digits_primes, 
                     num_special_primes, packed_precomuted_params, num_digits_primes);

    queue_copy(*breakintodigits_queues_[BREAKINFODIGITS_LOAD_DATA], packed_precomuted_params, 
        *breakinto_digits_packed_precomputed_params_buf_[breakintodigits_buf_index_]);
    
    // launch breakIntoDigits
    unsigned num_digits = num_digits_primes.size();
    unsigned num_digit1_primes = num_digits_primes[0];
    unsigned num_digit2_primes = num_digits > 1 ? num_digits_primes[1] : 0;
    unsigned num_digit3_primes = num_digits > 2 ? num_digits_primes[2] : 0;
    unsigned num_digit4_primes = num_digits > 3 ? num_digits_primes[3] : 0;

    // desinged means all normal primes
    uint num_designed_normal_primes = 0;
    for (int i = 0; i < num_designed_digits_primes.size(); i++) {
      num_designed_normal_primes += num_designed_digits_primes[i];
    }

    // pi includes the special primes
    auto num_output_primes = pi.size();
    auto output_size = num_output_primes * COEFF_COUNT * num_digits;

    ReLinearize_kernel_container_->breakintodigit_ops().kernel(*breakintodigits_queues_[BREAKINTODIGITS_KERN],
        *breakinto_digits_packed_precomputed_params_buf_[breakintodigits_buf_index_],
        num_digits, num_digit1_primes, num_digit2_primes, num_digit3_primes,
        num_digit4_primes, num_special_primes, num_designed_normal_primes,
        0b1111);
    
    breakintodigits_store_events_[breakintodigits_buf_index_] = ReLinearize_kernel_container_->breakintodigit_ops().store(
        *breakintodigits_queues_[BREAKINTODIGITS_KERN],
        *breakintodigits_output_buffer_[BREAKINTODIGITS_STORE],
        output_size,
        0b111,
    );

#if SYNC_MODE
    breakintodigits_store_events_[breakintodigits_buf_index_].wait();
#endif

    return breakintodigits_store_events_[breakintodigits_buf_index_];

}

int Device::BKdigits_GetLastBufIndex() {
    return (breakintodigits_buf_depth_ + breakintodigits_buf_index_ - 1) % breakintodigits_buf_depth_;
}

sycl::buffer<uint64_t>& Device::BKdigits_GetLastOutputBuf() {
    auto last_buf_index = BKdigits_GetLastBufIndex();
    return *breakintodigits_output_buffer_[last_buf_index];
}


sycl::event Device::keyswitchdigits_ProcessInput(FPGAObject_ReLinearize* fpga_obj,
                                                sycl::buffer<uint64_t> &digits,
                                                sycl::ulong4 digits_offset,
                                                sycl::event dpend_event) {
    keyswitchdigits_store_events_[keyswitchdigits_buf_index_].wait();

    // construct vectors.
    std::vector<uint8_t> primes_index(fpga_obj->primes_index_, fpga_obj->primes_index_ + fpga_obj->primes_index_len_);


    // compute the diff value to prepare for the next prime
    // 0,0,0,1,1 -> 0,0,1,0,1
    // the last one doesn't matter

    auto num_primes = primes_index.size();
    std::vector<sycl::ulong4> primes_keyswitch_digits;
    std::vector<int> primes_index_offset(primes_index.size());
    for (size_t i = 0; i < primes_index.size(); i++) {
      primes_index_offset[i] = primes_index[i] - i;
    }
    for (size_t i = 0; i < primes_index.size() - 1; i++) {
      primes_index_offset[i] =
          primes_index_offset[i + 1] - primes_index_offset[i];
    }

    // pre-computing r and k for primes
    for (size_t i = 0; i < primes_index.size(); i++) {
      auto prime = all_primes_[primes_index[i]];
      ulong4 tmp;
      tmp.s0() = prime;
      tmp.s1() = primes_index_offset[i];
      tmp.s2() = precompute_modulus_r(prime);
      tmp.s3() = precompute_modulus_k(prime);
      primes_keyswitch_digits.push_back(tmp);
    }

    queue_copy(*keyswitchdigits_queues_[*KEYSWITCHDIGITS_LOAD_DATA], primes_keyswitch_digits,
        *keyswitchdigits_packed_precomputed_params_buf_[keyswitchdigits_buf_index_]);
    
    int num_digits = 4;
    keyswitchdigits_store_events_[keyswitchdigits_buf_index_] = 
        ReLinearize_kernel_container_->keyswitchdigits_ops().KeySwitchDigits(
            *keyswitchdigits_queues_[*KEYSWITCHDIGITS_LOAD_DATA],
            *keyswitchdigits_packed_precomputed_params_buf_[keyswitchdigits_buf_index_],
            *keyswitchdigits_keys_buffer_[0],
            *keyswitchdigits_keys_buffer_[1],
            *keyswitchdigits_keys_buffer_[2],
            *keyswitchdigits_keys_buffer_[3],
            digits, digits, digits, digits,
            *keyswitchdigit_output_buffer_[keyswitchdigits_buf_index_],
            *keyswitchdigit_output_buffer_[keyswitchdigits_buf_index_],
            num_dgits, num_primes, digits_offset,
            depend_event, 0xff);

#if SYNC_MODE
    keyswitchdigits_store_events_[keyswitchdigits_buf_index_].wait();
#endif

    return keyswitchdigits_store_events_[keyswitchdigits_buf_index_];

}

void Device::keyswitchdigits_ProcessOutput (int output_buf_index) {
    if (!keyswitchdigits_output_ptr_[output_buf_index]) return;

    keyswitchdigits_store_events_[output_buf_index].wait();

    keyswitchdigits_queues_[KEYSWITCHDIGITS_STORE]->submit([&] (sycl::handler &h) {
        h.depends_on(keyswitchdigits_store_events_[output_buf_index]);
        h.copy(keyswitchdigit_output_buffer_[output_buf_index]->template get_access<sycl::access::mode::read>(
            h, sycl::range<1>(keyswitchdigits_output_size_[output_buf_index])), 
            keyswitchdigits_output_ptr_[output_buf_index]);
    });

    keyswitchdigits_queues_[KEYSWITCHDIGITS_STORE]->wait();
    keyswitchdigits_output_ptr_[output_buf_index] = nullptr;
}

int Device::KSdigits_GetNextBufIndex() {
    return (keyswitchdigits_buf_index_ + 1) % keyswitchdigits_buf_depth_;
}


void Device::KSdigits_ProcessLeftOutput() {
    for (int i = 0; i < keyswitchdigits_buf_depth_;  i++) {
        int current_index = (keyswitchdigits_buf_index_ + i) % keyswitchdigits_buf_depth_;
        keyswitchdigits_ProcessOutput(current_index);
    }
}


// TODO, modify this functions.
void Device::enqueue_input_data_ReLinearize(FPGAObject_ReLinearize* fpga_obj) {
    // launch ntt and intt.
    ReLinearize_Init(fpga_obj);

    // breakintodigit_processInput.
    sycl::event e = breakintodigits_ProcessInput(fpga_obj);
    breakintodigits_buf_index_ = (breakintodigits_buf_index_ + 1) % breakintodigits_buf_depth_;

    // breakintodigits_CopyOutput.
    breakintodigits_CopyOutput(fpga_obj);

    // PreComputeParams.
    PreComputeParams(fpga_obj);

    // TODO, add keyswitch_Enqueue, keyswitch_digits section functions.
    keyswitchdigits_output_ptr_[keyswitchdigits_buf_index_] = fpga_obj->output_;
    keyswitchdigits_output_size_[keyswitchdigits_buf_index_] = fpga_obj->output_len_;
    keyswitchdigits_ProcessOutput(KSdigits_GetNextBufIndex());
    keyswitchdigits_ProcessInput(fpga_obj);

}



/*******************************************************************************************/

bool Device::process_output() {
    bool rsl = false;
    rsl |= process_output_dyadic_multiply();
    return rsl;
}

bool Device::process_output_dyadic_multiply() {
    bool rsl = false;

    dyadic_multiply_tag_out_svm_[0] = -1;
    dyadic_multiply_results_out_valid_svm_[0] = 0;
    const auto& start_ocl = std::chrono::high_resolution_clock::now();
    auto tempEvent = (*(dyadicmult_kernel_container_->output_nb_fifo_usm))(
        dyadic_multiply_output_queue_, dyadic_multiply_results_out_svm_,
        dyadic_multiply_tag_out_svm_, dyadic_multiply_results_out_valid_svm_);
    dyadic_multiply_output_queue_.wait();
    const auto& end_ocl = std::chrono::high_resolution_clock::now();

    if (*dyadic_multiply_results_out_valid_svm_ == 1) {
        FPGA_ASSERT(dyadic_multiply_tag_out_svm_[0] >= 0);

        const auto& start_io = std::chrono::high_resolution_clock::now();
        FPGAObject* completed = nullptr;
        for (int credit = 0; credit < CREDIT; credit++) {
            if (fpga_objects_[credit]->tag_ ==
                dyadic_multiply_tag_out_svm_[0]) {
                completed = fpga_objects_[credit];
                break;
            }
        }

        if (completed) {
            completed->fill_out_data(dyadic_multiply_results_out_svm_);
            completed->recycle();
            rsl = true;

            for (int credit = 0; credit < CREDIT - 1; credit++) {
                fpga_objects_[credit] = fpga_objects_[credit + 1];
            }
            fpga_objects_[CREDIT - 1] = completed;

            if (debug_) {
                const auto& end_io = std::chrono::high_resolution_clock::now();
                const auto& duration_io =
                    std::chrono::duration_cast<std::chrono::duration<double>>(
                        end_io - start_io);
                const auto& duration_ocl =
                    std::chrono::duration_cast<std::chrono::duration<double>>(
                        end_ocl - start_ocl);
                const auto& duration_api =
                    std::chrono::duration_cast<std::chrono::duration<double>>(
                        end_io - start_ocl);

                double unit = 1.0e+6;  // microseconds
                if (debug_ == 1) {
                    std::cout << "DYADIC_MULTIPLY OCL-out     time taken: "
                              << std::fixed << std::setprecision(8)
                              << duration_ocl.count() * unit << " us"
                              << std::endl;
                    std::cout
                        << "DYADIC_MULTIPLY OCL-out avg time taken: "
                        << std::fixed << std::setprecision(8)
                        << duration_ocl.count() / completed->n_batch_ * unit
                        << " us" << std::endl;
                }
                if (debug_ == 2) {
                    std::cout << "DYADIC_MULTIPLY out I/O     time taken: "
                              << std::fixed << std::setprecision(8)
                              << duration_io.count() * unit << " us"
                              << std::endl;
                    std::cout << "DYADIC_MULTIPLY out API     time taken: "
                              << std::fixed << std::setprecision(8)
                              << duration_api.count() * unit << " us"
                              << std::endl
                              << std::endl;
                }
            }
        } else {
            FPGA_ASSERT(0);
        }
    }

    return rsl;
}

bool Device::process_output_NTT() {
    unsigned int batch = 1;
    int ntt_instance_index = CREDIT + 1;
    FPGAObject* completed = fpga_objects_[ntt_instance_index];
    FPGAObject_NTT* kernel_inf = dynamic_cast<FPGAObject_NTT*>(completed);
    FPGA_ASSERT(kernel_inf);
    batch = kernel_inf->n_batch_;

    const auto& start_ocl = std::chrono::high_resolution_clock::now();
    auto nttStoreEvent = (*(ntt_kernel_container_->ntt_output))(
        ntt_store_queue_, batch, NTT_coeff_poly_svm_);

    ntt_store_queue_.wait();
    const auto& end_ocl = std::chrono::high_resolution_clock::now();

    const auto& start_io = std::chrono::high_resolution_clock::now();
    completed->fill_out_data(NTT_coeff_poly_svm_);
    completed->recycle();

    if (debug_) {
        const auto& end_io = std::chrono::high_resolution_clock::now();
        const auto& duration_io =
            std::chrono::duration_cast<std::chrono::duration<double>>(end_io -
                                                                      start_io);
        const auto& duration_ocl =
            std::chrono::duration_cast<std::chrono::duration<double>>(
                end_ocl - start_ocl);
        const auto& duration_api =
            std::chrono::duration_cast<std::chrono::duration<double>>(
                end_io - start_ocl);

        double unit = 1.0e+6;  // microseconds
        if (debug_ == 1) {
            std::cout << "NTT OCL-out     time taken: " << std::fixed
                      << std::setprecision(8) << duration_ocl.count() * unit
                      << " us" << std::endl;
            std::cout << "NTT OCL-out avg time taken: " << std::fixed
                      << std::setprecision(8)
                      << duration_ocl.count() / completed->n_batch_ * unit
                      << " us" << std::endl;
        }
        if (debug_ == 2) {
            std::cout << "NTT out I/O     time taken: " << std::fixed
                      << std::setprecision(8) << duration_io.count() * unit
                      << " us" << std::endl;
            std::cout << "NTT out API     time taken: " << std::fixed
                      << std::setprecision(8) << duration_api.count() * unit
                      << " us" << std::endl
                      << std::endl;
        }
    }

    return 0;
}

bool Device::process_output_INTT() {
    unsigned int batch = 1;
    int intt_instance_index = CREDIT;

    FPGAObject* completed = fpga_objects_[intt_instance_index];
    FPGAObject_INTT* kernel_inf = dynamic_cast<FPGAObject_INTT*>(completed);

    FPGA_ASSERT(kernel_inf);

    batch = kernel_inf->n_batch_;

    const auto& start_ocl = std::chrono::high_resolution_clock::now();

    auto inttLoadEvent = (*(intt_kernel_container_->intt_output))(
        intt_store_queue_, batch, INTT_coeff_poly_svm_);
    intt_store_queue_.wait();
    const auto& end_ocl = std::chrono::high_resolution_clock::now();
    const auto& start_io = std::chrono::high_resolution_clock::now();
    completed->fill_out_data(INTT_coeff_poly_svm_);
    completed->recycle();
    if (debug_) {
        const auto& end_io = std::chrono::high_resolution_clock::now();
        const auto& duration_io =
            std::chrono::duration_cast<std::chrono::duration<double>>(end_io -
                                                                      start_io);
        const auto& duration_ocl =
            std::chrono::duration_cast<std::chrono::duration<double>>(
                end_ocl - start_ocl);
        const auto& duration_api =
            std::chrono::duration_cast<std::chrono::duration<double>>(
                end_io - start_ocl);

        double unit = 1.0e+6;  // microseconds
        if (debug_ == 1) {
            std::cout << "INTT OCL-out     time taken: " << std::fixed
                      << std::setprecision(8) << duration_ocl.count() * unit
                      << " us" << std::endl;
            std::cout << "INTT OCL-out avg time taken: " << std::fixed
                      << std::setprecision(8)
                      << duration_ocl.count() / completed->n_batch_ * unit
                      << " us" << std::endl;
        }
        if (debug_ == 2) {
            std::cout << "INTT out I/O     time taken: " << std::fixed
                      << std::setprecision(8) << duration_io.count() * unit
                      << " us" << std::endl;
            std::cout << "INTT out API     time taken: " << std::fixed
                      << std::setprecision(8) << duration_api.count() * unit
                      << " us" << std::endl
                      << std::endl;
        }
    }

    return 0;
}

void Device::KeySwitch_read_output() {
    int peer_id = (KeySwitch_id_ + 1) % 2;
    FPGAObject* peer = fpga_objects_[CREDIT + 2 + peer_id];
    FPGAObject_KeySwitch* peer_obj = dynamic_cast<FPGAObject_KeySwitch*>(peer);
    FPGA_ASSERT(peer_obj);
    size_t size_in =
        peer_obj->n_batch_ * peer_obj->n_ * peer_obj->decomp_modulus_size_;
    uint64_t size_out = size_in * peer_obj->key_component_count_;
    size_t result_size = size_out * sizeof(uint64_t);
// exhaust wait list
#ifdef __DEBUG_KS_RUNTIME
    auto lat_start = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
#endif
    KeySwitch_events_enqueue_[peer_id][0].wait();
    KeySwitch_events_enqueue_[peer_id][1].wait();
#ifdef __DEBUG_KS_RUNTIME
    auto lat_end = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
    std::cout << "KeySwitch KeySwitch_read_output latency: "
              << (lat_end - lat_start) / 1e6 << std::endl;
#endif
    sycl::host_accessor result_access(*(peer_obj->mem_KeySwitch_results_));
    memcpy(peer_obj->ms_output_, result_access.get_pointer(), result_size);
    peer->fill_out_data(peer_obj->ms_output_);
    peer->recycle();
}
bool Device::process_output_KeySwitch() {
    int obj_id = KeySwitch_id_ % 2;
    int KeySwitch_instance_index = CREDIT + 2 + obj_id;
    FPGAObject* completed = fpga_objects_[KeySwitch_instance_index];
    FPGAObject_KeySwitch* fpga_obj =
        dynamic_cast<FPGAObject_KeySwitch*>(completed);
    FPGA_ASSERT(fpga_obj);
    const auto& start_ocl = std::chrono::high_resolution_clock::now();
    unsigned rmem = 0;
    unsigned wmem = 0;
    if (RWMEM_FLAG) {
        rmem = 1;
        wmem = 1;
    }
    KeySwitch_events_enqueue_[obj_id][1] =
        (*(KeySwitch_kernel_container_->store))(
            keyswitch_queues_[KEYSWITCH_STORE], KeySwitch_events_write_[obj_id],
            *(fpga_obj->mem_KeySwitch_results_), fpga_obj->n_batch_,
            fpga_obj->n_, fpga_obj->decomp_modulus_size_, modulus_meta_, rmem,
            wmem);
    keyswitch_queues_[KEYSWITCH_STORE].wait();
    const auto& end_ocl = std::chrono::high_resolution_clock::now();

    const auto& start_io = std::chrono::high_resolution_clock::now();
    if (KeySwitch_id_ > 0) {
        KeySwitch_read_output();
    }

    if (debug_) {
        const auto& end_io = std::chrono::high_resolution_clock::now();
        const auto& duration_io =
            std::chrono::duration_cast<std::chrono::duration<double>>(end_io -
                                                                      start_io);
        const auto& duration_ocl =
            std::chrono::duration_cast<std::chrono::duration<double>>(
                end_ocl - start_ocl);
        const auto& duration_api =
            std::chrono::duration_cast<std::chrono::duration<double>>(
                end_io - start_ocl);

        double unit = 1.0e+6;  // microseconds
        if (debug_ == 1) {
            std::cout << "KeySwitch OCL-out     time taken: " << std::fixed
                      << std::setprecision(8) << duration_ocl.count() * unit
                      << " us" << std::endl;
            std::cout << "KeySwitch OCL-out avg time taken: " << std::fixed
                      << std::setprecision(8)
                      << duration_ocl.count() / completed->n_batch_ * unit
                      << " us" << std::endl;
        }
        if (debug_ == 2) {
            std::cout << "KeySwitch out I/O     time taken: " << std::fixed
                      << std::setprecision(8) << duration_io.count() * unit
                      << " us" << std::endl;
            std::cout << "KeySwitch out API     time taken: " << std::fixed
                      << std::setprecision(8) << duration_api.count() * unit
                      << " us" << std::endl
                      << std::endl;
        }
    }
    return 0;
}

void Device::MultLowLvl_read_output(FPGAObject_MultLowLvl* fpga_obj) {
#if 1
    sycl::host_accessor output1_acc(*output1_buf_);
    sycl::host_accessor output2_acc(*output2_buf_[0]);
    sycl::host_accessor output3_acc(*output3_buf_[0]);

    memcpy(fpga_obj->mem_output1_, output1_acc.get_pointer(), fpga_obj->n_batch_ * fpga_obj->coeff_count_ *
            fpga_obj->c_primes_len_ * sizeof(uint64_t));
    memcpy(fpga_obj->mem_output2_, output2_acc.get_pointer(), fpga_obj->n_batch_ * fpga_obj->coeff_count_ *
            fpga_obj->c_primes_len_ * sizeof(uint64_t));
    memcpy(fpga_obj->mem_output3_, output3_acc.get_pointer(), fpga_obj->n_batch_ * fpga_obj->coeff_count_ *
            fpga_obj->c_primes_len_ * sizeof(uint64_t));
#endif
}


bool Device::process_output_MultLowLvl() {
    int multlowlvl_index = CREDIT + 4;

    FPGAObject* completed = fpga_objects_[multlowlvl_index];
    FPGAObject_MultLowLvl* kernel_inf = dynamic_cast<FPGAObject_MultLowLvl*>(completed);

    FPGA_ASSERT(kernel_inf);

    int batch = kernel_inf->n_batch_;
    std::cout << __func__ << " batch = " << batch << std::endl;
    MultLowLvl_read_output(kernel_inf);
    completed->fill_out_data(nullptr);
    completed->recycle();

}

void DevicePool::getDevices(int numDevicesToUse, int choice) {
    /**
     * @brief runtime selection of emulator or hardware
     * when RUN_CHOICE=1, emulator_selector will be used,
     * when RUN_CHOICE=2 hardware_selector will be used.
     */
    sycl::ext::intel::fpga_emulator_selector emulator_selector;
    sycl::ext::intel::fpga_selector hardware_selector;
    sycl::device dev;
    if (choice == 1) {
        dev = emulator_selector.select_device();
        std::cout << "Using Emulation mode device selector ..." << std::endl;
    } else if (choice == 2) {
        dev = hardware_selector.select_device();
        std::cout << "Using Hardware mode device selector ..." << std::endl;
    } else {
        std::cout << "select the right run mode EMU{1}, FPGA{2}." << std::endl;
    }

    sycl::platform platform = dev.get_platform();
    device_list_ = platform.get_devices();
    std::cout << "Number of devices found: " << device_list_.size()
              << std::endl;

    for (auto& dev : device_list_) {
        std::cout << "   FPGA:  " << dev.get_info<sycl::info::device::name>()
                  << "\n";
    }
    if ((numDevicesToUse > device_list_.size()) || (numDevicesToUse < 0)) {
        std::cout << "   [WARN] Maximal NUM_DEV is " << device_count_
                  << " on this platform." << std::endl;
        device_count_ = device_list_.size();
    } else {
        device_count_ = numDevicesToUse;
    }
}

DevicePool::DevicePool(int choice, Buffer& buffer,
                       std::future<bool>& exit_signal, uint64_t coeff_size,
                       uint32_t modulus_size,
                       uint64_t batch_size_dyadic_multiply,
                       uint64_t batch_size_ntt, uint64_t batch_size_intt,
                       uint64_t batch_size_KeySwitch, 
                       uint64_t batch_size_MultLowLvl,uint32_t debug) {
    cl_uint dev_count_user = 1;

    // Get number of devices user wants to use from environement var
    const char* n_dev_user = getenv("NUM_DEV");
    if (n_dev_user) {
        dev_count_user = atoi(n_dev_user);
    }
    getDevices(dev_count_user, choice);
    std::cout << "   [INFO] Using " << device_count_ << " FPGA device(s)."
              << std::endl;

    future_exit_ = exit_signal.share();
    devices_ = new Device*[device_count_];
    for (unsigned int i = 0; i < device_count_; i++) {
        devices_[i] =
            new Device(device_list_[i], buffer, future_exit_, coeff_size,
                       modulus_size, batch_size_dyadic_multiply, batch_size_ntt,
                       batch_size_intt, batch_size_KeySwitch, 1, debug);
        std::thread runner(&Device::run, devices_[i]);
        runners_.emplace_back(std::move(runner));
    }
}

DevicePool::~DevicePool() {
    for (auto& runner : runners_) {
        runner.join();
    }
    for (unsigned int i = 0; i < device_count_; i++) {
        delete devices_[i];
        devices_[i] = nullptr;
    }
    delete[] devices_;
    devices_ = nullptr;
}

}  // namespace fpga
}  // namespace hexl
}  // namespace intel
