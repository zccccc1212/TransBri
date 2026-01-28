/*
 * Copyright (c) 2001-2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */


#ifndef RING_H
#define RING_H

#include <vector>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <condition_variable>
#include <thread>
#include <pthread.h>
#include <sched.h>
#include <sys/sysinfo.h>
#include <atomic>
#include <vector>
#include <mutex>
#include <cstdint>
#include <stdexcept>



#include "vma/ib/base/verbs_extra.h"
#include "vma/proto/flow_tuple.h"
#include "vma/sock/socket_fd_api.h"

class pkt_rcvr_sink;

#define ring_logpanic 		__log_info_panic
#define ring_logerr			__log_info_err
#define ring_logwarn		__log_info_warn
#define ring_loginfo		__log_info_info
#define ring_logdbg			__log_info_dbg
#define ring_logfunc		__log_info_func
#define ring_logfuncall		__log_info_funcall
#define ring_logfine		__log_info_fine

//#define MR_SIZE 4294967296 // 4GB
#define MR_SIZE 268435456   //256MB

typedef enum {
	CQT_RX,
	CQT_TX
} cq_type_t;

typedef int ring_user_id_t;

/* Ring event completion */
struct ring_ec {
	struct list_head list;
	struct vma_completion_t completion;
	struct vma_buff_t*      last_buff_lst;

	inline void clear()
	{
		INIT_LIST_HEAD(&list);
		memset(&completion, 0, sizeof(completion));
		last_buff_lst = NULL;
	}
};

class ring
{
public:
	ring();

	virtual ~ring();

	virtual void print_val();

	virtual bool		attach_flow(flow_tuple& flow_spec_5t, pkt_rcvr_sink* sink) = 0;
	virtual bool		detach_flow(flow_tuple& flow_spec_5t, pkt_rcvr_sink* sink) = 0;

	virtual void		restart() = 0;

	// Funcs taken from qp_mgr.h
	// Get/Release memory buffer descriptor with a linked data memory buffer
	virtual mem_buf_desc_t*	mem_buf_tx_get(ring_user_id_t id, bool b_block, int n_num_mem_bufs = 1) = 0;
	virtual int		mem_buf_tx_release(mem_buf_desc_t* p_mem_buf_desc_list, bool b_accounting, bool trylock = false) = 0;
	virtual void		send_ring_buffer(ring_user_id_t id, vma_ibv_send_wr* p_send_wqe, vma_wr_tx_packet_attr attr) = 0;
	virtual void		send_lwip_buffer(ring_user_id_t id, vma_ibv_send_wr* p_send_wqe, vma_wr_tx_packet_attr attr) = 0;

	// Funcs taken from cq_mgr.h
	virtual int		get_num_resources() const = 0;
	virtual int*		get_rx_channel_fds(size_t &length) const { length = 1; return m_p_n_rx_channel_fds; };
	virtual int		get_tx_channel_fd() const { return -1; };
	virtual bool 		get_hw_dummy_send_support(ring_user_id_t id, vma_ibv_send_wr* p_send_wqe) = 0;
	virtual int		request_notification(cq_type_t cq_type, uint64_t poll_sn) = 0;
	virtual bool		reclaim_recv_buffers(descq_t *rx_reuse) = 0;
	virtual bool		reclaim_recv_buffers(mem_buf_desc_t* rx_reuse_lst) = 0;
	virtual int		drain_and_proccess() = 0;
	virtual int		wait_for_notification_and_process_element(int cq_channel_fd, uint64_t* p_cq_poll_sn, void* pv_fd_ready_array = NULL) = 0;
	virtual int		poll_and_process_element_rx(uint64_t* p_cq_poll_sn, void* pv_fd_ready_array = NULL) = 0;
	virtual void		adapt_cq_moderation() = 0;
	virtual void		mem_buf_desc_return_single_to_owner_tx(mem_buf_desc_t* p_mem_buf_desc) = 0;

	virtual void		inc_tx_retransmissions_stats(ring_user_id_t id) = 0;
	virtual bool		is_member(ring_slave* rng) = 0;
	virtual bool		is_active_member(ring_slave* rng, ring_user_id_t id) = 0;
	ring*			get_parent() { return m_parent; };
	ring_user_id_t		generate_id() { return 0; };
	virtual ring_user_id_t	generate_id(const address_t src_mac, const address_t dst_mac, uint16_t eth_proto, uint16_t encap_proto, uint32_t src_ip, uint32_t dst_ip, uint16_t src_port, uint16_t dst_port) = 0;
	virtual int		modify_ratelimit(struct vma_rate_limit_t &rate_limit) = 0;
        virtual uint32_t	get_max_inline_data() = 0;
#ifdef DEFINED_TSO
        virtual uint32_t	get_max_send_sge(void) = 0;
        virtual uint32_t	get_max_payload_sz(void) = 0;
        virtual uint16_t	get_max_header_sz(void) = 0;
	virtual uint32_t	get_tx_lkey(ring_user_id_t id) = 0;
        virtual bool		is_tso(void) = 0;
#endif /* DEFINED_TSO */
	virtual int		reg_mr(void *addr, size_t length, uint32_t &lkey) { NOT_IN_USE(addr); NOT_IN_USE(length); NOT_IN_USE(lkey); return -1;};
	virtual int		dereg_mr(void *addr, size_t length) { NOT_IN_USE(addr);NOT_IN_USE(length); return -1;};

	virtual int		socketxtreme_poll(struct vma_completion_t *vma_completions, unsigned int ncompletions, int flags) = 0;

	virtual bool is_socketxtreme(void) = 0;
	virtual void put_ec(struct ring_ec *ec) = 0;
	virtual void del_ec(struct ring_ec *ec) = 0;
	virtual struct vma_completion_t *get_comp(void) = 0;

	inline int get_if_index() { return m_if_index; }

protected:
	inline void set_parent(ring* parent) { m_parent = ( parent ? parent : this); }
	inline void set_if_index(int if_index) { m_if_index = if_index; }

	int*			m_p_n_rx_channel_fds;
	ring*			m_parent;

	int                 m_if_index;     /* Interface index */
};



//zc add
/* structure to exchange data which is needed to connect the QPs */
struct cm_con_data_t
{
    uint64_t addr;        /* Buffer address */
    uint32_t rkey;        /* Remote key */
    uint32_t remote_recv_window_rkey;  // recv window rkey
    uint32_t qp_num;      /* QP number */
    uint16_t lid;         /* LID of the IB port */
    uint8_t gid[16];      /* gid */
} __attribute__((packed));

/* structure of system resources */
struct resources
{
    struct ibv_device_attr device_attr; /* Device attributes */
    struct ibv_port_attr port_attr;     /* IB port attributes */
    struct cm_con_data_t remote_props;  /* values to connect to remote side */
    struct ibv_context *ib_ctx;         /* device handle */
    struct ibv_pd *pd;                  /* PD handle */
    struct ibv_cq *send_cq;             /* 发送CQ handle */
    struct ibv_cq *recv_cq;             /* 接收CQ handle */
    struct ibv_qp *qp;                  /* QP handle */
    struct ibv_mr *recv_mr;             /* MR handle for recv buf */
    struct ibv_mr *send_mr;             /* MR handle for send buf */
    int sock;                           /* TCP socket file descriptor */
};

// zc add
//每个socekt fd对应一个Socket_transbridge,每个Socket_transbridge对应一个sor connection，通过unordered_map找，而不是直接存在Socket_transbridge的实例内，只有当建立好连接的时候才初始化sor connection,避免资源浪费
//每个sor connection 维护两个ringbuffer，分别是recv和send，后续可以考虑融合为一个

// ringbuffer类中不需要关注是不是mr，只需要我们在初始化ringbuffer的时候通过ibv_reg_mr注册成MR即可
//  但是这个ringbuffer是封装好的，外面没办法访问里面的数据地址，只能直接操作进行拷贝数据
// 仔细考虑过后，数据发送的接口也放到ringbuffer类中，一层层调用ringbuffer来进行实际的数据发送
// 为了让我们得到cqe以后可以根据cqe中的wr_id找到对应的那一个request操作的数据的缓冲区的地址和长度，我们需要另外一个结构来存储这些信息，并在提交请求的时候将wr_id设为这个结构的地址，方便查询

//采用imm以后不需要true_data_size_，删除

//为send buf增加mid指针，更加完备

//分两个，分别是sendbuf 和 recvbuf类


class RecvBuffer {
private:
    std::vector<unsigned char> buffer_;
    size_t capacity_;
    size_t head_;  // 已读取数据的开始位置
    size_t tail_;  // 已写入数据的结束位置
    
    // 大小统计
    size_t total_size_;      // 总数据大小
    size_t available_size_;  // 可用空间大小
    
    // 同步原语
    mutable std::shared_mutex rw_mutex_;
    std::mutex m_cv_mutex;
    std::condition_variable m_cv;
    std::mutex m_recv_mutex;
    std::condition_variable m_recv_cv;
    std::atomic<uint64_t> m_data_arrival_count{0};
    std::atomic<bool> is_valid_{true};

    // 原子变量
    std::atomic<size_t> atomic_total_size_{0};
    std::atomic<size_t> atomic_available_{0};

public:
    explicit RecvBuffer(size_t capacity) 
        : capacity_(capacity), head_(0), tail_(0), 
          total_size_(0), available_size_(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("Capacity must be greater than 0");
        }
        buffer_.resize(capacity, 0);
        update_atomic_vars();
    }

    ~RecvBuffer() {
        is_valid_.store(false, std::memory_order_release);
        m_recv_cv.notify_all();
        m_cv.notify_all();
    }

    // 禁用拷贝和移动
    RecvBuffer(const RecvBuffer&) = delete;
    RecvBuffer& operator=(const RecvBuffer&) = delete;
    RecvBuffer(RecvBuffer&&) = delete;
    RecvBuffer& operator=(RecvBuffer&&) = delete;

    // 基本信息
    size_t capacity() const { return capacity_; }
    size_t total_size() const { return atomic_total_size_.load(std::memory_order_acquire); }
    size_t available() const { return atomic_available_.load(std::memory_order_acquire); }
    bool empty() const { return total_size() == 0; }
    bool full() const { return available() == 0; }
    bool is_valid() const { return is_valid_.load(std::memory_order_acquire); }

    // 读取数据但不移动指针
    size_t peek(void* data, size_t len) const {
        if (!is_valid() || data == nullptr || len == 0 || empty()) return 0;
        
        std::shared_lock lock(rw_mutex_);
        
        len = std::min(len, total_size_);
        unsigned char* dst = static_cast<unsigned char*>(data);
        
        // 计算连续可读取空间
        size_t contiguous = calc_contiguous_size(head_, tail_);
        size_t to_read = std::min(len, contiguous);
        
        // 读取第一部分
        memcpy(dst, &buffer_[head_], to_read);
        
        // 如果需要回绕读取剩余部分
        if (to_read < len) {
            memcpy(dst + to_read, &buffer_[0], len - to_read);
        }
        
        return len;
    }

    // 读取数据并移动指针
    size_t read(void* data, size_t len) {
        if (!is_valid() || data == nullptr || len == 0 || empty()) return 0;
        
        std::unique_lock lock(rw_mutex_);
        
        len = std::min(len, total_size_);
        unsigned char* dst = static_cast<unsigned char*>(data);
        
        // 计算连续可读取空间
        size_t contiguous = calc_contiguous_size(head_, tail_);
        size_t to_read = std::min(len, contiguous);
        
        // 读取第一部分
        memcpy(dst, &buffer_[head_], to_read);
        
        // 如果需要回绕读取剩余部分
        if (to_read < len) {
            memcpy(dst + to_read, &buffer_[0], len - to_read);
        }
        
        head_ = (head_ + len) % capacity_;
        total_size_ -= len;
        available_size_ += len;
        
        update_atomic_vars();
        
        return len;
    }

    // 安全的批量读取
    size_t read_safe(void* data, size_t len, int timeout_ms = 5000) {
        if (!is_valid() || data == nullptr || len == 0) return 0;
        
        if (!wait_for_data(len, timeout_ms)) {
            return 0;
        }
        
        return read(data, len);
    }

    // 等待数据到达
    bool wait_for_data(size_t required_size, int timeout_ms = 5000) {
        std::unique_lock<std::mutex> lock(m_recv_mutex);
        
        auto predicate = [this, required_size]() {
            return !is_valid() || this->total_size() >= required_size;
        };
        
        if (this->total_size() >= required_size) {
            return true;
        }
        
        uint64_t initial_count = m_data_arrival_count.load(std::memory_order_acquire);
        
        if (timeout_ms > 0) {
            auto result = m_recv_cv.wait_for(lock, 
                std::chrono::milliseconds(timeout_ms), predicate);
            
            if (!result) {
                uint64_t current_count = m_data_arrival_count.load(std::memory_order_acquire);
                if (current_count > initial_count) {
                    return this->total_size() >= required_size;
                }
                return false;
            }
            return is_valid() && this->total_size() >= required_size;
        } else {
            m_recv_cv.wait(lock, predicate);
            return is_valid() && this->total_size() >= required_size;
        }
    }

    // 更新tail指针（当外部直接写入了数据后调用）
    void updateTail(size_t bytes_added) {
        std::unique_lock lock(rw_mutex_);
        
        if (bytes_added == 0 || bytes_added > available_size_) {
            return;
        }
        
        tail_ = (tail_ + bytes_added) % capacity_;
        total_size_ += bytes_added;
        available_size_ -= bytes_added;
        
        update_atomic_vars();
        
        // 通知数据到达
        notify_data_arrival();
    }

    // 清空缓冲区
    void clear() {
        std::unique_lock lock(rw_mutex_);
        head_ = tail_ = 0;
        total_size_ = 0;
        available_size_ = capacity_;
        
        update_atomic_vars();
        m_cv.notify_all();
    }

    // 获取数据到达计数（用于调试）
    uint64_t get_data_arrival_count() const {
        return m_data_arrival_count.load(std::memory_order_acquire);
    }

    unsigned char* get_buffer_start() {
        return buffer_.data();
    }

    const unsigned char* get_buffer_start() const {
        return buffer_.data();
    }

private:
    // 计算连续空间
    size_t calc_contiguous_size(size_t from, size_t to) const {
        if(full())    return capacity_;

        if(empty()) return 0;

        if (from < to) {
            return to - from;
        } else {
            return capacity_ - from;
        }
    }

    // 更新原子变量
    void update_atomic_vars() {
        atomic_total_size_.store(total_size_, std::memory_order_release);
        atomic_available_.store(available_size_, std::memory_order_release);
    }

    // 通知数据到达
    void notify_data_arrival() {
        m_data_arrival_count.fetch_add(1, std::memory_order_release);
        m_recv_cv.notify_all();
    }
};

class SendBuffer {
private:
    std::vector<unsigned char> buffer_;
    size_t capacity_;
    
    // 指针管理
    std::atomic<size_t> head_;  // 已确认数据的开始位置（原子变量）
    size_t mid_;                // 已发送但未确认数据的开始位置
    size_t tail_;               // 未发送数据的结束位置
    
    // 延迟更新的统计变量
    size_t total_size_;      // 总数据大小
    size_t unack_size_;      // 已发送但未确认的数据大小
    size_t unsent_size_;     // 未发送的数据大小
    size_t available_size_;  // 可用空间大小
    
    // 待确认的数据量（无锁更新）
    std::atomic<size_t> pending_ack_{0};
    
    // 同步原语
    mutable std::shared_mutex rw_mutex_;
    std::mutex m_cv_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> is_valid_{true};

    // 原子统计变量（供外部查询）
    std::atomic<size_t> atomic_total_size_{0};
    std::atomic<size_t> atomic_unack_size_{0};
    std::atomic<size_t> atomic_unsent_size_{0};
    std::atomic<size_t> atomic_available_{0};

public:
    explicit SendBuffer(size_t capacity) 
        : capacity_(capacity), head_(0), mid_(0), tail_(0), 
          total_size_(0), unack_size_(0), unsent_size_(0), 
          available_size_(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("Capacity must be greater than 0");
        }
        buffer_.resize(capacity, 0);
        update_atomic_vars();
    }

    ~SendBuffer() {
        is_valid_.store(false, std::memory_order_release);
        m_cv.notify_all();
    }

    // 禁用拷贝和移动
    SendBuffer(const SendBuffer&) = delete;
    SendBuffer& operator=(const SendBuffer&) = delete;
    SendBuffer(SendBuffer&&) = delete;
    SendBuffer& operator=(SendBuffer&&) = delete;

    // 基本信息
    size_t capacity() const { return capacity_; }
    size_t total_size() const { return atomic_total_size_.load(std::memory_order_acquire); }
    size_t unack_size() const { return atomic_unack_size_.load(std::memory_order_acquire); }
    size_t unsent_size() const { return atomic_unsent_size_.load(std::memory_order_acquire); }
    size_t available() const { return atomic_available_.load(std::memory_order_acquire); }
    bool empty() const { return total_size() == 0; }
    bool full() const { return available() == 0; }
    bool is_valid() const { return is_valid_.load(std::memory_order_acquire); }

    // 应用延迟更新（在需要写锁的操作中调用）
    void apply_pending_updates() {
        size_t acked = pending_ack_.exchange(0, std::memory_order_acq_rel);
        if (acked > 0) {
            // 更新统计变量
            unack_size_ -= acked;
            total_size_ -= acked;
            available_size_ += acked;
            
            update_atomic_vars();
        }
    }

    // 写入数据到未发送区域
    size_t write(const void* data, size_t len) {
        if (!is_valid() || data == nullptr || len == 0) return 0;
        
        std::unique_lock lock(rw_mutex_);
        
        // 应用延迟更新
        apply_pending_updates();
        
        if (available_size_ == 0) return 0;
        
        len = std::min(len, available_size_);
        const unsigned char* src = static_cast<const unsigned char*>(data);
        
        // 计算连续可写入空间
        size_t contiguous = calc_contiguous_write_size();
        size_t to_write = std::min(len, contiguous);
        
        // 写入第一部分
        memcpy(&buffer_[tail_], src, to_write);
        
        // 如果需要回绕写入剩余部分
        if (to_write < len) {
            memcpy(&buffer_[0], src + to_write, len - to_write);
        }
        
        tail_ = (tail_ + len) % capacity_;
        unsent_size_ += len;
        total_size_ += len;
        available_size_ -= len;
        
        update_atomic_vars();
        
        return len;
    }

    // 安全的批量写入 - 在等待时进行延迟更新
    size_t write_safe(const void* data, size_t len, int timeout_ms = 5000) {
        if (!is_valid() || data == nullptr || len == 0) return 0;
        
        // 先检查是否有足够空间，如果没有则等待
        if (!wait_available_with_update(len, timeout_ms)) {
            return 0;
        }
        
        return write(data, len);
    }

    // 标记数据为已发送（移动mid指针）
    size_t mark_sent(size_t len) {
        if (!is_valid() || len == 0 || unsent_size_ == 0) return 0;
        
        std::unique_lock lock(rw_mutex_);
        
        // 应用延迟更新
        apply_pending_updates();
        
        len = std::min(len, unsent_size_);
        mid_ = (mid_ + len) % capacity_;
        unsent_size_ -= len;
        unack_size_ += len;
        
        update_atomic_vars();
        
        return len;
    }

    // 确认已发送数据（无锁更新，只移动head指针）
    size_t mark_ack(size_t len) {
        if (!is_valid() || len == 0) return 0;
        
        // 无锁更新head指针
        size_t current_head = head_.load(std::memory_order_acquire);
        size_t new_head = (current_head + len) % capacity_;
        
        // 使用CAS确保原子性更新
        while (!head_.compare_exchange_weak(current_head, new_head, 
                                          std::memory_order_release,
                                          std::memory_order_acquire)) {
            // 如果CAS失败，重新计算new_head
            new_head = (current_head + len) % capacity_;
        }
        
        // 记录待确认的数据量（无锁）
        pending_ack_.fetch_add(len, std::memory_order_release);
        
        // 通知等待空间的线程（无锁通知）
        m_cv.notify_all();
        
        return len;
    }

    // 获取未发送数据的区域
    const unsigned char* get_unsent_region(size_t& size) const {
        std::shared_lock lock(rw_mutex_);
        if (unsent_size_ == 0) {
            size = 0;
            return nullptr;
        }
        size_t contiguous = calc_contiguous_size(mid_, tail_);
        size = std::min(contiguous, unsent_size_);
        return &buffer_[mid_];
    }

    // 获取未确认数据的区域
    const unsigned char* get_unack_region(size_t& size) const {
        // 使用原子head_，不需要锁
        size_t current_head = head_.load(std::memory_order_acquire);
        size_t current_mid = mid_;  // mid_只在主线程更新，相对安全
        
        if (current_head == current_mid) {
            size = 0;
            return nullptr;
        }
        
        size_t contiguous = calc_contiguous_size(current_head, current_mid);
        size_t unack_size = (current_mid >= current_head) ? 
                           (current_mid - current_head) : 
                           (capacity_ - current_head + current_mid);
        size = std::min(contiguous, unack_size);
        return &buffer_[current_head];
    }

    // 获取可写区域
    unsigned char* get_writable_region(size_t& size) {
        std::shared_lock lock(rw_mutex_);
        if (full()) {
            size = 0;
            return nullptr;
        }
        size_t contiguous = calc_contiguous_write_size();
        size = std::min(contiguous, available_size_);
        return &buffer_[tail_];
    }

    // 等待可用空间并在被唤醒时进行延迟更新
    bool wait_available_with_update(size_t required_size, int timeout_ms = 5000) {
        std::unique_lock<std::mutex> lock(m_cv_mutex);
        
        auto predicate = [this, required_size]() {
            // 检查是否需要更新统计信息
            if (pending_ack_.load(std::memory_order_acquire) > 0) {
                // 在等待期间发现有pending更新，先应用更新
                std::unique_lock rw_lock(rw_mutex_);
                apply_pending_updates();
            }
            return !is_valid() || available_size_ >= required_size;
        };
        
        if (timeout_ms > 0) {
            auto result = m_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), predicate);
            if (!result || !is_valid()) {
                return false;
            }
        } else {
            m_cv.wait(lock, predicate);
            if (!is_valid()) {
                return false;
            }
        }
        
        return available_size_ >= required_size;
    }

    // 原有的wait_available_cv（兼容性）
    bool wait_available_cv(size_t required_size, int timeout_ms = 5000) {
        return wait_available_with_update(required_size, timeout_ms);
    }

    // 清空缓冲区
    void clear() {
        std::unique_lock lock(rw_mutex_);
        head_.store(0, std::memory_order_release);
        mid_ = tail_ = 0;
        total_size_ = unack_size_ = unsent_size_ = 0;
        available_size_ = capacity_;
        pending_ack_.store(0, std::memory_order_release);
        
        update_atomic_vars();
        m_cv.notify_all();
    }

    unsigned char* get_buffer_start() {
        return buffer_.data();
    }

    const unsigned char* get_buffer_start() const {
        return buffer_.data();
    }

    // 获取当前head位置（原子操作）
    size_t get_head() const {
        return head_.load(std::memory_order_acquire);
    }

    // 强制应用pending更新
    void force_update() {
        std::unique_lock lock(rw_mutex_);
        apply_pending_updates();
    }

private:
    // 计算连续空间
    size_t calc_contiguous_size(size_t from, size_t to) const {
        if (from <= to) {
            return to - from;
        } else {
            return capacity_ - from;
        }
    }

    size_t calc_contiguous_write_size() const {
        size_t current_head = head_.load(std::memory_order_acquire);
        
        if (full()) return 0;
        
        // 如果缓冲区为空，整个缓冲区都是连续可写的
        if (empty()) return capacity_;
        
        // 如果 tail 在 head 之前，连续空间到缓冲区末尾
        if (tail_ < current_head) {
            return current_head - tail_;
        }
        // 如果 tail 在 head 之后，连续空间到缓冲区末尾
        else {
            return capacity_ - tail_;
        }
    }

    // 更新原子变量
    void update_atomic_vars() {
        atomic_total_size_.store(total_size_, std::memory_order_release);
        atomic_unack_size_.store(unack_size_, std::memory_order_release);
        atomic_unsent_size_.store(unsent_size_, std::memory_order_release);
        atomic_available_.store(available_size_, std::memory_order_release);
    }
};


class CQEPoller;



class SoR_connection{
private:
	int m_fd;	

    uint64_t cur_send_wr_id;
    uint64_t cur_recv_wr_id;

    uint64_t remote_recv_addr_start;        // remote Buffer address start 
	uint64_t total_send;                    // total send data
    uint64_t next_remote_to_write;          // next time post send to write remote
    
    size_t remote_recv_buffer;              // will be registered as MR and will update by remote and this side. TODO: may has some trouble but don't care now 
    struct ibv_mr *remote_recv_window_mr;

    size_t m_recv_buf;                      // will be regis as MR too
    struct ibv_mr *my_recv_window_mr;
    size_t m_recv_buf_remote;      //这个代表对面存储的接收方的接收缓冲区的大小，每次收到数据这个会减小，但是单纯调用recv并不更新这个变量，只有当这个变量低到一个临界点（比如30%），说明在发送方看来接收缓冲区可能快用完了
                                    //这时，将这个值更新位真正的接收窗口的大小，并通知发送方更新接收窗口

	int m_qpn;
	int m_recv_cqn;
	int m_send_cqn;

	long int send_buffer_total;
	long int recv_buffer_total;
	long int send_buffer_current;
	long int recv_buffer_current;
	long int cur_recv_data;


	union ibv_gid my_gid;
	int m_gidindex;
    CQEPoller* m_cqe_poller;

    mutable std::mutex m_recv_mutex;
    std::condition_variable m_recv_cv;
    bool m_data_ready = false;  // 条件标志

    struct ibv_device_attr m_device_attr;

    // 添加纯原子变量计数RDMA发送请求
    std::atomic<uint32_t> m_rdma_send_request_count{0};
    uint32_t m_max_rdma_send_requests{8000};  // 最大发送请求数

public:
    struct resources m_res;

    SendBuffer * m_send_rb;
	RecvBuffer * m_recv_rb;

	SoR_connection(int fd);
	~SoR_connection();	

    void resources_init();
    int find_gid();

	int connect_to_peer();
	int sock_sync_data(int xfer_size, char *local_data, char *remote_data);
	int create_rdma_resources();
	int modify_qp();
	int modify_qp_to_init();
	int modify_qp_to_rtr();
	int modify_qp_to_rts();
	int create_ringbuffer(size_t capacity);
	
    size_t get_send_buf(){ return m_send_rb->available(); }
    size_t write_data_in_send_buf(__const void *__buf, size_t __nbytes);// 将数据写入send buf

    bool wait_send_buf(size_t __nbytes){return m_send_rb->wait_available_cv(__nbytes, 5000);}

    size_t get_remote_recv_buf(){return this->remote_recv_buffer;}
    bool wait_remote_recv_buf();
    void update_my_remote_recv_window_notify();

    size_t sync_remote_recv_window();

    int post_receive_for_recv_window();

    int post_send_data_with_imm();
    int post_send_notify_with_imm();

    size_t poll_send_completion();  // 专门轮询发送完成
    size_t poll_recv_completion();  // 专门轮询接收完成

    int update_my_recv_window_add(uint32_t bytes_transed) {
        std::lock_guard<std::mutex> lock(m_recv_mutex);
        
        m_recv_buf += bytes_transed;
        return static_cast<int>(m_recv_buf);
    }

    int update_my_recv_window_reduce(uint32_t bytes_transed) {
        std::lock_guard<std::mutex> lock(m_recv_mutex);
        
        m_recv_buf -= bytes_transed;
        m_recv_buf_remote -= bytes_transed;
        return static_cast<int>(m_recv_buf);
    }

    // 线程安全的查询
    uint32_t get_recv_window() const {
        std::lock_guard<std::mutex> lock(m_recv_mutex);
        return m_recv_buf_remote;
    }

    // 发送队列限制相关函数
    // 尝试获取发送槽位（非阻塞）
    bool try_acquire_send_slot() {
        uint32_t current = m_rdma_send_request_count.load(std::memory_order_relaxed);
        
        if (current >= m_max_rdma_send_requests) {
            return false;
        }
        
        // 使用CAS操作确保原子性增加
        return m_rdma_send_request_count.compare_exchange_weak(
            current, current + 1, std::memory_order_acquire, std::memory_order_relaxed);
    }

    // 获取发送槽位（带自旋等待）
    void acquire_send_slot() {        
        while (true) {
            uint32_t current = m_rdma_send_request_count.load(std::memory_order_relaxed);
            
            if (current < m_max_rdma_send_requests) {
                if (m_rdma_send_request_count.compare_exchange_weak(
                    current, current + 1, std::memory_order_acquire, std::memory_order_relaxed)) {
                    break; // 成功获取槽位
                }
                // CAS失败，继续重试
            } else {
                // 达到上限，短暂休眠后重试
                std::this_thread::yield();
            }
        }
    }

    // 批量释放发送槽位
    void release_send_slots(uint32_t count) {
        m_rdma_send_request_count.fetch_sub(count, std::memory_order_release);
    }

    // 获取当前发送请求计数
    uint32_t get_send_request_count() const {
        return m_rdma_send_request_count.load(std::memory_order_relaxed);
    }

    // 获取最大发送请求数
    uint32_t get_max_send_requests() const {
        return m_max_rdma_send_requests;
    }

    // 新增方法：检查是否有可用槽位
    bool has_available_send_slot() const {
        return m_rdma_send_request_count.load(std::memory_order_relaxed) < 
               m_max_rdma_send_requests;
    }


    // 启动轮询线程
    void start_cqe_poller();

    // 停止轮询线程
    void stop_cqe_poller() ;

    // 发送数据时通知轮询线程
    int post_send_notify(__const void* data, size_t size);

    void print_stats_for_debug(){
        printf("cur_send_wr_id : %ld, total_send : %ld\n", cur_send_wr_id, total_send);
    }

    // 获取统计信息
    //void get_poller_stats(uint64_t& send_completions, uint64_t& recv_completions,
    //                     uint64_t& send_errors, uint64_t& recv_errors);

};

class SoRconn_collection{
public:
    SoRconn_collection();
    ~SoRconn_collection();

    // 添加socket文件描述符对应的sor conn 到集合
    int add_sorconn(int fd);
    
    // 根据fd查找对应的sor conn
    SoR_connection* find_sorconn(int fd);
    
    // 从集合中移除并关闭对应的sor conn
    int  remove_sorconn(int fd);
    
    // 获取当前这个总共建立了多少条sor conn
    size_t size() const;
    
    // 清空集合并关闭所有sor conn
    void clear();

private:
    // 使用map存储fd到Socket_transbridge指针的映射
    std::unordered_map<int, SoR_connection*> m_conn_map;

};



// 最简单的RDMA操作数据结构
struct rdma_op_data {
    void*   data_addr;
    size_t  data_size;
    int     op_type;  // 0=发送, 1=接收
};

// 最简单的内存池
class simple_rdma_pool {
private:
    std::vector<rdma_op_data*> free_list_;
    std::vector<rdma_op_data*> all_objects_;
    std::mutex mtx_;
    
public:
    simple_rdma_pool(size_t initial_size = 1000) {
        // 预分配对象
        for (size_t i = 0; i < initial_size; ++i) {
            rdma_op_data* obj = new rdma_op_data();
            free_list_.push_back(obj);
            all_objects_.push_back(obj);
        }
    }
    
    ~simple_rdma_pool() {
        // 清理所有对象
        for (auto obj : all_objects_) {
            delete obj;
        }
        free_list_.clear();
        all_objects_.clear();
    }
    
    // 获取一个对象
    rdma_op_data* allocate(void* addr = nullptr, size_t size = 0, int type = 0) {
        std::lock_guard<std::mutex> lock(mtx_);
        
        if (free_list_.empty()) {
            // 池为空时动态扩展
            rdma_op_data* obj = new rdma_op_data();
            all_objects_.push_back(obj);
            obj->data_addr = addr;
            obj->data_size = size;
            obj->op_type = type;
            return obj;
        }
        
        rdma_op_data* obj = free_list_.back();
        free_list_.pop_back();
        obj->data_addr = addr;
        obj->data_size = size;
        obj->op_type = type;
        return obj;
    }
    
    // 释放对象（返回池中）
    void deallocate(rdma_op_data* obj) {
        if (!obj) return;
        
        std::lock_guard<std::mutex> lock(mtx_);
        obj->data_addr = nullptr;
        obj->data_size = 0;
        free_list_.push_back(obj);
    }
};

// 全局内存池实例
extern simple_rdma_pool* g_rdma_pool;


class ThreadAffinity {
public:
    // 设置当前线程的CPU亲和性
    static bool set_current_thread_affinity(int cpu_core) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_core, &cpuset);
        
        pthread_t current_thread = pthread_self();
        int result = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
        
        if (result != 0) {
            fprintf(stderr, "Failed to set thread affinity to core %d: %s\n", 
                    cpu_core, strerror(result));
            return false;
        }
        
        // 验证设置是否成功
        CPU_ZERO(&cpuset);
        result = pthread_getaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
        if (result == 0 && CPU_ISSET(cpu_core, &cpuset)) {
            printf("Thread successfully bound to CPU core %d\n", cpu_core);
            return true;
        } else {
            fprintf(stderr, "Failed to verify thread affinity setting\n");
            return false;
        }
    }
    
    // 设置指定线程的CPU亲和性
    static bool set_thread_affinity(std::thread& thread, int cpu_core) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_core, &cpuset);
        
        pthread_t native_handle = thread.native_handle();
        int result = pthread_setaffinity_np(native_handle, sizeof(cpu_set_t), &cpuset);
        
        if (result != 0) {
            fprintf(stderr, "Failed to set thread affinity to core %d: %s\n", 
                    cpu_core, strerror(result));
            return false;
        }
        return true;
    }
    
    // 获取系统CPU核心数量
    static int get_cpu_count() {
        return get_nprocs();
    }
    
    // 检查CPU核心是否有效
    static bool is_valid_core(int cpu_core) {
        int cpu_count = get_cpu_count();
        return (cpu_core >= 0 && cpu_core < cpu_count);
    }
    
    // 设置线程调度策略（实时优先级）
    static bool set_thread_scheduling(int policy = SCHED_FIFO, int priority = 80) {
        struct sched_param param;
        param.sched_priority = priority;
        
        int result = pthread_setschedparam(pthread_self(), policy, &param);
        if (result != 0) {
            fprintf(stderr, "Failed to set thread scheduling: %s\n", strerror(result));
            return false;
        }
        
        printf("Thread scheduling set to policy=%d, priority=%d\n", policy, priority);
        return true;
    }
    
    // 设置线程名称（便于调试）
    static bool set_thread_name(const char* name) {
        int result = pthread_setname_np(pthread_self(), name);
        if (result != 0) {
            fprintf(stderr, "Failed to set thread name: %s\n", strerror(result));
            return false;
        }
        return true;
    }
};

extern std::mutex g_cq_poll_mutex;

//cq tcp poller 按理来说应该可以考虑和udp结合到一起
class CQEPoller {
private:
    SoR_connection* m_connection;
    std::thread m_poll_thread;
    std::atomic<bool> m_stop_flag;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_pending_send_work;
    bool m_pending_recv_work;
    
    // 线程属性
    int m_cpu_core;
    bool m_realtime_scheduling;
    std::string m_thread_name;
    
    // 统计信息
    std::atomic<uint64_t> m_send_completions;
    std::atomic<uint64_t> m_recv_completions;
    std::atomic<uint64_t> m_send_errors;
    std::atomic<uint64_t> m_recv_errors;
    std::atomic<uint64_t> m_poll_cycles;

public:
    struct ThreadConfig {
        int cpu_core;                    // -1表示不绑定核心
        bool realtime_scheduling;        // 是否启用实时调度
        std::string thread_name;         // 线程名称
        int scheduling_priority;         // 调度优先级
        
        // 构造函数设置默认值
        ThreadConfig() 
            : cpu_core(-1)
            , realtime_scheduling(false)
            , thread_name("CQE-Poller")
            , scheduling_priority(80)
        {}
    };

    CQEPoller(SoR_connection* conn, const ThreadConfig& config = ThreadConfig()) 
        : m_connection(conn)
        , m_stop_flag(false)
        , m_pending_send_work(false)
        , m_pending_recv_work(false)
        , m_cpu_core(config.cpu_core)
        , m_realtime_scheduling(config.realtime_scheduling)
        , m_thread_name(config.thread_name)
        , m_send_completions(0)
        , m_recv_completions(0)
        , m_send_errors(0)
        , m_recv_errors(0)
        , m_poll_cycles(0) {
        
        // 验证CPU核心设置
        if (m_cpu_core != -1 && !ThreadAffinity::is_valid_core(m_cpu_core)) {
            fprintf(stderr, "Warning: Invalid CPU core %d, system has %d cores\n",
                   m_cpu_core, ThreadAffinity::get_cpu_count());
            m_cpu_core = -1; // 禁用无效的核心绑定
        }
    }
    
    ~CQEPoller() {
        stop();
    }
    
    void start() {
        m_stop_flag = false;
        m_poll_thread = std::thread(&CQEPoller::poll_loop, this);
        
        // 设置线程属性
        setup_thread_properties();
        
        std::cout << "CQE Poller thread started" << std::endl;
    }
    
    void stop() {
        if (!m_stop_flag) {
            m_stop_flag = true;
            m_cv.notify_all();
            
            if (m_poll_thread.joinable()) {
                m_poll_thread.join();
            }
            std::cout << "CQE Poller thread stopped" << std::endl;
        }
    }
    
    // 获取当前配置
    ThreadConfig get_config() const {
        ThreadConfig config;
        config.cpu_core = m_cpu_core;
        config.realtime_scheduling = m_realtime_scheduling;
        config.thread_name = m_thread_name;
        return config;
    }
    
    // 动态修改CPU亲和性（线程运行时）
    bool set_cpu_affinity(int cpu_core) {
        if (!ThreadAffinity::is_valid_core(cpu_core)) {
            fprintf(stderr, "Invalid CPU core: %d\n", cpu_core);
            return false;
        }
        
        if (ThreadAffinity::set_thread_affinity(m_poll_thread, cpu_core)) {
            m_cpu_core = cpu_core;
            return true;
        }
        return false;
    }
    
    // 通知有新的发送工作
    void notify_send_work() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pending_send_work = true;
        }
        m_cv.notify_one();
    }
    
    // 通知有新的接收工作  
    void notify_recv_work() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pending_recv_work = true;
        }
        m_cv.notify_one();
    }
    
    // 获取统计信息
    void get_stats(uint64_t& send_completions, uint64_t& recv_completions, 
                   uint64_t& send_errors, uint64_t& recv_errors, uint64_t& poll_cycles) {
        send_completions = m_send_completions;
        recv_completions = m_recv_completions;
        send_errors = m_send_errors;
        recv_errors = m_recv_errors;
        poll_cycles = m_poll_cycles;
    }
    
    void reset_stats() {
        m_send_completions = 0;
        m_recv_completions = 0;
        m_send_errors = 0;
        m_recv_errors = 0;
        m_poll_cycles = 0;
    }

private:
    void setup_thread_properties() {
        // 设置线程名称
        if (!m_thread_name.empty()) {
            ThreadAffinity::set_thread_name(m_thread_name.c_str());
        }
        
        // 设置CPU亲和性
        if (m_cpu_core != -1) {
            ThreadAffinity::set_thread_affinity(m_poll_thread, m_cpu_core);
        }
        
        // 设置实时调度（需要root权限）
        if (m_realtime_scheduling) {
            ThreadAffinity::set_thread_scheduling(SCHED_FIFO, 80);
        }
    }
    
    void poll_loop() {
        // 在线程开始运行时设置属性（确保在正确的线程上下文中）
        setup_thread_properties();
        
        const int POLL_BATCH_SIZE = 3200;
//        const int IDLE_SLEEP_US = 1000;
//        const int BUSY_POLL_THRESHOLD = 1000; // 忙碌轮询阈值
        
        struct ibv_wc wc_array[POLL_BATCH_SIZE];
//        int consecutive_idle_cycles = 0;
        
        while (!m_stop_flag) {
            m_poll_cycles++;
            
//            bool had_work = false;
            
            // 批量轮询发送CQ
            int send_polled = batch_poll_send_cq(wc_array, POLL_BATCH_SIZE);
            if (send_polled > 0) {
//                had_work = true;
//                consecutive_idle_cycles = 0;
                process_send_completions(wc_array, send_polled);
            }
            
            // 批量轮询接收CQ
/*            int recv_polled = batch_poll_recv_cq(wc_array, POLL_BATCH_SIZE);
            if (recv_polled > 0) {
                //had_work = true;
                //consecutive_idle_cycles = 0;
                process_recv_completions(wc_array, recv_polled);
            }
*/
//要不考虑不准睡眠呢，会是什么反应，重新测试一下到时候
//TODO
/*            // 自适应睡眠策略
            if (!had_work) {
                consecutive_idle_cycles++;
                
                // 根据空闲程度调整睡眠时间
                int sleep_time = IDLE_SLEEP_US;
                if (consecutive_idle_cycles > BUSY_POLL_THRESHOLD) {
                    // 长时间空闲，增加睡眠时间
                    sleep_time = IDLE_SLEEP_US * 10;
                }
                
                std::unique_lock<std::mutex> lock(m_mutex);
                if (!m_pending_send_work && !m_pending_recv_work && !m_stop_flag) {
                    m_cv.wait_for(lock, std::chrono::microseconds(sleep_time));
                }
                
                // 重置工作标志
                m_pending_send_work = false;
                m_pending_recv_work = false;
            } else {
                // 有工作的时候减少睡眠，提高响应性
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
*/
            //std::this_thread::sleep_for(std::chrono::microseconds(1));
            std::this_thread::yield();
        }
        
        // 线程退出前处理所有剩余的CQE
        drain_completion_queues();
        
        printf("CQE Poller thread exiting. Total poll cycles: %lu\n", 
               m_poll_cycles.load());
    }
    
    // 原有的处理函数保持不变...
    int batch_poll_send_cq(struct ibv_wc* wc_array, int max_count) {
        return ibv_poll_cq(m_connection->m_res.send_cq, max_count, wc_array);
    }
    
    int batch_poll_recv_cq(struct ibv_wc* wc_array, int max_count) {
        std::unique_lock<std::mutex> lock(g_cq_poll_mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            return 0; // 锁被占用，跳过本次轮询
        }
        return ibv_poll_cq(m_connection->m_res.recv_cq, max_count, wc_array);
    }
    
    void process_send_completions(struct ibv_wc* wc_array, int count) {
        m_connection->release_send_slots(count);
        for (int i = 0; i < count; i++) {
            if (wc_array[i].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "Failed status %s (%d)\n", ibv_wc_status_str(wc_array[i].status), // 这个函数可以直接将状态码转为字符串
                wc_array[i].status);
                debug_print_send_completions();
                m_connection->print_stats_for_debug();
                m_send_errors++;
                std::terminate();
                //continue;
            }

            if(wc_array[i].opcode == IBV_WC_SEND){
                //TODO 好像没什么要做的
                printf("this send is to notify remote to update recv window\n");

            }
            else if(wc_array[i].opcode == IBV_WC_RDMA_WRITE){
                rdma_op_data* tracker = (rdma_op_data*)wc_array[i].wr_id;
                if (!tracker) {
                    fprintf(stderr, "Invalid tracker in send completion\n");
                    continue;
                }
                size_t data_size = tracker->data_size;
                // 清理资源
                if (g_rdma_pool) {
                    g_rdma_pool->deallocate(tracker);
                }
                // 更新发送窗口
                if (m_connection->m_send_rb) {
                    m_connection->m_send_rb->mark_ack(data_size);
                }
            }
            else{
                fprintf(stderr, "we don't expect to get a wc not send and write with imm \n");
            }
            
            m_send_completions++;
        }
    }
    // recv 是用来更新作为发送方remote的接收窗口的，因为发送方会维护一个对端的接收窗口
    // recv with imm 是用来作为接收方接收数据的，更新作为接收方的接收窗口
    void process_recv_completions(struct ibv_wc* wc_array, int count) {
        for (int i = 0; i < count; i++) {
            if (wc_array[i].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "Recv completion error: 0x%x\n", wc_array[i].status);
                m_recv_errors++;
                continue;
            }
            if(wc_array[i].opcode == IBV_WC_RECV_RDMA_WITH_IMM){
                uint32_t imm_data = wc_array[i].imm_data;
                uint32_t this_seg_len = ntohl(imm_data);
                if (m_connection->m_send_rb) {m_connection->m_recv_rb->updateTail(this_seg_len);}
                m_connection->update_my_recv_window_reduce(this_seg_len);
            }
            else if (wc_array[i].opcode == IBV_WC_RECV)
            {
                m_connection->update_my_remote_recv_window_notify();
                printf("this send is to notify remote to update recv window\n");
            }
            else{
                fprintf(stderr, "we don't expect to get a wc not recv and recv with imm \n");
            }
        }
    }
    
    void drain_completion_queues() {
        struct ibv_wc wc;
        int poll_result;
        
        // 排空发送CQ
        while ((poll_result = ibv_poll_cq(m_connection->m_res.send_cq, 1, &wc)) > 0) {
            process_send_completions(&wc, 1);
        }
        
        // 排空接收CQ
        while ((poll_result = ibv_poll_cq(m_connection->m_res.recv_cq, 1, &wc)) > 0) {
            process_recv_completions(&wc, 1);
        }
    }

    void debug_print_send_completions() const {
        uint64_t send_completions = m_send_completions.load();
        uint64_t send_errors = m_send_errors.load();
        uint64_t poll_cycles = m_poll_cycles.load();
        
        printf("[CQEPoller DEBUG] Send Completions: %lu, Send Errors: %lu, Poll Cycles: %lu\n",
               send_completions, send_errors, poll_cycles);
    }
};

//udp ring relevent



class SequentialUdpBuffer {
public:
    // 构造函数
    SequentialUdpBuffer(uint32_t local_ip, uint16_t local_port, 
                       size_t block_count = 1000);
    ~SequentialUdpBuffer() = default;
    
    // 禁用拷贝和移动
    SequentialUdpBuffer(const SequentialUdpBuffer&) = delete;
    SequentialUdpBuffer& operator=(const SequentialUdpBuffer&) = delete;
    SequentialUdpBuffer(SequentialUdpBuffer&&) = delete;
    SequentialUdpBuffer& operator=(SequentialUdpBuffer&&) = delete;
    
    // ============ 缓冲区信息 ============
    size_t block_count() const noexcept;
    size_t data_capacity() const noexcept;        // 每块数据容量
    size_t total_block_size() const noexcept;     // 每块总大小
    size_t header_size() const noexcept;          // 头部大小
    
    // ============ 缓冲区状态 ============
    size_t available_blocks() const noexcept;     // 可用块数
    size_t used_blocks() const noexcept;          // 已使用块数
    bool has_data() const noexcept;               // 是否有数据
    bool is_full() const noexcept;                // 是否已满
    bool is_empty() const noexcept;               // 是否为空
    
    // ============ 本地地址信息 ============
    uint32_t local_ip() const noexcept;
    uint16_t local_port() const noexcept;
    void update_local_address(uint32_t new_ip, uint16_t new_port) {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        
        // 更新地址
        local_ip_ = new_ip;
        local_port_ = new_port;
    
    }
    
    // ============ 发送缓冲区接口 ============
    // 写入数据，头部自动使用绑定的本地IP和端口
    bool write_block(const void* data, size_t len);
    
    // 标记块为已发送完成（更新head指针）
    bool mark_block_sent_completed();
    
    // ============ 接收缓冲区接口 ============
    // 读取数据，返回发送方的IP和端口
    ssize_t read_block(void *buf, size_t nbytes,
                      struct sockaddr *srcAddr, socklen_t *addrlen);
    
    // 标记块为已接收完成（更新tail指针）
    bool mark_block_received_completed();
    
    // ============ 直接内存访问（用于RDMA） ============
    // 获取下一个接收位置（接收缓冲区使用）
    unsigned char* get_next_receive_ptr();
    
    // 获取下一个发送位置（发送缓冲区使用）
    unsigned char* get_next_send_ptr();
    
    // 获取头指针位置（用于读取）
    unsigned char* get_head_ptr();
    
    // 获取尾指针位置（用于写入）
    unsigned char* get_tail_ptr();
    
    // ============ RDMA内存注册接口 ============
    // 获取整个缓冲区的起始地址（用于RDMA注册MR）
    unsigned char* buffer_data() noexcept;
    const unsigned char* buffer_data() const noexcept;
    
    // 获取整个缓冲区的大小（字节数）
    size_t buffer_size() const noexcept;
    
    // ============ 指针位置信息 ============
    size_t get_head_index() const noexcept;   // 获取head指针位置
    size_t get_tail_index() const noexcept;   // 获取tail指针位置
    
    // ============ 缓冲区管理 ============
    void clear();

private:
    // ============ 常量 ============
    static constexpr size_t DATA_SIZE = 1472;     // 每个块的数据部分大小
    static constexpr size_t HEADER_SIZE = 8;      // 头部大小
    static constexpr size_t BLOCK_TOTAL_SIZE = DATA_SIZE + HEADER_SIZE; // 1480字节
    
    // ============ 块头部结构 ============
    #pragma pack(push, 1)
    struct BlockHeader {
        uint32_t source_ip;      // 源IP地址（4字节）
        uint16_t source_port;    // 源端口号（2字节）
        uint16_t data_length;    // 数据长度（2字节），0-1472
    };
    #pragma pack(pop)
    
    // ============ 成员变量 ============
    std::vector<unsigned char> buffer_;       // 数据缓冲区
    std::vector<bool> block_valid_;           // 块有效标志数组
    size_t block_count_;                      // 总块数
    
    // 本地地址信息
    uint32_t local_ip_;                       // 本地IP地址
    uint16_t local_port_;                     // 本地端口号
    
    // 缓冲区指针（环形缓冲区）
    std::atomic<size_t> head_;                // head指针：第一个有效数据块
    std::atomic<size_t> tail_;                // tail指针：最后一个有效数据块的下一个位置
    std::atomic<size_t> used_blocks_;         // 已使用块数
    // 接收专用指针（独立于head和tail）
    std::atomic<size_t> next_receive_index_;  // 新增
    
    // 同步原语
    mutable std::mutex buffer_mutex_;         // 缓冲区操作互斥锁
};


class QpnToFdMap {
public:
    // 获取实例（全局单例）
    static QpnToFdMap& get() {
        static QpnToFdMap instance;
        return instance;
    }
    
    // 添加映射：QPN -> FD
    void add(uint32_t qpn, int fd) {
        std::lock_guard<std::mutex> lock(mutex_);
        map_[qpn] = fd;
    }
    
    // 删除映射
    void remove(uint32_t qpn) {
        std::lock_guard<std::mutex> lock(mutex_);
        map_.erase(qpn);
    }
    
    // 查找FD
    int getFd(uint32_t qpn) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(qpn);
        return it != map_.end() ? it->second : -1;
    }
    
    // 清空所有
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        map_.clear();
    }

private:
    QpnToFdMap() = default;
    
    std::unordered_map<uint32_t, int> map_;
    std::mutex mutex_;
};


// CQ管理器 - 单例模式，管理共享的完成队列
class CQManager {
public:
    // 获取单例实例
    static CQManager& getInstance();
    
    // 禁止拷贝和赋值
    CQManager(const CQManager&) = delete;
    CQManager& operator=(const CQManager&) = delete;
    
    // 创建共享的CQ
    bool createSharedCQs(ibv_context* context, 
                        uint32_t send_cq_size = 1024,
                        uint32_t recv_cq_size = 1024,
                        void* send_cq_context = nullptr,
                        void* recv_cq_context = nullptr);
    
    // 获取共享CQ
    ibv_cq* getSharedSendCq() const { return m_shared_send_cq; }
    ibv_cq* getSharedRecvCq() const { return m_shared_recv_cq; }
    
    // 启动/停止轮询线程
    void startPollingThreads();
    void stopPollingThreads();
    
    // 轮询接口
    int pollSendCq(int timeout_ms = 0);
    int pollRecvCq(int timeout_ms = 0);
    
    // 清理资源
    void cleanup();
    
    // 统计信息结构体
    struct CQStats {
        uint64_t send_completions;
        uint64_t recv_completions;
        uint64_t send_errors;
        uint64_t recv_errors;
        size_t registered_qps;
    };
    
    // 获取和打印统计信息
    CQStats getStats() const;
    void printStats() const;
    
private:
    // 私有构造函数和析构函数
    CQManager();
    ~CQManager();
    
    // 轮询线程函数
    void sendPollingThread();
    void recvPollingThread();
    
    // 处理完成事件
    void handleSendCompletion(ibv_wc& wc);
    void handleRecvCompletion(ibv_wc& wc);
    
private:
    // 共享的CQ
    ibv_cq* m_shared_send_cq;
    ibv_cq* m_shared_recv_cq;
    
    // 轮询线程
    std::thread m_send_polling_thread;
    std::thread m_recv_polling_thread;
    std::atomic<bool> m_polling_stop;
    
    // 统计信息
    std::atomic<uint64_t> m_send_completions;
    std::atomic<uint64_t> m_recv_completions;
    std::atomic<uint64_t> m_send_errors;
    std::atomic<uint64_t> m_recv_errors;
    
    // 同步原语
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_cv;
};

//ud rdma qp
// 简化的RDMA资源结构
struct SimpleRdmaResources {
    ibv_context* context = nullptr;      // 设备上下文
    ibv_pd* pd = nullptr;                // 保护域
    ibv_qp* qp = nullptr;                // 队列对（UD类型）
    
    // 内存区域
    struct {
        ibv_mr* send_mr = nullptr;       // 发送内存区域
        ibv_mr* recv_mr = nullptr;       // 接收内存区域
        void* send_buf = nullptr;        // 发送缓冲区
        void* recv_buf = nullptr;        // 接收缓冲区
        size_t send_size = 0;            // 发送缓冲区大小
        size_t recv_size = 0;            // 接收缓冲区大小
    } memory;
    
    // 设备信息
    struct {
        std::string name;
        uint8_t port_num = 1;
        uint16_t lid = 0;
    } device;
    
    // UD特定参数
    uint32_t qkey = 0x111111;
    
    // 共享CQ引用
    ibv_cq* shared_send_cq = nullptr;
    ibv_cq* shared_recv_cq = nullptr;
};



// 简化的RDMA管理器 - 单例模式，只管理一个UD QP


// 一个进程一个qp，然后两个节点通信的时候，发现对面的ip地址有就可以直接发送到那个qp即可
// 然后每个udp socket维护自己的ringbuffer
// 同样有一个额外的轮询线程，这个轮询线程会负责一直轮询cq
// 在发送数据包的时候会同时发送一个包头过去，这个包头在最前面，告诉对面这个包的源IP，目标IP，源port，目标port
// 这一个数据包的长度用立即数来传
// 所以在创建socket时就需要为每个socket根据ip地址和端口建立一个映射，方便后面查找
// 当轮询线程轮循到某一个数据包时，就可以根据.

// 不对，压根就不可行，轮询线程轮循到数据包时，只能通知那一个socket，但是他调用recvfrom时，都不知道数据从哪里取


// 还是不可行，必须每个udp socket一个qp，自己管自己的
class UDRdmaManager {
public:
    // 构造函数
    UDRdmaManager(uint32_t local_ip, uint16_t local_port);
    
    // 析构函数
    ~UDRdmaManager();
    
    // 禁止拷贝构造和拷贝赋值
    UDRdmaManager(const UDRdmaManager&) = delete;
    UDRdmaManager& operator=(const UDRdmaManager&) = delete;
    
    // 允许移动构造和移动赋值
    UDRdmaManager(UDRdmaManager&& other) noexcept;
    UDRdmaManager& operator=(UDRdmaManager&& other) noexcept;
    
    // 初始化RDMA资源
    bool initialize(uint32_t local_ip, uint16_t local_port, int sockfd,
                    size_t send_buffer_size = 1024,
                    size_t recv_buffer_size = 1024,
                    uint32_t max_send_wr = 1024,
                    uint32_t max_recv_wr = 1024,
                    uint32_t max_send_sge = 16,
                    uint32_t max_recv_sge = 16);
    
    // 清理所有资源
    void cleanup();
    
    // 检查是否已初始化
    bool isInitialized() const { return m_initialized; }
    
    // ============ QP和资源获取 ============
    ibv_qp* getQp() const { return m_resources.qp; }
    ibv_pd* getPd() const { return m_resources.pd; }
    ibv_context* getContext() const { return m_resources.context; }
    
    // 获取QP信息
    uint32_t getQpNum() const { return m_resources.qp ? m_resources.qp->qp_num : 0; }
    uint16_t getLid() const { return m_resources.device.lid; }
    uint8_t getPortNum() const { return m_resources.device.port_num; }
    uint32_t getQkey() const { return m_resources.qkey; }
    
    // ============ 缓冲区信息 ============

    uint32_t getSendBufferLkey() const { 
        return m_resources.memory.send_mr ? m_resources.memory.send_mr->lkey : 0; 
    }
    uint32_t getRecvBufferLkey() const { 
        return m_resources.memory.recv_mr ? m_resources.memory.recv_mr->lkey : 0; 
    }
    
    // 分配和注册内存区域
    bool allocateAndRegisterMemory(uint32_t local_ip, uint16_t local_port, size_t send_block_count, size_t recv_block_count);
    
    // ============ SequentialUdpBuffer 管理 ============
    // 获取发送缓冲区引用
    SequentialUdpBuffer& send_buffer() { return *send_buffer_; }
    const SequentialUdpBuffer& send_buffer() const { return *send_buffer_; }
    
    // 获取接收缓冲区引用
    SequentialUdpBuffer& recv_buffer() { return *recv_buffer_; }
    const SequentialUdpBuffer& recv_buffer() const { return *recv_buffer_; }
    
    // 获取MR信息
    struct MemoryRegions {
        ibv_mr* send_mr;
        ibv_mr* recv_mr;
        size_t send_size;
        size_t recv_size;
    };
    
    MemoryRegions get_memory_regions() const;
    
    // ============ 本地地址管理 ============
    bool updateLocalAddress(uint32_t new_local_ip, uint16_t new_local_port);
    
    // ============ 接收缓冲区相关方法 ============
    size_t post_recv(size_t n);
    bool post_single_recv_internal();
    
    // ============ 共享CQ管理 ============
    // 静态方法获取共享CQ
    static ibv_cq* getSharedSendCq();
    static ibv_cq* getSharedRecvCq();
    
    // 静态方法启动/停止轮询线程
    static bool startPollingThreads();
    static void stopPollingThreads();
    
    // 静态方法获取统计信息
    static void printCqStats();
    
    // ============ 错误信息 ============
    const char* getLastError() const { return m_errorMsg; }
    
    // ============ 调试信息 ============
    void printInfo() const;
    
    // ============ 资源统计 ============
    static int getInstanceCount() { return instance_count_.load(); }

    // 获取GID
    void getGid(uint8_t gid[16]) const {
        if (m_gid_initialized) {
            memcpy(gid, m_gid, 16);
        } else {
            memset(gid, 0, 16);
        }
    }

    
private:
    // WR ID生成器（静态原子变量，全局递增）
    static std::atomic<uint64_t> wr_id_counter_;

    // ============ 内部初始化方法 ============
    bool discoverAndOpenDevice();
    bool createProtectionDomain();
    bool createCompletionQueues();  // 现在使用共享CQ
    bool createUdQueuePair(uint32_t max_send_wr, uint32_t max_recv_wr,
                          uint32_t max_send_sge, uint32_t max_recv_sge,int socket_fd);
    bool initUdQp();
    
    
    // ============ 错误处理 ============
    void setLastError(const std::string& error);
    void setLastError(int errnum);

    
    // ============ 成员变量 ============
    SimpleRdmaResources m_resources;
    bool m_initialized = false;
    char m_errorMsg[256] = {0};
    std::mutex m_mutex;

    // 本地地址信息
    uint32_t local_ip_;
    uint16_t local_port_;
    
    // 缓冲区
    std::unique_ptr<SequentialUdpBuffer> send_buffer_;
    std::unique_ptr<SequentialUdpBuffer> recv_buffer_;
    
    // RDMA内存区域
    ibv_mr* send_mr_ = nullptr;
    ibv_mr* recv_mr_ = nullptr;
    
    // 已创建的地址句柄
    std::vector<ibv_ah*> m_created_ahs;
    
    // 静态变量，跟踪实例数量
    static std::atomic<int> instance_count_;

    // GID缓存（新增）
    uint8_t m_gid[16];
    bool m_gid_initialized = false;
    bool queryGid();
};




// zc add
extern SoRconn_collection* g_p_conn_collection;

inline SoR_connection* sorconn_collection_get_conn(int fd)
{
	if (g_p_conn_collection) 
		return g_p_conn_collection->find_sorconn(fd);
	return NULL;
}






#endif /* RING_H */
