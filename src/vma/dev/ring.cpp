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


#include "ring.h"
#include "vma/proto/route_table_mgr.h"

#undef  MODULE_NAME
#define MODULE_NAME     "ring"
#undef  MODULE_HDR
#define MODULE_HDR      MODULE_NAME "%d:%s() "


//zc add
/* poll CQ timeout in millisec (2 seconds) */
#define MAX_POLL_CQ_TIMEOUT 200000

#define MSG_SIZE 64
#define MR_SIZE 4294967296

#define RECV_SIZE	1048576  //rdma recv 操作的大小，可能设置成MTU好一点

#define RECV_WINDOW_SIZE    4095
#define CQE_SIZE     4095    //cqe size



simple_rdma_pool* g_rdma_pool = NULL;

SoRconn_collection* g_p_conn_collection = NULL;


ring::ring() :
	m_p_n_rx_channel_fds(NULL), m_parent(NULL)
{
	m_if_index = 0;

	print_val();
}

ring::~ring()
{
}

void ring::print_val()
{
	ring_logdbg("%d: %p: parent %p",
			m_if_index, this, ((uintptr_t)this == (uintptr_t)m_parent ? 0 : m_parent));
}



// zc add
SoR_connection::SoR_connection(int fd /*fd is the key to find sor conn*/){
	m_fd = fd;
	m_gidindex = 4;// 144 : 4  ; 155 : 2;
    m_send_rb = nullptr;
    m_recv_rb = nullptr;

    cur_send_wr_id = 0;
    cur_recv_wr_id = 0;


    send_buffer_total = MR_SIZE;
    recv_buffer_total = MR_SIZE;
	
    m_recv_buf = MR_SIZE;
    
    // 配置线程属性
    CQEPoller::ThreadConfig config;
    config.cpu_core = 4;                    // 绑定到CPU核心2
    config.realtime_scheduling = true;      // 启用实时调度（需要root权限）
    config.thread_name = "RDMA-CQE-Poller"; // 线程名称
    config.scheduling_priority = 90;        // 调度优先级

    m_cqe_poller = new CQEPoller(this, config);

    next_remote_to_write = 0;
    

	resources_init();
	int rc = create_rdma_resources();
    find_gid();
	if(rc == 1) // create rdma res success
	{

	}
	else{

	}

}

SoR_connection::~SoR_connection(){
    if (m_send_rb) delete m_send_rb;
    if (m_recv_rb) delete m_recv_rb;
}

void SoR_connection::resources_init(){
	memset(&m_res, 0 ,sizeof(m_res));
}


int SoR_connection::create_rdma_resources(){
	struct ibv_device **dev_list = NULL;
    struct ibv_qp_init_attr qp_init_attr;
    struct ibv_device *ib_dev = NULL;
    //size_t size;
    //int i;
    int mr_flags = 0;
    //int cq_size = 0;
    int num_devices;
    int rc = 0;

	/* get device names in the system */
    dev_list = ibv_get_device_list(&num_devices);
    if(!dev_list)
    {
        fprintf(stderr, "failed to get IB devices list\n");
        rc = 1;
        goto resources_create_exit;
    }

	/* if there isn't any IB device in host */
    if(!num_devices)
    {
        fprintf(stderr, "found %d device(s)\n", num_devices);
        rc = 1;
        goto resources_create_exit;
    }
    fprintf(stdout, "found %d device(s)\n", num_devices);
	
	// 这里应该是找到正确的rdma设备，在我们的服务器现在有多张rdma网卡的情况下，这里后续需要想办法区分一下，只能用英伟达的网卡
	// 好像不对，如果使用RC模式的话，照理来说intel的网卡也是可以的，这里需要区分吗？

	ib_dev = dev_list[0];

	/* get device handle */
    m_res.ib_ctx = ibv_open_device(ib_dev);
    if(!m_res.ib_ctx)
    {
        fprintf(stderr, "failed to open device \n");
        rc = 1;
        goto resources_create_exit;
    }

	ibv_free_device_list(dev_list);
	dev_list = NULL;
    ib_dev = NULL;

    /* query port properties */
    if(ibv_query_port(m_res.ib_ctx, 1, &m_res.port_attr))// 端口
    {
        fprintf(stderr, "ibv_query_port on port 0 failed\n");
        rc = 1;
        goto resources_create_exit;
    }


    /* allocate Protection Domain */
    m_res.pd = ibv_alloc_pd(m_res.ib_ctx);
    if(!m_res.pd)
    {
        fprintf(stderr, "ibv_alloc_pd failed\n");
        rc = 1;
        goto resources_create_exit;
    }
    
    // 创建发送CQ
    m_res.send_cq = ibv_create_cq(m_res.ib_ctx, CQE_SIZE, NULL, NULL, 0);
    if (!m_res.send_cq) {
        fprintf(stderr, "Failed to create send CQ\n");
        return -1;
    }
    
    // 创建接收CQ  
    m_res.recv_cq = ibv_create_cq(m_res.ib_ctx, CQE_SIZE, NULL, NULL, 0);
    if (!m_res.recv_cq) {
        fprintf(stderr, "Failed to create recv CQ\n");
        return -1;
    }


    //create ringbuffer and mr and rigister mr
    create_ringbuffer(MR_SIZE);
    
    
    mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE ;

    m_res.send_mr = ibv_reg_mr(m_res.pd, m_send_rb->getBufferStartAddress(), MR_SIZE, mr_flags);
	if(!m_res.send_mr)
    {
        fprintf(stderr, "ibv_reg_mr failed with mr_flags=0x%x\n", mr_flags);
        rc = 1;
        goto resources_create_exit;
    }
	fprintf(stdout, "MR was registered with addr=%p, lkey=0x%x, rkey=0x%x, flags=0x%x\n",
            m_send_rb->getBufferStartAddress(), m_res.send_mr->lkey, m_res.send_mr->rkey, mr_flags);

    m_res.recv_mr = ibv_reg_mr(m_res.pd, m_recv_rb->getBufferStartAddress(), MR_SIZE, mr_flags);
    if(!m_res.recv_mr)
    {
        fprintf(stderr, "ibv_reg_mr failed with mr_flags=0x%x\n", mr_flags);
        rc = 1;
        goto resources_create_exit;
    }
	fprintf(stdout, "MR was registered with addr=%p, lkey=0x%x, rkey=0x%x, flags=0x%x\n",
            m_recv_rb->getBufferStartAddress(), m_res.recv_mr->lkey, m_res.recv_mr->rkey, mr_flags);

    //set the recv window as MR
    
    remote_recv_window_mr = ibv_reg_mr(m_res.pd, (void *)&remote_recv_buffer, sizeof(remote_recv_window_mr), mr_flags);
    //remote_recv_window_mr = ibv_reg_mr(m_res.pd, (void *)&remote_recv_buffer, 4, mr_flags);

    if(!remote_recv_window_mr){
        fprintf(stderr, "ibv_reg_mr failed with mr_flags=0x%x\n", mr_flags);
    }
    fprintf(stdout, "MR was registered with addr=%p, lkey=0x%x, rkey=0x%x, flags=0x%x\n",
            (void *)&remote_recv_buffer, remote_recv_window_mr->lkey, remote_recv_window_mr->rkey, mr_flags);

    my_recv_window_mr = ibv_reg_mr(m_res.pd, (void *)&m_recv_buf, sizeof(m_recv_buf), mr_flags);
    //my_recv_window_mr = ibv_reg_mr(m_res.pd, (void *)&m_recv_buf, 4, mr_flags);
    if(!remote_recv_window_mr){
        fprintf(stderr, "ibv_reg_mr failed with mr_flags=0x%x\n", mr_flags);
    }
    fprintf(stdout, "MR was registered with addr=%p, lkey=0x%x, rkey=0x%x, flags=0x%x\n",
            (void *)&m_recv_buf, my_recv_window_mr->lkey, my_recv_window_mr->rkey, mr_flags);
    

	/* create the Queue Pair */
	memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.sq_sig_all = 1;
    qp_init_attr.send_cq = m_res.send_cq;
    qp_init_attr.recv_cq = m_res.recv_cq;
    qp_init_attr.cap.max_send_wr = 1+RECV_WINDOW_SIZE;//TODO : set the approperiate wr number
    qp_init_attr.cap.max_recv_wr = 1+RECV_WINDOW_SIZE;
    qp_init_attr.cap.max_send_sge = 2;
    qp_init_attr.cap.max_recv_sge = 2;
    m_res.qp = ibv_create_qp(m_res.pd, &qp_init_attr);

    if(!m_res.qp)
    {
        fprintf(stderr, "failed to create QP\n");
        rc = 1;
        goto resources_create_exit;
    }
    fprintf(stdout, "QP was created, QP number=0x%x\n", m_res.qp->qp_num);

resources_create_exit:
	if(rc)
    {
        /* Error encountered, cleanup */
        if(m_res.qp)
        {
            ibv_destroy_qp(m_res.qp);
            m_res.qp = NULL;
        }
		if(m_res.recv_mr)
        {
            ibv_dereg_mr(m_res.recv_mr);
            m_res.recv_mr = NULL;
        }
        if(m_res.send_mr)
        {
            ibv_dereg_mr(m_res.send_mr);
            m_res.send_mr = NULL;
        }
        if(m_res.send_cq)
        {
            ibv_destroy_cq(m_res.send_cq);
            m_res.send_cq = NULL;
        }
        if(m_res.recv_cq)
        {
            ibv_destroy_cq(m_res.recv_cq);
            m_res.recv_cq = NULL;
        }
        if(m_res.pd)
        {
            ibv_dealloc_pd(m_res.pd);
            m_res.pd = NULL;
        }
        if(m_res.ib_ctx)
        {
            ibv_close_device(m_res.ib_ctx);
            m_res.ib_ctx = NULL;
        }
        if(dev_list)
        {
            ibv_free_device_list(dev_list);
            dev_list = NULL;
        }
        if(m_res.sock >= 0)
        {
            if(close(m_res.sock))
            {
                fprintf(stderr, "failed to close socket\n");
            }
            m_res.sock = -1;
        }
    }
    return rc;
}

int SoR_connection::find_gid(){
	int rc = ibv_query_gid(m_res.ib_ctx, 1, m_gidindex, &my_gid);// 144
	if(rc)
    {
        fprintf(stderr, "could not get gid for port 1 index %d\n", m_gidindex);
    }
	return rc;
}

int SoR_connection::connect_to_peer(){

	struct cm_con_data_t local_con_data;
    struct cm_con_data_t remote_con_data;
    struct cm_con_data_t tmp_con_data;
    int rc;

	//prepare local rdma data to trans 
	local_con_data.addr = htonll((uintptr_t)m_recv_rb->getBufferStartAddress());
    local_con_data.rkey = htonl(m_res.recv_mr->rkey);
    local_con_data.qp_num = htonl(m_res.qp->qp_num);
    local_con_data.lid = htons(m_res.port_attr.lid);
    local_con_data.remote_recv_window_rkey = htonl(remote_recv_window_mr->rkey);

	memcpy(local_con_data.gid, &my_gid, 16);
    fprintf(stdout, "\nLocal LID = 0x%x\n", m_res.port_attr.lid);

	//同步两端的rdma元数据
    if(sock_sync_data(sizeof(struct cm_con_data_t), (char *) &local_con_data, (char *) &tmp_con_data) < 0)
    {
        fprintf(stderr, "failed to exchange connection data between sides\n");
        rc = 1;
        goto connect_qp_exit;
    }

	//recv remote rdma data
	remote_con_data.addr = ntohll(tmp_con_data.addr);
    remote_con_data.rkey = ntohl(tmp_con_data.rkey);
    remote_con_data.qp_num = ntohl(tmp_con_data.qp_num);
    remote_con_data.lid = ntohs(tmp_con_data.lid);
    remote_con_data.remote_recv_window_rkey = ntohl(tmp_con_data.remote_recv_window_rkey);

    memcpy(remote_con_data.gid, tmp_con_data.gid, 16);

    this->remote_recv_addr_start = remote_con_data.addr;
    this->total_send = 0;
    this->next_remote_to_write = remote_con_data.addr;
    this->remote_recv_buffer = MR_SIZE;

	/* save the remote side attributes, we will need it for the post SR */
    m_res.remote_props = remote_con_data;
    //fprintf(stdout, "  Remote address = 0x%"PRIx64"\n", remote_con_data.addr);
    fprintf(stdout, " Remote rkey = 0x%x\n", remote_con_data.rkey);
    fprintf(stdout, " Remote QP number = 0x%x\n", remote_con_data.qp_num);
    fprintf(stdout, " Remote LID = 0x%x\n", remote_con_data.lid);
    fprintf(stdout, " Remote recv window rkey = 0x%x\n", remote_con_data.remote_recv_window_rkey);


	rc = modify_qp();
	if(rc){

	}

    for(int j = 0; j < RECV_WINDOW_SIZE;++j){
        post_receive_for_recv_window();
    }

    start_cqe_poller();


connect_qp_exit:
	return rc;
}

int SoR_connection::sock_sync_data(int xfer_size, char * local_data, char * remote_data){
	int rc;
    int read_bytes = 0;
    int total_read_bytes = 0;

	rc = orig_os_api.write(m_fd, local_data, xfer_size);

    if(rc < xfer_size)
    {
        fprintf(stderr, "Failed writing data during sock_sync_data\n");
    }
    else
    {
        rc = 0;
    }

    while(!rc && total_read_bytes < xfer_size)
    {
        read_bytes = orig_os_api.read(m_fd, remote_data, xfer_size);
        if(read_bytes > 0)
        {
            total_read_bytes += read_bytes;
        }
        else
        {
            rc = read_bytes;
        }
    }
    return rc;
}

int SoR_connection::modify_qp(){
	int rc = modify_qp_to_init();
	if(rc)
    {
        fprintf(stderr, "change QP state to INIT failed\n");
        goto modify_qp_exit;
    }

	rc = modify_qp_to_rtr();
	if(rc)
    {
        fprintf(stderr, "failed to modify QP state to RTR\n");
        goto modify_qp_exit;
    }

	rc = modify_qp_to_rts();
    if(rc)
    {
        fprintf(stderr, "failed to modify QP state to RTS\n");
        goto modify_qp_exit;
    }
    fprintf(stdout, "QP state was change to RTS\n");

modify_qp_exit:
	return rc;
}

int SoR_connection::modify_qp_to_init(){
	struct ibv_qp_attr attr;
    int flags;
    int rc;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;
    attr.port_num = 1;
    attr.pkey_index = 0;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    rc = ibv_modify_qp(m_res.qp, &attr, flags);
    if(rc)
    {
        fprintf(stderr, "failed to modify QP state to INIT\n");
    }
    return rc;
}

int SoR_connection::modify_qp_to_rtr(){
	struct ibv_qp_attr attr;
    int flags;
    int rc;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_256;
    attr.dest_qp_num = m_res.remote_props.qp_num;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 0x12;
    attr.ah_attr.is_global = 0;
    attr.ah_attr.dlid = m_res.remote_props.lid;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = 0;
    if(m_gidindex >= 0)
    {
        attr.ah_attr.is_global = 1;
        attr.ah_attr.port_num = 1;
        memcpy(&attr.ah_attr.grh.dgid, m_res.remote_props.gid, 16);
        attr.ah_attr.grh.flow_label = 0;
        attr.ah_attr.grh.hop_limit = 1;
        attr.ah_attr.grh.sgid_index = m_gidindex;// gid index 
        attr.ah_attr.grh.traffic_class = 0;
    }

    flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
            IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    rc = ibv_modify_qp(m_res.qp, &attr, flags);
    if(rc)
    {
        fprintf(stderr, "failed to modify QP state to RTR\n");
    }
    return rc;
}

int SoR_connection::modify_qp_to_rts(){
	struct ibv_qp_attr attr;
    int flags;
    int rc;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 0x12;
    attr.retry_cnt = 6;
    attr.rnr_retry = 0;
    attr.sq_psn = 0;
    attr.max_rd_atomic = 1;
    flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
            IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    rc = ibv_modify_qp(m_res.qp, &attr, flags);
    if(rc)
    {
        fprintf(stderr, "failed to modify QP state to RTS\n");
    }
    return rc;
}

//post recv 对于使用imm的write，接收方也一定要提前post recv，只是recv中的sge 可以为空，也可以不为空，但是没有实际意义
int SoR_connection::post_receive_for_recv_window(){
    struct ibv_recv_wr rr;
    struct ibv_sge sge;
    struct ibv_recv_wr *bad_wr;
    int rc;

    /* prepare the scatter/gather entry */
    memset(&sge, 0, sizeof(sge));

    //TODO: set the correct sge.addr
    sge.addr = (uintptr_t)&remote_recv_buffer;
    sge.length = 8;
    sge.lkey = remote_recv_window_mr->lkey;

    /* prepare the receive work request */
    memset(&rr, 0, sizeof(rr));
    rr.next = NULL;
    rr.wr_id = (uint64_t)cur_recv_wr_id;//将wr_id设置为rdma_op_data对应的结构的地址，直接方便查询
    
    cur_recv_wr_id++;

    rr.sg_list = &sge;
    rr.num_sge = 1;

    /* post the Receive Request to the RQ */
    rc = ibv_post_recv(m_res.qp, &rr, &bad_wr);
    if(rc)
    {
        // 获取错误代码
        int error_code = errno;
        std::cerr << "ibv_post_recv failed with error code: " << error_code 
                  << " (" << strerror(error_code) << ")" << std::endl;

        // 可以根据具体的错误代码进行更细致的处理
        switch (error_code) {
            case EINVAL:
                std::cerr << "Invalid value provided in QP, WR, or bad_wr." << std::endl;
                break;
            case ENOMEM:
                std::cerr << "Not enough memory to post the receive request." << std::endl;
                break;
            default:
                std::cerr << "Unknown error." << std::endl;
                break;
        }
    }
    else
    {
        //fprintf(stdout, "Receive Request was posted\n");
    }
    return rc;
}

size_t SoR_connection::write_data_in_send_buf(__const void *__buf, size_t __nbytes){
    size_t data_size = m_send_rb->write(__buf, __nbytes);
    if(data_size < __nbytes) {
        fprintf(stderr, "failed to write data, expected %zu, actual %zu\n", __nbytes, data_size);
        return -1;
    }
    return data_size;
}

int SoR_connection::post_send_data_with_imm(){
    struct ibv_send_wr sr;
    struct ibv_sge sge;
    struct ibv_send_wr *bad_wr = NULL;
    int rc;
    size_t continues_size;

    size_t cur_data_size = m_send_rb->size();
    size_t remote_recv_buf = get_remote_recv_buf();
    uint32_t will_to_send = remote_recv_buf > cur_data_size ? cur_data_size : remote_recv_buf;

    // prepare the scatter/gather entry 
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)m_send_rb->get_unack_region(continues_size);
    if(continues_size < will_to_send){

    }

    m_send_rb->mark_sent(will_to_send);
    sge.length = will_to_send;
    sge.lkey = m_res.send_mr->lkey;

    // 从内存池获取跟踪对象
    rdma_op_data* tracker = g_rdma_pool->allocate((void *)sge.addr, sge.length, 0);  // 0=发送

    // prepare the send work request 
    memset(&sr, 0, sizeof(sr));
    sr.next = NULL;
    sr.wr_id = (uint64_t)tracker; // 将wr_id设置为rdma_op_data对应的结构的地址，直接方便查询
    
    cur_send_wr_id++;

    sr.sg_list = &sge;
    sr.num_sge = 1;
    sr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM; // 改为带立即数的RDMA WRITE
    
    // 设置立即数 - 通常用于携带元数据，比如数据长度、操作类型等
    sr.imm_data = htonl(will_to_send); // 将数据长度作为立即数发送
    
    sr.wr.rdma.remote_addr = remote_recv_addr_start  + (total_send % MR_SIZE); // 需要预先获取的远程内存地址
    sr.wr.rdma.rkey = m_res.remote_props.rkey;        // 需要预先获取的远程rkey

    total_send += will_to_send;

    //next_remote_to_write += will_to_send;

    // 执行RDMA WRITE操作 
    rc = ibv_post_send(m_res.qp, &sr, &bad_wr);
    if(rc)
    {
        fprintf(stderr, "failed to post RDMA WRITE with immediate\n");
    }
    else
    {
        fprintf(stdout, "RDMA WRITE with immediate was posted, will_to_send: %u\n", will_to_send);
    }
    return rc;
}

size_t SoR_connection::sync_remote_recv_window(){
    struct ibv_send_wr sr;
    struct ibv_sge sge;
    struct ibv_send_wr *bad_wr = NULL;
    int rc;

    // prepare the scatter/gather entry 
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)&m_recv_buf;
    sge.length = 8;
    sge.lkey = my_recv_window_mr->lkey;

     // prepare the send work request 
    memset(&sr, 0, sizeof(sr));
    sr.next = NULL;
    sr.wr_id = 1;

    sr.sg_list = &sge;
    sr.num_sge = 1;
    sr.opcode = IBV_WR_SEND; // RDMA SEND

    rc = ibv_post_send(m_res.qp, &sr, &bad_wr);
    if(rc)
    {
        fprintf(stderr, "failed to post RDMA SEND\n");
    }
    else
    {
        fprintf(stdout, "RDMA send was posted, is used to sync remote recv window\n");
    }
    return rc;
}

bool SoR_connection::wait_remote_recv_buf() {
    std::unique_lock<std::mutex> lock(m_recv_mutex);
    
    // 带超时的等待，避免永久阻塞
    bool success = m_recv_cv.wait_for(lock, std::chrono::seconds(10), [this]() {
        return m_data_ready;
    });
    
    if (!success) {
        // 超时处理
        return false;
    }
    
    m_data_ready = false;
    return true;
}

// 唤醒等待的函数
void SoR_connection::update_my_remote_recv_window_notify(){
    {
        std::lock_guard<std::mutex> lock(m_recv_mutex);
        m_data_ready = true;
    }
    m_recv_cv.notify_one();  // 或者 notify_all() 根据需求
}

//现在有问题，如果是使用atomic操作让接收方通知发送方更新发送缓冲区，那么发送方怎么知道缓冲区更新了呢
//或许可以维护两个值，主要是怕有冲突，一个是发送方发完一次数据以后记录接收方的大小，另一个也是，然后发送方下一次发送数据以前就比较这两个大小是否有更新，但是这样如果是阻塞情况下就不知道什么时候更新，因为没有通知
//要不就设置一个超时循环，要不就选用带通知的方法
//要不考虑为一个进程里面所有的socket都创建一个额外的接收缓冲区更新cq
//其实可以直接用send和recv操作，直接写现在发送方还剩多大的数据就可以了
//而且这个recv也会非常方便，每次post recv的大小和起始地址都是一样的其实
//然后轮循到cqe以后根据cqe的类型，就可以知道是数据来了，还是控制流消息（更新窗口大小）来了，容易区分

size_t SoR_connection::poll_recv_completion() {
    struct ibv_wc wc;
    unsigned long start_time_msec;
    unsigned long cur_time_msec;
    struct timeval cur_time;
    int poll_result;
    //int rc = 0;
    
    gettimeofday(&cur_time, NULL);
    start_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);
    
    do {
        // 关键：只轮询接收CQ
        poll_result = ibv_poll_cq(m_res.recv_cq, 1, &wc);
        gettimeofday(&cur_time, NULL);
        cur_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);
    } while ((poll_result == 0) && ((cur_time_msec - start_time_msec) < MAX_POLL_CQ_TIMEOUT));
    
    if (poll_result < 0) {
        fprintf(stderr, "poll recv CQ failed\n");
        return -1;
    } else if (poll_result == 0) {
        fprintf(stderr, "recv completion not found after timeout\n");
        return -2;
    } else {
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "recv failed with status: 0x%x\n", wc.status);
            return -3;
        }
        if(wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM){
            uint32_t imm_data = wc.imm_data;
            uint32_t this_seg_len = ntohl(imm_data);
            m_recv_rb->updateTail(this_seg_len);
            update_my_recv_window_reduce(this_seg_len);
        }
        else if (wc.opcode == IBV_WC_RECV)
        {
            //TODO notify sender recv window has updated
            this->update_my_remote_recv_window_notify();
            printf("this send is to notify remote to update recv window\n");
        }
        else{
            fprintf(stderr, "we don't expect to get a wc not recv and recv with imm \n");
        }
        return 1;
    }
}

int SoR_connection::create_ringbuffer(size_t capacity){
    RingBuffer * sendbuf = new SendBuffer(capacity);
    RingBuffer * recvbuf = new RecvBuffer(capacity);
    m_send_rb = sendbuf;
    m_recv_rb = recvbuf;
    return 0;

}

// 启动轮询线程
void SoR_connection::start_cqe_poller() {
    if (m_cqe_poller) {
        m_cqe_poller->start();
    }
}

// 停止轮询线程
void SoR_connection::stop_cqe_poller() {
    if (m_cqe_poller) {
        m_cqe_poller->stop();
    }
}

int SoR_connection::post_send_notify_with_imm() {
    int rc = post_send_data_with_imm();  // 原来的post_send_data函数
    if (rc == 0 && m_cqe_poller) {
        m_cqe_poller->notify_send_work();
    }
    return rc;
}




SoRconn_collection::SoRconn_collection(){

}

SoRconn_collection::~SoRconn_collection(){
    clear();
}



int SoRconn_collection::add_sorconn(int fd /*fd is key*/){

    if(fd < 0){
        return -1; //fd is invalid
    }

    auto it = m_conn_map.find(fd);
    if(it != m_conn_map.end()){
        return -2; // already exist this fd sor conn
    }
    
    try{
        SoR_connection* sorconn = new SoR_connection(fd);
        m_conn_map[fd] = sorconn;
        return 1;
    } catch (const std::exception& e){
        return -3;
    }
}

SoR_connection* SoRconn_collection::find_sorconn(int fd){
    auto it = m_conn_map.find(fd);
    if(it == m_conn_map.end()){
        return nullptr;
    }
    return it->second;
}

int  SoRconn_collection::remove_sorconn(int fd){
    auto it = m_conn_map.find(fd);
    if(it == m_conn_map.end()){// 本来就不存在
        return -1; 
    }
    delete it->second; // 先使用delete删除对应的new所指向的资源

    m_conn_map.erase(it);
    return 1;// 删除成功

}


size_t SoRconn_collection::size() const{
    return m_conn_map.size();
}

void SoRconn_collection::clear(){
    for(auto & pair : m_conn_map){

        delete pair.second;// 这是为了删除实例，释放资源所占的空间
    }
    m_conn_map.clear();
}







