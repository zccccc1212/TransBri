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
    struct ibv_cq *cq;                  /* CQ handle */
    struct ibv_qp *qp;                  /* QP handle */
    struct ibv_mr *recv_mr;                  /* MR handle for recv buf */
	struct ibv_mr *send_mr;                  /* MR handle for send buf */
    int sock;                           /* TCP socket file descriptor */
};

// zc add
//每个socekt fd对应一个sockfd_tcp,每个sockfd_tcp对应一个sor connection，通过unordered_map找，而不是直接存在sockfd_tcp的实例内，只有当建立好连接的时候才初始化sor connection,避免资源浪费
//每个sor connection 维护两个ringbuffer，分别是recv和send，后续可以考虑融合为一个

// ringbuffer类中不需要关注是不是mr，只需要我们在初始化ringbuffer的时候通过ibv_reg_mr注册成MR即可
//  但是这个ringbuffer是封装好的，外面没办法访问里面的数据地址，只能直接操作进行拷贝数据
// 仔细考虑过后，数据发送的接口也放到ringbuffer类中，一层层调用ringbuffer来进行实际的数据发送

class RingBuffer {
private:
    std::vector<unsigned char> buffer_;
    size_t capacity_;
    size_t head_;  // 读取位置
    size_t tail_;  // 写入位置
    size_t size_;  // 当前数据量


public:
    // 禁用拷贝和移动构造
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&) = delete;
    RingBuffer& operator=(RingBuffer&&) = delete;

    // 构造函数 - 指定容量
    explicit RingBuffer(size_t capacity) 
        : capacity_(capacity), head_(0), tail_(0), size_(0) {
        if (capacity == 0) {
            throw std::invalid_argument("Capacity must be greater than 0");
        }
        buffer_.resize(capacity);
        memset(buffer_, 0 , capacity_)
    }

    ~RingBuffer() = default;

    // 获取缓冲区总容量
    size_t capacity() const { return capacity_; }
    
    // 获取当前数据量
    size_t size() const { return size_; }
    
    // 获取剩余空间
    size_t available() const { return capacity_ - size_; }
    
    // 检查是否为空
    bool empty() const { return size_ == 0; }

    // 检查是否已满
    bool full() const { return size_ == capacity_; }

    // 获取当前有效数据的起始地址（head位置）
    unsigned char* getDataPtr() {
        if (empty()) {
            return nullptr;
        }
        return &buffer_[head_];
    }

    // 获取当前有效数据的常量起始地址
    const unsigned char* getDataPtr() const {
        if (empty()) {
            return nullptr;
        }
        return &buffer_[head_];
    }

    // 获取整个缓冲区的起始地址（用于MR注册）
    unsigned char* getBufferPtr() {
        return buffer_.data();
    }

    // 获取整个缓冲区的常量起始地址
    const unsigned char* getBufferPtr() const {
        return buffer_.data();
    }

    // 获取当前有效数据的连续长度（从head到缓冲区末尾或到tail）
    size_t getContiguousDataLength() const {
        if (empty()) {
            return 0;
        }
        
        // 如果数据没有回绕
        if (head_ <= tail_) {
            return tail_ - head_;
        } else {
            // 数据回绕了，连续长度到缓冲区末尾
            return capacity_ - head_;
        }
    }

    // 获取头部位置（调试用）
    size_t getHead() const { return head_; }

    // 获取尾部位置（调试用）
    size_t getTail() const { return tail_; }

    // 写入数据 - 返回实际写入的字节数
    size_t write(const void* data, size_t len) {
        if (len == 0 || full()) return 0;
        
        len = std::min(len, available());
        if (len == 0) return 0;

        const unsigned char* src = static_cast<const unsigned char*>(data);
        
        // 计算连续可写入空间
        size_t contiguous = capacity_ - tail_;
        size_t to_write = std::min(len, contiguous);
        
        // 写入第一部分
        std::memcpy(&buffer_[tail_], src, to_write);
        
        // 如果需要回绕写入剩余部分
        if (to_write < len) {
            std::memcpy(&buffer_[0], src + to_write, len - to_write);
        }
        
        tail_ = (tail_ + len) % capacity_;
        size_ += len;
        
        return len;
    }

    // 读取数据但不移动读指针 - 返回实际读取的字节数
    size_t peek(void* data, size_t len) const {
        if (len == 0 || empty()) return 0;
        
        len = std::min(len, size_);
        unsigned char* dst = static_cast<unsigned char*>(data);
        
        // 计算连续可读取空间
        size_t contiguous = capacity_ - head_;
        size_t to_read = std::min(len, contiguous);
        
        // 读取第一部分
        std::memcpy(dst, &buffer_[head_], to_read);
        
        // 如果需要回绕读取剩余部分
        if (to_read < len) {
            std::memcpy(dst + to_read, &buffer_[0], len - to_read);
        }
        
        return len;
    }

    // 读取数据并移动读指针 - 返回实际读取的字节数
    size_t read(void* data, size_t len) {
        size_t bytes_read = peek(data, len);
        if (bytes_read > 0) {
            head_ = (head_ + bytes_read) % capacity_;
            size_ -= bytes_read;
        }
        return bytes_read;
    }

    // 丢弃数据 - 返回实际丢弃的字节数
    size_t discard(size_t len) {
        if (len == 0 || empty()) return 0;
        
        len = std::min(len, size_);
        head_ = (head_ + len) % capacity_;
        size_ -= len;
        
        return len;
    }

    // 清空缓冲区
    void clear() {
        head_ = tail_ = 0;
        size_ = 0;
    }

    // 获取连续可读空间的指针
    const unsigned char* getContiguousReadBlock() const {
        if (empty()) {
            return nullptr;
        }
        
        return &buffer_[head_];
    }

    // 获取连续可写空间的指针
    unsigned char* getContiguousWriteBlock() {
        if (full()) {
            return nullptr;
        }
        
        return &buffer_[tail_];
    }

    // 获取连续可读空间的大小
    size_t getContiguousReadSize() const {
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

    // 获取连续可写空间的大小
    size_t getContiguousWriteSize() const {
        if (full()) {
            return 0;
        }
        
        // 计算从tail到缓冲区末尾的连续空间
        size_t contiguous = capacity_ - tail_;
        
        // 如果空间没有回绕，返回实际连续大小
        if (tail_ < head_ || (tail_ >= head_ && head_ == 0)) {
            return std::min(contiguous, available());
        } else {
            // 空间回绕了，连续空间到缓冲区末尾
            return contiguous;
        }


    // 打印状态（调试用）
    void printStats() const {
        printf("RingBuffer Stats: Capacity=%zu, Size=%zu, Available=%zu, Head=%zu, Tail=%zu\n",
               capacity_, size_, available(), head_, tail_);
    }
};


class SoR_connection{
private:
	int m_fd;	

	struct resources m_res;
	int m_qpn;
	int m_recv_cqn;
	int m_send_cqn;
	int send_buffer_total;
	int recv_buffer_total;
	int send_buffer_current;
	int recv_buffer_current;
	

	union ibv_gid my_gid;
	int m_gidindex;

	RingBuffer * m_send_rb;
	RingBuffer * m_recv_rb;
public:
	SoR_connection();
	~SoR_connection();	

	int post_send(__const void *__buf, size_t __nbytes);
	int poll_completion();
	int post_receive();

	int connect_to_peer();
	int sock_sync_data(int xfer_size, char *local_data, char *remote_data);
	int create_rdma_resources();
	int modify_qp();
	int modify_qp_to_init();
	int modify_qp_to_rtr();
	int modify_qp_to_rts();
	int create_ringbuffer(size_t capacity);
	


};

class SoRconn_collection{
public:
    SoRconn_collection();
    ~SoRconn_collection();

    // 添加socket文件描述符对应的sor conn 到集合
    int add_sorconn(int fd);
    
    // 根据fd查找对应的sor conn
    Sockfd_tcp* find_sorconn(int fd);
    
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



// zc add
extern SoRconn_collection* g_p_conn_collection;

inline SoR_connection* sorconn_collection_get_conn(int fd)
{
	if (g_p_conn_collection) 
		return g_p_conn_collection->find_sorconn(fd);
	return NULL;
}

#endif /* RING_H */
