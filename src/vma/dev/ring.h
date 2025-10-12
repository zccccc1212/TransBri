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
//每个socekt fd对应一个sockfd_tcp,每个sockfd_tcp对应一个sor connection，通过unordered_map找，而不是直接存在sockfd_tcp的实例内，只有当建立好连接的时候才初始化sor connection,避免资源浪费
//每个sor connection 维护两个ringbuffer，分别是recv和send，后续可以考虑融合为一个

// ringbuffer类中不需要关注是不是mr，只需要我们在初始化ringbuffer的时候通过ibv_reg_mr注册成MR即可
//  但是这个ringbuffer是封装好的，外面没办法访问里面的数据地址，只能直接操作进行拷贝数据
// 仔细考虑过后，数据发送的接口也放到ringbuffer类中，一层层调用ringbuffer来进行实际的数据发送
// 为了让我们得到cqe以后可以根据cqe中的wr_id找到对应的那一个request操作的数据的缓冲区的地址和长度，我们需要另外一个结构来存储这些信息，并在提交请求的时候将wr_id设为这个结构的地址，方便查询
class RingBuffer {
private:
    std::vector<unsigned char> buffer_;
    size_t capacity_;
    size_t head_;  // 读取位置
    size_t tail_;  // 写入位置
    size_t size_;  // 当前所占用的空间的大小
    size_t true_data_size_; //真正存储了数据的大小
    int flag_; //0 for recv 1 for send
    
    // 读写锁 - 允许多个读或单个写
    mutable std::shared_mutex rw_mutex_;
    
    // 用于条件变量的快速状态检查
    std::atomic<size_t> atomic_size_{0};
    std::atomic<size_t> atomic_available_{0};

    // 添加条件变量用于高效等待
    std::mutex m_cv_mutex;
    std::condition_variable m_cv;

    // 接收数据专用的条件变量和互斥锁
    std::mutex m_recv_mutex;
    std::condition_variable m_recv_cv;
    std::atomic<uint64_t> m_data_arrival_count{0}; // 数据到达计数器

public:
    // 禁用拷贝和移动构造
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&) = delete;
    RingBuffer& operator=(RingBuffer&&) = delete;

    // 构造函数 - 指定容量
    explicit RingBuffer(size_t capacity, int flag) 
        : capacity_(capacity), head_(0), tail_(0), size_(0), true_data_size_(0), flag_(flag) {
        if (capacity == 0) {
            throw std::invalid_argument("Capacity must be greater than 0");
        }
        buffer_.resize(capacity, 0);
        atomic_size_.store(0);
        atomic_available_.store(capacity);
    }

    ~RingBuffer() = default;

    // RAII包装器，用于自动锁管理
    class ReadLock {
    private:
        const RingBuffer& buffer_;
        std::shared_lock<std::shared_mutex> lock_;
    public:
        explicit ReadLock(const RingBuffer& buffer) 
            : buffer_(buffer), lock_(buffer.rw_mutex_) {}
        
        const RingBuffer* operator->() const { return &buffer_; }
        const RingBuffer& operator*() const { return buffer_; }
    };

    class WriteLock {
    private:
        RingBuffer& buffer_;
        std::unique_lock<std::shared_mutex> lock_;
    public:
        explicit WriteLock(RingBuffer& buffer) 
            : buffer_(buffer), lock_(buffer.rw_mutex_) {}
        
        RingBuffer* operator->() { return &buffer_; }
        RingBuffer& operator*() { return buffer_; }
    };

    // 获取缓冲区总容量 - 无锁，因为capacity_是const
    size_t capacity() const { return capacity_; }
    
    // 获取所占用的空间的大小 - 使用原子变量避免锁
    size_t size() const { return atomic_size_.load(std::memory_order_acquire); }
    

    size_t getHead() const {return head_;}

    size_t getTail() const {return tail_;}


    // 获取真正存储的数据量 - 需要读锁
    size_t true_data_size() const { 
        std::shared_lock lock(rw_mutex_);
        return true_data_size_; 
    }

    // 获取剩余空间 - 使用原子变量避免锁
    size_t available() const { return atomic_available_.load(std::memory_order_acquire); }
    
    // 检查是否为空 - 使用原子变量避免锁
    bool empty() const { return atomic_size_.load(std::memory_order_acquire) == 0; }

    // 检查是否已满 - 使用原子变量避免锁
    bool full() const { return atomic_available_.load(std::memory_order_acquire) == 0; }

    // 等待足够的数据到达
    bool wait_for_data(size_t required_true_data_size, int timeout_ms = 5000) {
        std::unique_lock<std::mutex> lock(m_recv_mutex);
        
        auto predicate = [this, required_true_data_size]() {
            return true_data_size_ >= required_true_data_size;
        };
        
        // 如果条件已经满足，立即返回
        if (predicate()) {
            return true;
        }
        
        // 保存当前数据到达计数，用于检测新数据
        uint64_t initial_count = m_data_arrival_count.load(std::memory_order_acquire);
        
        if (timeout_ms > 0) {
            auto result = m_recv_cv.wait_for(lock, 
                std::chrono::milliseconds(timeout_ms), predicate);
            
            if (!result) {
                // 超时，检查是否有任何新数据到达
                uint64_t current_count = m_data_arrival_count.load(std::memory_order_acquire);
                if (current_count > initial_count) {
                    // 有数据到达但不够，返回当前状态
                    return true_data_size_ >= required_true_data_size;
                }
                return false; // 真正超时，没有新数据
            }
            return true;
        } else {
            m_recv_cv.wait(lock, predicate);
            return true;
        }
    }
    
    // 通知数据到达
    void notify_data_arrival() {
        m_data_arrival_count.fetch_add(1, std::memory_order_release);
        m_recv_cv.notify_all();
    }
    
    // 获取数据到达计数（用于调试）
    uint64_t get_data_arrival_count() const {
        return m_data_arrival_count.load(std::memory_order_acquire);
    }
    
    // 在add_true_data_size中通知数据到达
    void add_true_data_size(size_t adddata) { 
        std::unique_lock<std::shared_mutex> lock(rw_mutex_);
        true_data_size_ += adddata; 
        
        // 通知数据到达
        if (adddata > 0) {
            // 注意：这里需要在锁外通知，避免死锁
            lock.unlock();
            notify_data_arrival();
        }
    }

    // 等待可用空间的函数
    void wait_available(size_t required_size, int timeout_ms = 5000) {
        auto start_time = std::chrono::steady_clock::now();
        
        while (atomic_available_.load(std::memory_order_acquire) < required_size) {
            // 检查超时
            if (timeout_ms > 0) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - start_time);
                if (elapsed.count() >= timeout_ms) {
                    throw std::runtime_error("Wait for available space timeout");
                }
            }
            
            // 短暂休眠避免忙等待
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    
    // 带条件变量的等待（更高效）
    void wait_available_cv(size_t required_size, int timeout_ms = 5000) {
        std::unique_lock<std::mutex> lock(m_cv_mutex);
        
        auto predicate = [this, required_size]() {
            return atomic_available_.load(std::memory_order_acquire) >= required_size;
        };
        
        if (timeout_ms > 0) {
            m_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), predicate);
        } else {
            m_cv.wait(lock, predicate);
        }
        
        // 检查是否真的满足条件（可能因超时唤醒）
        if (atomic_available_.load(std::memory_order_acquire) < required_size) {
            throw std::runtime_error("Wait for available space timeout");
        }
    }


    // 写入数据 - 返回实际写入的字节数 flag = 1 for write data, flag = 0 for write len
    size_t write(const void* data, size_t len, int flag) {
        if (len == 0 || full()) return 0;
        
        std::unique_lock lock(rw_mutex_);
        
        len = std::min(len, capacity_ - size_);
        if (len == 0) return 0;

        const unsigned char* src = static_cast<const unsigned char*>(data);
        
        // 计算连续可写入空间
        size_t contiguous = capacity_ - tail_;
        size_t to_write = std::min(len, contiguous);
        
        // 写入第一部分
        memcpy(&buffer_[tail_], src, to_write);
        
        // 如果需要回绕写入剩余部分
        if (to_write < len) {
            memcpy(&buffer_[0], src + to_write, len - to_write);
        }
        
        if(flag){
            true_data_size_ += len;
        }
        
        tail_ = (tail_ + len) % capacity_;
        size_ += len;
        
        // 更新原子变量
        atomic_size_.store(size_, std::memory_order_release);
        atomic_available_.store(capacity_ - size_, std::memory_order_release);
        
        return len;
    }

    // 读取数据但不移动读指针 - 返回实际读取的字节数
    size_t peek(void* data, size_t len) const {
        if (len == 0 || empty()) return 0;
        
        std::shared_lock lock(rw_mutex_);
        
        len = std::min(len, size_);
        unsigned char* dst = static_cast<unsigned char*>(data);
        
        // 计算连续可读取空间
        size_t contiguous = (head_ <= tail_) ? (tail_ - head_) : (capacity_ - head_);
        size_t to_read = std::min(len, contiguous);
        
        // 读取第一部分
        memcpy(dst, &buffer_[head_], to_read);
        
        // 如果需要回绕读取剩余部分
        if (to_read < len) {
            memcpy(dst + to_read, &buffer_[0], len - to_read);
        }
        
        return len;
    }

    // 读取数据并移动读指针 - 返回实际读取的字节数, flag = 1 for read data, flag = 0 for read len
    size_t read(void* data, size_t len, int flag) {
        if (len == 0 || empty()) return 0;
        
        std::unique_lock lock(rw_mutex_);
        
        len = std::min(len, size_);
        unsigned char* dst = static_cast<unsigned char*>(data);
        
        // 计算连续可读取空间
        size_t contiguous = (head_ <= tail_) ? (tail_ - head_) : (capacity_ - head_);
        size_t to_read = std::min(len, contiguous);
        
        // 读取第一部分
        memcpy(dst, &buffer_[head_], to_read);
        
        // 如果需要回绕读取剩余部分
        if (to_read < len) {
            memcpy(dst + to_read, &buffer_[0], len - to_read);
        }
        
        head_ = (head_ + len) % capacity_;
        size_ -= len;
        
        if(flag){
            true_data_size_ -= len;
        }
        
        // 更新原子变量
        atomic_size_.store(size_, std::memory_order_release);
        atomic_available_.store(capacity_ - size_, std::memory_order_release);
        
        return len;
    }

    // 丢弃数据 - 返回实际丢弃的字节数
    size_t discard(size_t len) {
        if (len == 0 || empty()) return 0;
        
        std::unique_lock lock(rw_mutex_);
        
        len = std::min(len, size_);
        head_ = (head_ + len) % capacity_;
        size_ -= len;
        
        // 更新原子变量
        atomic_size_.store(size_, std::memory_order_release);
        atomic_available_.store(capacity_ - size_, std::memory_order_release);
        
        return len;
    }

    // 清空缓冲区
    void clear() {
        std::unique_lock lock(rw_mutex_);
        head_ = tail_ = 0;
        size_ = 0;
        true_data_size_ = 0;
        
        // 更新原子变量
        atomic_size_.store(0, std::memory_order_release);
        atomic_available_.store(capacity_, std::memory_order_release);
    }

    
    /*
     * 在head指针前写入4个字节的数据
     * 前提条件：调用发生在数据读取后，不会覆盖已有数据，不会发生缓冲区溢出
     * @param data 要写入的数据指针（必须至少4个字节）
     * @return 总是返回4（固定写入4个字节）
     */
    size_t writeBeforeHead(const void* data) {
        std::unique_lock lock(rw_mutex_);

        const unsigned char* src = static_cast<const unsigned char*>(data);

        // 计算新的head位置（向前移动4个位置）
        size_t new_head = (head_ - 4 + capacity_) % capacity_;

        // 写入4个字节到new_head位置
        if (new_head + 4 <= capacity_) {
            // 不需要回绕，连续写入
            memcpy(&buffer_[new_head], src, 4);
        } else {
            // 需要回绕写入
            size_t first_part = capacity_ - new_head;
            memcpy(&buffer_[new_head], src, first_part);
            memcpy(&buffer_[0], src + first_part, 4 - first_part);
        }

        // 更新head指针和缓冲区大小
        head_ = new_head;
        
        // 更新原子变量
        atomic_size_.store(size_, std::memory_order_release);
        atomic_available_.store(capacity_ - size_, std::memory_order_release);

        return 4;
    }

    // 获取连续可读空间的大小 - 需要读锁
    size_t getContiguousReadSize() const {
        std::shared_lock lock(rw_mutex_);
        if (empty()) {
            return 0;
        }
        
        // 计算从head到缓冲区末尾的连续空间
        size_t contiguous = capacity_ - head_;
        
        // 如果数据没有回绕，返回实际连续大小
        if (head_ < tail_ || (head_ >= tail_ && tail_ == 0)) {
            return std::min(contiguous, size_);
        } else {
            // 数据回绕了，连续空间到缓冲区末尾
            return contiguous;
        }
    }

    // 获取连续可写空间的大小 - 需要读锁
    size_t getContiguousWriteSize() const {
        std::shared_lock lock(rw_mutex_);
        if (full()) {
            return 0;
        }
        
        // 计算从tail到缓冲区末尾的连续空间
        size_t contiguous = capacity_ - tail_;
        
        // 如果空间没有回绕，返回实际连续大小
        if (tail_ < head_ || (tail_ >= head_ && head_ == 0)) {
            return std::min(contiguous, capacity_ - size_);
        } else {
            // 空间回绕了，连续空间到缓冲区末尾
            return contiguous;
        }
    }
/*
    // 更新 head 指针（当外部直接读取了数据后调用）
    void updateHead(size_t bytes_processed) {
        std::unique_lock lock(rw_mutex_);
        
        head_ = (head_ + bytes_processed) % capacity_;
        size_ -= bytes_processed;
        
        // 如果缓冲区为空，重置指针到起始位置以保持连续性
        if (size_ == 0) {
            head_ = tail_ = 0;
        }
        
        // 更新原子变量
        atomic_size_.store(size_, std::memory_order_release);
        atomic_available_.store(capacity_ - size_, std::memory_order_release);
    }*/

    // 更新head指针时（数据被消费后）更新available
    void updateHead(size_t bytes_processed) {
        std::unique_lock<std::shared_mutex> lock(rw_mutex_);
        
        head_ = (head_ + bytes_processed) % capacity_;
        size_ -= bytes_processed;
        
        // 更新原子变量
        atomic_size_.store(size_, std::memory_order_release);
        atomic_available_.store(capacity_ - size_, std::memory_order_release);

        // 如果缓冲区为空，重置指针
        if (size_ == 0) {
            head_ = tail_ = 0;
        }
        
        // 通知等待的线程
        if (bytes_processed > 0) {
            m_cv.notify_all();  // 通知所有等待可用空间的线程
        }
    }

    // 更新 tail 指针（当外部直接写入了数据后调用）
    void updateTail(size_t bytes_added) {
        std::unique_lock lock(rw_mutex_);
        
        tail_ = (tail_ + bytes_added) % capacity_;
        size_ += bytes_added;
        
        // 更新原子变量
        atomic_size_.store(size_, std::memory_order_release);
        atomic_available_.store(capacity_ - size_, std::memory_order_release);
    }

    // 打印状态（调试用）
    void printStats() const {
        std::shared_lock lock(rw_mutex_);
        printf("RingBuffer Stats: Capacity=%zu, Size=%zu, Available=%zu, Head=%zu, Tail=%zu, TrueData=%zu\n",
               capacity_, size_, capacity_ - size_, head_, tail_, true_data_size_);
    }

    /**
     * 获取缓冲区数据存储的真实起始地址
     * @return 指向缓冲区起始位置的指针
     * @note 这个地址是缓冲区内存块的起始位置，不一定是有效数据的起始位置
     */
    unsigned char* getBufferStartAddress() {
        return buffer_.data();
    }

    const unsigned char* getBufferStartAddress() const {
        return buffer_.data();
    }

    /**
     * 获取tail指针当前指向的地址（写入位置）
     * @return 指向当前写入位置的指针
     * @note 如果缓冲区为空，返回的地址可能无效
     */
    unsigned char* getTailAddress() {
        std::shared_lock lock(rw_mutex_);
        return &buffer_[tail_];
    }

    const unsigned char* getTailAddress() const {
        std::shared_lock lock(rw_mutex_);
        return &buffer_[tail_];
    }

    /**
     * 获取head指针当前指向的地址（读取位置）
     * @return 指向当前读取位置的指针
     * @note 如果缓冲区为空，返回的地址可能无效
     */
    unsigned char* getHeadAddress() {
        std::shared_lock lock(rw_mutex_);
        return &buffer_[head_];
    }

    const unsigned char* getHeadAddress() const {
        std::shared_lock lock(rw_mutex_);
        return &buffer_[head_];
    }



    // 手动加锁方法（用于需要连续多个操作的情况）
    void lock_read() const { rw_mutex_.lock_shared(); }
    void unlock_read() const { rw_mutex_.unlock_shared(); }
    void lock_write() { rw_mutex_.lock(); }
    void unlock_write() { rw_mutex_.unlock(); }
};





class CQEPoller;



class SoR_connection{
private:
	int m_fd;	

    uint64_t cur_send_wr_id;
    uint64_t cur_recv_wr_id;

	
	int m_qpn;
	int m_recv_cqn;
	int m_send_cqn;
	long int send_buffer_total;
	long int recv_buffer_total;
	long int send_buffer_current;
	long int recv_buffer_current;
	

	union ibv_gid my_gid;
	int m_gidindex;
    CQEPoller* m_cqe_poller;


public:


    struct resources m_res;

    RingBuffer * m_send_rb;
	RingBuffer * m_recv_rb;

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
	
    int post_send(__const void *__buf, size_t __nbytes);
	int post_receive();

    size_t poll_send_completion();  // 专门轮询发送完成
    int poll_recv_completion();  // 专门轮询接收完成

    // 启动轮询线程
    void start_cqe_poller();

    // 停止轮询线程
    void stop_cqe_poller() ;

    // 发送数据时通知轮询线程
    int post_send_notify(__const void* data, size_t size);

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
    // 使用map存储fd到Sockfd_tcp指针的映射
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
        
        const int POLL_BATCH_SIZE = 32;
        const int IDLE_SLEEP_US = 1000;
        const int BUSY_POLL_THRESHOLD = 1000; // 忙碌轮询阈值
        
        struct ibv_wc wc_array[POLL_BATCH_SIZE];
        int consecutive_idle_cycles = 0;
        
        while (!m_stop_flag) {
            m_poll_cycles++;
            
            bool had_work = false;
            
            // 批量轮询发送CQ
            int send_polled = batch_poll_send_cq(wc_array, POLL_BATCH_SIZE);
            if (send_polled > 0) {
                had_work = true;
                consecutive_idle_cycles = 0;
                process_send_completions(wc_array, send_polled);
            }
            
            // 批量轮询接收CQ
            int recv_polled = batch_poll_recv_cq(wc_array, POLL_BATCH_SIZE);
            if (recv_polled > 0) {
                had_work = true;
                consecutive_idle_cycles = 0;
                process_recv_completions(wc_array, recv_polled);
            }
            
            // 自适应睡眠策略
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
        return ibv_poll_cq(m_connection->m_res.recv_cq, max_count, wc_array);
    }
    
    void process_send_completions(struct ibv_wc* wc_array, int count) {
        for (int i = 0; i < count; i++) {
            if (wc_array[i].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "Send completion error: 0x%x\n", wc_array[i].status);
                m_send_errors++;
                continue;
            }
            
            rdma_op_data* tracker = (rdma_op_data*)wc_array[i].wr_id;
            if (!tracker) {
                fprintf(stderr, "Invalid tracker in send completion\n");
                continue;
            }
            
            void* data_addr = tracker->data_addr;
            size_t data_size = tracker->data_size;
            
            printf("Send completed - Data addr: %p, Size: %zu , send buf head : %ld, tail : %ld \n", data_addr, data_size, m_connection->m_send_rb->getHead(), m_connection->m_send_rb->getTail());
            
            // 清理资源
            if (g_rdma_pool) {
                g_rdma_pool->deallocate(tracker);
            }
            
            // 更新发送窗口
            if (m_connection->m_send_rb) {
                m_connection->m_send_rb->updateHead(data_size);
            }
            /*
            if (m_connection->send_buffer_current >= data_size) {
                m_connection->send_buffer_current -= data_size;
            }*/
            
            m_send_completions++;
        }
    }
    
    void process_recv_completions(struct ibv_wc* wc_array, int count) {
        for (int i = 0; i < count; i++) {
            if (wc_array[i].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "Recv completion error: 0x%x\n", wc_array[i].status);
                m_recv_errors++;
                continue;
            }
            
            //说实话没搞明白到底需不需要这个字节序转换，先放着

            uint32_t this_seg_len_net;  // 网络字节序的长度
            m_connection->m_recv_rb->peek(&this_seg_len_net, 4);

            // 转换为主机字节序
            uint32_t this_seg_len = ntohl(this_seg_len_net);

            // 处理接收完成
            if (m_connection->m_recv_rb) {
                m_connection->m_recv_rb->add_true_data_size(this_seg_len_net);
            }
            
            printf("recv complte . this seg len net : %d  this seg len : %d \n", this_seg_len_net, this_seg_len);

            // 此时只是轮询到recv的 cqe，但是我们的RR本来就是满的，必须要应用程序确实读取数据以后才可以重新post recv，不然对端永远不知道接收缓冲区满没满，当RQ中没有RR了就说明接收缓冲区满了
            //m_connection->post_receive();
            
            // 更新接收窗口
            //if (m_connection->recv_buffer_current >= wc_array[i].byte_len) {
            //    m_connection->recv_buffer_current -= wc_array[i].byte_len;
            //}
            
            m_recv_completions++;
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
