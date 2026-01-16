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
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <netinet/in.h>
#include <arpa/inet.h>


#undef  MODULE_NAME
#define MODULE_NAME     "ring"
#undef  MODULE_HDR
#define MODULE_HDR      MODULE_NAME "%d:%s() "


//zc add
/* poll CQ timeout in millisec (2 seconds) */
#define MAX_POLL_CQ_TIMEOUT 200000

#define RECV_WINDOW_SIZE    8000
#define CQE_SIZE     8000    //cqe size



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
SoR_connection::SoR_connection(int fd /*fd is the key to find sor conn*/)
    : m_fd(fd)
    , cur_send_wr_id(0)
    , cur_recv_wr_id(0)
{
	m_gidindex = 4;// 144 : 4  ; 155 : 2;
    m_send_rb = nullptr;
    m_recv_rb = nullptr;


    send_buffer_total = MR_SIZE;
    recv_buffer_total = MR_SIZE;
	
    m_recv_buf = MR_SIZE;
    
    // 配置线程属性
    CQEPoller::ThreadConfig config;
    config.cpu_core = 4;                    // 绑定到CPU核心2
    config.realtime_scheduling = true;      // 启用实时调度（需要root权限）
    config.thread_name = "RDMA-CQE-Poller"; // 线程名称
    config.scheduling_priority = 99;        // 调度优先级

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

    m_res.send_mr = ibv_reg_mr(m_res.pd, m_send_rb->get_buffer_start(), MR_SIZE, mr_flags);
	if(!m_res.send_mr)
    {
        fprintf(stderr, "ibv_reg_mr failed with mr_flags=0x%x\n", mr_flags);
        rc = 1;
        goto resources_create_exit;
    }
	fprintf(stdout, "MR was registered with addr=%p, lkey=0x%x, rkey=0x%x, flags=0x%x\n",
            m_send_rb->get_buffer_start(), m_res.send_mr->lkey, m_res.send_mr->rkey, mr_flags);

    m_res.recv_mr = ibv_reg_mr(m_res.pd, m_recv_rb->get_buffer_start(), MR_SIZE, mr_flags);
    if(!m_res.recv_mr)
    {
        fprintf(stderr, "ibv_reg_mr failed with mr_flags=0x%x\n", mr_flags);
        rc = 1;
        goto resources_create_exit;
    }
	fprintf(stdout, "MR was registered with addr=%p, lkey=0x%x, rkey=0x%x, flags=0x%x\n",
            m_recv_rb->get_buffer_start(), m_res.recv_mr->lkey, m_res.recv_mr->rkey, mr_flags);

    //set the recv window as MR
    
    remote_recv_window_mr = ibv_reg_mr(m_res.pd, (void *)&remote_recv_buffer, sizeof(remote_recv_buffer), mr_flags);
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
    qp_init_attr.cap.max_send_wr = RECV_WINDOW_SIZE;//TODO : set the approperiate wr number
    qp_init_attr.cap.max_recv_wr = RECV_WINDOW_SIZE;
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
	local_con_data.addr = htonll((uintptr_t)m_recv_rb->get_buffer_start());
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
    m_recv_buf_remote = MR_SIZE;
	
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
    attr.path_mtu = IBV_MTU_4096;
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
    int flag = 0;

    if(!try_acquire_send_slot()){
        return 0;
    }

    size_t cur_data_size = m_send_rb->unsent_size();
    size_t remote_recv_buf = get_remote_recv_buf();
    uint32_t will_to_send = remote_recv_buf > cur_data_size ? cur_data_size : remote_recv_buf;

    // prepare the scatter/gather entry 
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)m_send_rb->get_unsent_region(continues_size);
    if(continues_size < will_to_send){
        flag = 1;
        will_to_send = continues_size;
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

    // 执行RDMA WRITE操作 
    rc = ibv_post_send(m_res.qp, &sr, &bad_wr);
    if(rc)
    {
        fprintf(stderr, "failed to post RDMA WRITE with immediate.  cur_send_wr_id : %ld , total_send : %ld flag : %d\n", cur_send_wr_id, total_send, flag);
    }
    else
    {
        //fprintf(stdout, "RDMA WRITE with immediate was posted, will_to_send: %u\n", will_to_send);
        //std::lock_guard<std::mutex> lock(m_recv_mutex);
        remote_recv_buffer -= will_to_send;
    }

    if(flag){// 说明数据到这里其实分段了，所以我们要分两次调用ibv_post_send,分两次发送具体的数据
        post_send_data_with_imm();
    }
    return rc;
}

size_t SoR_connection::sync_remote_recv_window(){
    struct ibv_send_wr sr;
    struct ibv_sge sge;
    struct ibv_send_wr *bad_wr = NULL;
    int rc;

    std::lock_guard<std::mutex> lock(m_recv_mutex);
    m_recv_buf_remote = m_recv_buf;


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
        fprintf(stderr, "failed to post RDMA SEND to sync remote recv window \n");
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

// 非阻塞版本

std::mutex g_cq_poll_mutex;

size_t SoR_connection::poll_recv_completion() {
    struct ibv_wc wc_array[3200];
    unsigned long start_time_msec;
    unsigned long cur_time_msec;
    struct timeval cur_time;
    int poll_result;
    //int rc = 0;
    
    gettimeofday(&cur_time, NULL);
    start_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);
    
    do {
        // 关键：只轮询接收CQ
        {
            std::unique_lock<std::mutex> lock(g_cq_poll_mutex, std::try_to_lock);
            if (!lock.owns_lock()) {
                return 0; // 锁被占用，直接返回
            }
            poll_result = ibv_poll_cq(m_res.recv_cq, 3200, wc_array);
        }
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
        for(int i = 0;i < poll_result;++i){
            if (wc_array[i].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "recv failed with status: 0x%x\n", wc_array[i].status);
                return -3;
            }
            if(wc_array[i].opcode == IBV_WC_RECV_RDMA_WITH_IMM){
                uint32_t imm_data = wc_array[i].imm_data;
                uint32_t this_seg_len = ntohl(imm_data);
                m_recv_rb->updateTail(this_seg_len);
                update_my_recv_window_reduce(this_seg_len);
                post_receive_for_recv_window();
            }
            else if (wc_array[i].opcode == IBV_WC_RECV)
            {
                //TODO notify sender recv window has updated
                this->update_my_remote_recv_window_notify();
                printf("this send is to notify remote to update recv window\n");
            }
            else{
                fprintf(stderr, "we don't expect to get a wc not recv and recv with imm \n");
            }
        }
        return 1;
    }
}

int SoR_connection::create_ringbuffer(size_t capacity){
    SendBuffer * sendbuf = new SendBuffer(capacity);
    RecvBuffer * recvbuf = new RecvBuffer(capacity);
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



//SoRconn_collection impl
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

int SoRconn_collection::remove_sorconn(int fd){
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


//CQManager udp impl
// 构造函数
CQManager::CQManager() 
    : m_shared_send_cq(nullptr)
    , m_shared_recv_cq(nullptr)
    , m_polling_stop(false)
    , m_send_completions(0)
    , m_recv_completions(0)
    , m_send_errors(0)
    , m_recv_errors(0) {
    std::cout << "CQManager created (singleton)" << std::endl;
}

// 析构函数
CQManager::~CQManager() {
    cleanup();
    std::cout << "CQManager destroyed" << std::endl;
}

// 创建共享的CQ
bool CQManager::createSharedCQs(ibv_context* context, 
                               uint32_t send_cq_size,
                               uint32_t recv_cq_size,
                               void* send_cq_context,
                               void* recv_cq_context) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // 如果CQ已经创建，先清理
    if (m_shared_send_cq || m_shared_recv_cq) {
        cleanup();
    }
    
    // 创建发送CQ
    m_shared_send_cq = ibv_create_cq(context, send_cq_size, 
                                    send_cq_context, nullptr, 0);
    if (!m_shared_send_cq) {
        std::cerr << "Failed to create shared send CQ: " << strerror(errno) << std::endl;
        return false;
    }
    
    // 创建接收CQ
    m_shared_recv_cq = ibv_create_cq(context, recv_cq_size,
                                    recv_cq_context, nullptr, 0);
    if (!m_shared_recv_cq) {
        std::cerr << "Failed to create shared recv CQ: " << strerror(errno) << std::endl;
        ibv_destroy_cq(m_shared_send_cq);
        m_shared_send_cq = nullptr;
        return false;
    }
    
    std::cout << "Created shared CQs: send_cq=" << m_shared_send_cq 
              << ", recv_cq=" << m_shared_recv_cq << std::endl;
    return true;
}

// 启动轮询线程
void CQManager::startPollingThreads() {
    if (m_polling_stop) {
        m_polling_stop = false;
        
        // 启动发送CQ轮询线程
        if (m_shared_send_cq) {
            m_send_polling_thread = std::thread(&CQManager::sendPollingThread, this);
            std::cout << "Started send CQ polling thread" << std::endl;
        }
        
        // 启动接收CQ轮询线程
        if (m_shared_recv_cq) {
            m_recv_polling_thread = std::thread(&CQManager::recvPollingThread, this);
            std::cout << "Started recv CQ polling thread" << std::endl;
        }
    }
}
// 发送轮询线程函数
void CQManager::sendPollingThread() {
    std::cout << "Send polling thread started" << std::endl;
    
    while (!m_polling_stop.load(std::memory_order_acquire)) {
        // 轮询发送CQ，使用非阻塞方式
        int num_completions = pollSendCq(10); // 0表示非阻塞
        
        // 如果没有完成事件，短暂休眠避免CPU过度占用
        if (num_completions == 0) {
            // 使用std::this_thread::sleep_for替代条件变量等待
            std::this_thread::sleep_for(std::chrono::microseconds(10)); // 10微秒
        }
    }
    
    std::cout << "Send polling thread exiting" << std::endl;
}

// 接收轮询线程函数
void CQManager::recvPollingThread() {
    std::cout << "Recv polling thread started" << std::endl;
    
    while (!m_polling_stop.load(std::memory_order_acquire)) {
        // 轮询接收CQ，使用非阻塞方式
        int num_completions = pollRecvCq(10); // 0表示非阻塞
        
        // 如果没有完成事件，短暂休眠避免CPU过度占用
        if (num_completions == 0) {
            // 使用std::this_thread::sleep_for替代条件变量等待
            std::this_thread::sleep_for(std::chrono::microseconds(10)); // 10微秒
        }
        
    }
    
    std::cout << "Recv polling thread exiting" << std::endl;
}

// 轮询发送CQ
int CQManager::pollSendCq(int timeout_ms) {
    if (!m_shared_send_cq) {
        std::cerr << "Shared send CQ not created" << std::endl;
        return -1;
    }
    
    int num_completions = 0;
    ibv_wc wc;
    
    // 轮询CQ
    while (true) {
        int ret = ibv_poll_cq(m_shared_send_cq, 1, &wc);
        if (ret < 0) {
            std::cerr << "Error polling send CQ: " << strerror(errno) << std::endl;
            m_send_errors++;
            return -1;
        } else if (ret == 0) {
            // 没有完成事件
            break;
        }
        
        // 处理完成事件
        handleSendCompletion(wc);
        num_completions++;
        m_send_completions++;
        
        // 检查是否还有更多完成事件
        if (timeout_ms > 0) {
            // 如果有超时限制，只处理一次
            break;
        }
    }
    
    return num_completions;
}

// 轮询接收CQ
int CQManager::pollRecvCq(int timeout_ms) {
    if (!m_shared_recv_cq) {
        std::cerr << "Shared recv CQ not created" << std::endl;
        return -1;
    }
    
    int num_completions = 0;
    ibv_wc wc;
    
    // 轮询CQ
    while (true) {
        int ret = ibv_poll_cq(m_shared_recv_cq, 1, &wc);
        if (ret < 0) {
            std::cerr << "Error polling recv CQ: " << strerror(errno) << std::endl;
            m_recv_errors++;
            return -1;
        } else if (ret == 0) {
            // 没有完成事件
            break;
        }
        
        // 处理完成事件
        handleRecvCompletion(wc);
        num_completions++;
        m_recv_completions++;
        
        // 检查是否还有更多完成事件
        if (timeout_ms > 0) {
            // 如果有超时限制，只处理一次
            break;
        }
    }
    
    return num_completions;
}

// 处理发送完成事件
void CQManager::handleSendCompletion(ibv_wc& wc) {
    if (wc.status != IBV_WC_SUCCESS) {
        std::cout << "send wc something wrong happen " << std::endl;
    }

    int fd = QpnToFdMap::get().getFd(wc.qp_num);
    Socket_transbridge* sock_ptr = nullptr;
    if (fd != -1) {
        sock_ptr = my_fd_collection_get_sockfd(fd);
        // 要进行动态类型转换
        if (sock_ptr) {
            Socket_tb_udp* udp_sock = dynamic_cast<Socket_tb_udp*>(sock_ptr);
            if (udp_sock) {
                bool success = udp_sock->m_rdma_manager->send_buffer()->mark_block_sent_completed();
                    if (!success) {
                        std::cerr << "Warning: Failed to mark block as sent completed for QP " 
                                  << wc.qp_num << std::endl;
                    }
            }
            else {
                std::cerr << "Warning: Socket is not UDP type for QP " << wc.qp_num << std::endl;
            }
        } else {
            std::cerr << "Warning: No socket found for fd " << fd << std::endl;
        }
    } else {
        std::cerr << "Warning: No fd found for QP " << wc.qp_num << std::endl;
    }
    // 默认处理：打印完成事件信息
        std::cout << "Send completion: QP=" << qp_name 
                  << " (qp_num=" << wc.qp_num << ")"
                  << ", wr_id=" << wc.wr_id
                  << ", status=" << ibv_wc_status_str(wc.status)
                  << ", opcode=" << wc.opcode
                  << ", byte_len=" << wc.byte_len 
                  << ", fd=" << fd
                  << std::endl;
    
    
}

// 处理接收完成事件
void CQManager::handleRecvCompletion(ibv_wc& wc) {

    if (wc.status != IBV_WC_SUCCESS) {
        std::cout << "recv wc something wrong happen " << std::endl;
    }

     int fd = QpnToFdMap::get().getFd(wc.qp_num);
    Socket_transbridge* sock_ptr = nullptr;
    
    // 通过QP号找到对应的socket
    if (fd != -1) {
        sock_ptr = my_fd_collection_get_sockfd(fd);
        
        // 要进行动态类型转换，确保是UDP类型的socket
        if (sock_ptr) {
            Socket_tb_udp* udp_sock = dynamic_cast<Socket_tb_udp*>(sock_ptr);
            
            if (udp_sock) {
                // 1. 更新接收缓冲区状态
                if (udp_sock->m_rdma_manager && udp_sock->m_rdma_manager->recv_buffer()) {
                    // 标记接收缓冲区中已接收完成的块
                    bool success = udp_sock->m_rdma_manager->recv_buffer()->mark_block_received_completed();
                }
            }
            else {
                std::cerr << "Warning: Socket is not UDP type for QP " << wc.qp_num << std::endl;
            }
        } else {
            std::cerr << "Warning: No socket found for fd " << fd << std::endl;
        }
    } else {
        std::cerr << "Warning: No fd found for QP " << wc.qp_num << std::endl;
    }
    // 默认处理：打印完成事件信息
        std::cout << "recv completion: QP=" << qp_name 
                  << " (qp_num=" << wc.qp_num << ")"
                  << ", wr_id=" << wc.wr_id
                  << ", status=" << ibv_wc_status_str(wc.status)
                  << ", opcode=" << wc.opcode
                  << ", byte_len=" << wc.byte_len 
                  << ", fd=" << fd
                  << std::endl;
}


// 停止轮询线程
void CQManager::stopPollingThreads() {
    m_polling_stop = true;
    
    // 通知轮询线程
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cv.notify_all();
    }
    
    // 等待线程结束
    if (m_send_polling_thread.joinable()) {
        m_send_polling_thread.join();
        std::cout << "Send polling thread stopped" << std::endl;
    }
    
    if (m_recv_polling_thread.joinable()) {
        m_recv_polling_thread.join();
        std::cout << "Recv polling thread stopped" << std::endl;
    }
}


//UD qp
// 单例实例

UDRdmaManager::UDRdmaManager(uint32_t local_ip, uint16_t local_port)
    : local_ip_(local_ip), local_port_(local_port) {
    // 构造函数实现
}

UDRdmaManager::~UDRdmaManager() {
    cleanup();
}

bool UDRdmaManager::initialize(uint32_t local_ip, uint16_t local_port,int sockfd,
                                   size_t send_buffer_size,
                                   size_t recv_buffer_size,
                                   uint32_t max_send_wr,
                                   uint32_t max_recv_wr,
                                   uint32_t max_send_sge,
                                   uint32_t max_recv_sge
                                    ) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_initialized) {
        std::cout << "RDMA manager already initialized" << std::endl;
        return true;
    }
    
    std::cout << "Initializing Simple RDMA Manager..." << std::endl;
    
    // 步骤1: 发现并打开设备
    if (!discoverAndOpenDevice()) {
        return false;
    }
    
    // 步骤2: 创建保护域
    if (!createProtectionDomain()) {
        cleanup();
        return false;
    }

     // 查询GID（应该在设备发现和打开后调用）
    if (!queryGid()) {
        std::cerr << "Warning: Failed to query GID, continuing without GID" << std::endl;
        return false;
    }
    
    // 步骤3: 创建完成队列
    if (!createCompletionQueues()) {
        cleanup();
        return false;
    }
    
    // 步骤4: 分配和注册内存（可选）
    // 这里可能还需要重新考虑，因为要和ringbuffer联动起来
    if (send_buffer_size > 0 || recv_buffer_size > 0) {
        if (!allocateAndRegisterMemory(local_ip, local_port, send_buffer_size, recv_buffer_size)) {
            cleanup();
            return false;
        }
    }
    
    // 步骤5: 创建UD队列对
    if (!createUdQueuePair(max_send_wr, max_recv_wr, max_send_sge, max_recv_sge, sockfd)) {
        cleanup();
        return false;
    }
    
    // 步骤6: 初始化UD QP
    if (!initUdQp()) {
        cleanup();
        return false;
    }
    
    // 步骤7: 发布初始接收工作请求（WR）
    // 在初始化完成后立即发布接收WR，数量等于接收缓冲区的块数
    if (recv_buffer_ && recv_buffer_size > 0) {
        size_t initial_recv_wrs = recv_buffer_->block_count();
        std::cout << "Posting initial " << initial_recv_wrs 
                  << " receive WRs for buffer of " << recv_buffer_size << " blocks" << std::endl;
        
        size_t posted = post_recv(initial_recv_wrs);
        
        if (posted != initial_recv_wrs) {
            std::cerr << "Warning: Failed to post all initial receive WRs. "
                      << "Posted " << posted << " out of " << initial_recv_wrs << std::endl;
        } else {
            std::cout << "Successfully posted " << posted << " initial receive WRs" << std::endl;
        }
    } else {
        std::cerr << "Warning: No receive buffer created, skipping initial receive WR posting" << std::endl;
    }

    m_initialized = true;
    std::cout << "Simple RDMA Manager initialized successfully!" << std::endl;
    printInfo();
    
    return true;
}

void UDRdmaManager::cleanup() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_initialized) return;
    
    std::cout << "Cleaning up RDMA resources..." << std::endl;
    
    // 销毁QP
    if (m_resources.qp) {
        ibv_destroy_qp(m_resources.qp);
        m_resources.qp = nullptr;
    }
    
    // 注销内存区域
    if (m_resources.memory.recv_mr) {
        ibv_dereg_mr(m_resources.memory.recv_mr);
        m_resources.memory.recv_mr = nullptr;
    }
    
    if (m_resources.memory.send_mr) {
        ibv_dereg_mr(m_resources.memory.send_mr);
        m_resources.memory.send_mr = nullptr;
    }
    
    // 释放缓冲区
    if (m_resources.memory.recv_buf) {
        free(m_resources.memory.recv_buf);
        m_resources.memory.recv_buf = nullptr;
    }
    
    if (m_resources.memory.send_buf) {
        free(m_resources.memory.send_buf);
        m_resources.memory.send_buf = nullptr;
    }
    
    // 销毁CQ
    if (m_resources.recv_cq) {
        ibv_destroy_cq(m_resources.recv_cq);
        m_resources.recv_cq = nullptr;
    }
    
    if (m_resources.send_cq) {
        ibv_destroy_cq(m_resources.send_cq);
        m_resources.send_cq = nullptr;
    }
    
    // 销毁PD
    if (m_resources.pd) {
        ibv_dealloc_pd(m_resources.pd);
        m_resources.pd = nullptr;
    }
    
    // 关闭设备上下文
    if (m_resources.context) {
        ibv_close_device(m_resources.context);
        m_resources.context = nullptr;
    }
    
    m_initialized = false;
    std::cout << "RDMA resources cleaned up" << std::endl;
}

bool UDRdmaManager::discoverAndOpenDevice() {
    int num_devices = 0;
    ibv_device** dev_list = ibv_get_device_list(&num_devices);
    
    if (!dev_list) {
        setLastError("Failed to get IB devices list");
        return false;
    }
    
    if (num_devices == 0) {
        ibv_free_device_list(dev_list);
        setLastError("No RDMA devices found");
        return false;
    }
    
    std::cout << "Found " << num_devices << " RDMA device(s)" << std::endl;
    
    // 显示所有设备信息
    for (int i = 0; i < num_devices; ++i) {
        const char* dev_name = ibv_get_device_name(dev_list[i]);
        std::cout << "  Device " << i << ": " << dev_name;
        
        // 如果是第一个设备，标记为将选择的设备
        if (i == 0) {
            std::cout << " (will use this device)";
        }
        std::cout << std::endl;
    }
    
    // 始终选择第一个设备
    ibv_device* selected_device = dev_list[0];
    const char* dev_name = ibv_get_device_name(selected_device);
    m_resources.device.name = dev_name;
    
    // 打开设备
    m_resources.context = ibv_open_device(selected_device);
    ibv_free_device_list(dev_list);
    
    if (!m_resources.context) {
        setLastError("Failed to open RDMA device");
        return false;
    }
    
    std::cout << "Opened device: " << m_resources.device.name << std::endl;
    
    // 查询端口属性（尝试端口1，如果失败则尝试端口2）
    m_resources.device.port_num = 1;
    ibv_port_attr port_attr;
    if (ibv_query_port(m_resources.context, m_resources.device.port_num, &port_attr)) {
        m_resources.device.port_num = 2;
        if (ibv_query_port(m_resources.context, m_resources.device.port_num, &port_attr)) {
            setLastError("Failed to query port attributes");
            return false;
        }
    }
    
    m_resources.device.lid = port_attr.lid;
    std::cout << "Using port " << (int)m_resources.device.port_num 
              << ", LID: 0x" << std::hex << m_resources.device.lid << std::dec << std::endl;
    
    return true;
}

bool UDRdmaManager::createProtectionDomain() {
    m_resources.pd = ibv_alloc_pd(m_resources.context);
    if (!m_resources.pd) {
        setLastError("Failed to allocate protection domain");
        return false;
    }
    
    std::cout << "Created protection domain" << std::endl;
    return true;
}


bool UDRdmaManager::createCompletionQueues() {
    // 获取CQManager单例
    CQManager& cq_manager = CQManager::getInstance();
    
    // 检查共享CQ是否已经创建
    if (cq_manager.getSharedSendCq() && cq_manager.getSharedRecvCq()) {
        // 共享CQ已经存在，直接使用
        
        std::cout << "Using existing shared completion queues: send_cq=" << m_resources.send_cq 
                  << ", recv_cq=" << m_resources.recv_cq << std::endl;
        return true;
    }
    
    // 共享CQ不存在，创建新的共享CQ
    if (!cq_manager.createSharedCQs(m_resources.context, 8000, 8000, nullptr, nullptr)) {
        setLastError("Failed to create shared completion queues");
        return false;
    }
    
    // 启动轮询线程
    cq_manager.startPollingThreads();
    
    std::cout << "Created shared completion queues: send_cq=" << m_resources.send_cq 
              << ", recv_cq=" << m_resources.recv_cq << std::endl;
    return true;
}

bool UDRdmaManager::allocateAndRegisterMemory(uint32_t local_ip, uint16_t local_port, size_t send_block_count, size_t recv_block_count) {
    // 创建发送缓冲区
    if (send_block_count > 0) {
        try {
            send_buffer_ = std::make_unique<SequentialUdpBuffer>(
                local_ip, local_port, send_block_count);
        } catch (const std::exception& e) {
            setLastError(std::string("Failed to create send buffer: ") + e.what());
            return false;
        }
        
        // 获取发送缓冲区的内存信息
        unsigned char* send_buf = send_buffer_->buffer_data();
        size_t send_size = send_buffer_->buffer_size();
        
        // 注册发送缓冲区为MR
        int send_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ;
        send_mr_ = ibv_reg_mr(m_resources.pd, send_buf, send_size, send_access_flags);
        
        if (!send_mr_) {
            send_buffer_.reset();
            setLastError("Failed to register send memory region");
            return false;
        }
        
        std::cout << "Registered send memory: " << send_size << " bytes" 
                  << " (" << send_block_count << " blocks)"
                  << ", lkey=0x" << std::hex << send_mr_->lkey 
                  << ", rkey=0x" << send_mr_->rkey << std::dec << std::endl;
    }
    
    // 创建接收缓冲区
    if (recv_block_count > 0) {
        try {
            recv_buffer_ = std::make_unique<SequentialUdpBuffer>(
                local_ip, local_port, recv_block_count);
        } catch (const std::exception& e) {
            setLastError(std::string("Failed to create receive buffer: ") + e.what());
            
            // 如果接收缓冲区创建失败，清理已创建的发送缓冲区
            if (send_mr_) {
                ibv_dereg_mr(send_mr_);
                send_mr_ = nullptr;
                send_buffer_.reset();
            }
            return false;
        }
        
        // 获取接收缓冲区的内存信息
        unsigned char* recv_buf = recv_buffer_->buffer_data();
        size_t recv_size = recv_buffer_->buffer_size();
        
        // 注册接收缓冲区为MR
        int recv_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
        recv_mr_ = ibv_reg_mr(m_resources.pd, recv_buf, recv_size, recv_access_flags);
        
        if (!recv_mr_) {
            recv_buffer_.reset();
            
            // 如果接收MR注册失败，清理已创建的发送MR
            if (send_mr_) {
                ibv_dereg_mr(send_mr_);
                send_mr_ = nullptr;
                send_buffer_.reset();
            }
            
            setLastError("Failed to register receive memory region");
            return false;
        }
        
        std::cout << "Registered receive memory: " << recv_size << " bytes" 
                  << " (" << recv_block_count << " blocks)"
                  << ", lkey=0x" << std::hex << recv_mr_->lkey 
                  << ", rkey=0x" << recv_mr_->rkey << std::dec << std::endl;
    }
    
    return true;
}

UDRdmaManager::MemoryRegions UDRdmaManager::get_memory_regions() const {
    MemoryRegions regions;
    regions.send_mr = send_mr_;
    regions.recv_mr = recv_mr_;
    regions.send_size = send_buffer_ ? send_buffer_->buffer_size() : 0;
    regions.recv_size = recv_buffer_ ? recv_buffer_->buffer_size() : 0;
    return regions;
}

bool UDRdmaManager::createUdQueuePair(uint32_t max_send_wr,
                                         uint32_t max_recv_wr,
                                         uint32_t max_send_sge,
                                         uint32_t max_recv_sge,
                                        int socket_fd) {
    ibv_qp_init_attr qp_init_attr;
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    
    // 设置UD QP属性
    qp_init_attr.qp_type = IBV_QPT_UD;  // UD类型
    qp_init_attr.sq_sig_all = 1;        // 所有发送请求都产生完成事件
    


    // 获取CQManager单例
    CQManager& cq_manager = CQManager::getInstance();
    
    // 获取共享CQ
    ibv_cq* shared_send_cq = cq_manager.getSharedSendCq();
    ibv_cq* shared_recv_cq = cq_manager.getSharedRecvCq();

    // 检查共享CQ是否已创建
    if (!shared_send_cq || !shared_recv_cq) {
        setLastError("Shared CQs not created. Call CQManager::createSharedCQs first.");
        return false;
    }

    // 使用共享CQ
    qp_init_attr.send_cq = shared_send_cq;
    qp_init_attr.recv_cq = shared_recv_cq;
    
    qp_init_attr.cap.max_send_wr = max_send_wr;
    qp_init_attr.cap.max_recv_wr = max_recv_wr;
    qp_init_attr.cap.max_send_sge = max_send_sge;
    qp_init_attr.cap.max_recv_sge = max_recv_sge;
    qp_init_attr.cap.max_inline_data = 0;  // 内联数据大小，0表示不使用内联
    
    m_resources.qp = ibv_create_qp(m_resources.pd, &qp_init_attr);
    if (!m_resources.qp) {
        setLastError("Failed to create UD queue pair");
        return false;
    }
    
    std::cout << "Created UD queue pair, QP number: 0x" << std::hex 
              << m_resources.qp->qp_num << std::dec << std::endl;
    
    // 建立QP与socket fd的映射
    if (socket_fd >= 0) {
        QpnToFdMap::get().add(m_resources.qp->qp_num, socket_fd);
        std::cout << "Mapped QPN " << std::hex << m_resources.qp->qp_num 
                  << std::dec << " to socket FD " << socket_fd << std::endl;
    }


    return true;
}


bool UDRdmaManager::initUdQp() {
    // 对于UD QP，我们需要将其状态从RESET修改为INIT
    ibv_qp_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));
    
    qp_attr.qp_state = IBV_QPS_INIT;
    qp_attr.pkey_index = 0;
    qp_attr.port_num = m_resources.device.port_num;
    qp_attr.qkey = 0x111111;  // UD QP需要设置qkey，这里使用一个默认值
    
    int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_QKEY;
    
    if (ibv_modify_qp(m_resources.qp, &qp_attr, flags)) {
        setLastError("Failed to modify UD QP to INIT state");
        return false;
    }
    
    // 然后从INIT修改为RTR（准备接收）
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_RTR;
    
    flags = IBV_QP_STATE;
    
    if (ibv_modify_qp(m_resources.qp, &qp_attr, flags)) {
        setLastError("Failed to modify UD QP to RTR state");
        return false;
    }
    
    // 最后从RTR修改为RTS（准备发送）
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_RTS;
    qp_attr.sq_psn = 0;  // UD QP需要设置PSN
    
    flags = IBV_QP_STATE | IBV_QP_SQ_PSN;
    
    if (ibv_modify_qp(m_resources.qp, &qp_attr, flags)) {
        setLastError("Failed to modify UD QP to RTS state");
        return false;
    }
    
    std::cout << "UD QP initialized to RTS state" << std::endl;
    return true;
}


void UDRdmaManager::deregisterMemory(ibv_mr* mr) {
    if (mr) {
        ibv_dereg_mr(mr);
        std::cout << "Deregistered memory region" << std::endl;
    }
}

void UDRdmaManager::printInfo() const {
    std::cout << "\n=== Simple RDMA Manager Info ===" << std::endl;
    std::cout << "Status: " << (m_initialized ? "Initialized" : "Not initialized") << std::endl;
    
    if (m_initialized) {
        std::cout << "Device: " << m_resources.device.name << std::endl;
        std::cout << "Port: " << (int)m_resources.device.port_num << std::endl;
        std::cout << "LID: 0x" << std::hex << m_resources.device.lid << std::dec << std::endl;
        std::cout << "QP number: 0x" << std::hex << getQpNum() << std::dec << std::endl;
        std::cout << "QP type: UD (Unreliable Datagram)" << std::endl;
        
        if (m_resources.memory.send_buf) {
            std::cout << "Send buffer: " << m_resources.memory.send_size << " bytes" << std::endl;
        }
        
        if (m_resources.memory.recv_buf) {
            std::cout << "Receive buffer: " << m_resources.memory.recv_size << " bytes" << std::endl;
        }
    }
    std::cout << "================================\n" << std::endl;
}

void UDRdmaManager::setLastError(const std::string& error) {
    strncpy(m_errorMsg, error.c_str(), sizeof(m_errorMsg) - 1);
    m_errorMsg[sizeof(m_errorMsg) - 1] = '\0';
    std::cerr << "RDMA Error: " << error << std::endl;
}

void UDRdmaManager::setLastError(int errnum) {
    strncpy(m_errorMsg, strerror(errnum), sizeof(m_errorMsg) - 1);
    m_errorMsg[sizeof(m_errorMsg) - 1] = '\0';
    std::cerr << "RDMA Error: " << m_errorMsg << std::endl;
}

// 轮询发送完成队列
int UDRdmaManager::pollSendCompletionQueue(int timeout_ms = 100) {//默认每次轮询轮询100ms
    if (!m_initialized || !m_resources.send_cq) {
        setLastError("Send CQ not initialized");
        return -1;
    }
    
    return pollCompletionQueue(m_resources.send_cq, timeout_ms);
}

// 轮询接收完成队列
int UDRdmaManager::pollRecvCompletionQueue(int timeout_ms = 100) {
    if (!m_initialized || !m_resources.recv_cq) {
        setLastError("Receive CQ not initialized");
        return -1;
    }
    
    return pollCompletionQueue(m_resources.recv_cq, timeout_ms);
}

// 通用的轮询完成队列方法
int UDRdmaManager::pollCompletionQueue(ibv_cq* cq, int timeout_ms) {
    if (!cq) {
        setLastError("Invalid completion queue");
        return -1;
    }
    
    const int MAX_CQE_PER_POLL = 8000;  // 与CQ大小匹配
    ibv_wc wc_array[MAX_CQE_PER_POLL];
    
    // 如果指定了超时时间，使用带超时的轮询
    if (timeout_ms > 0) {
        struct timeval start_time, current_time;
        gettimeofday(&start_time, NULL);
        
        do {
            // 尝试轮询CQ，一次轮询多个CQE
            int num_completions = ibv_poll_cq(cq, MAX_CQE_PER_POLL, wc_array);
            
            if (num_completions > 0) {
                // 处理所有完成事件
                int total_handled = 0;
                for (int i = 0; i < num_completions; i++) {
                    int result = handleCompletion(wc_array[i]);
                    if (result > 0) {
                        total_handled++;
                    } else if (result < 0) {
                        // 发生错误，但继续处理剩余CQE
                        std::cerr << "Error handling completion #" << i << std::endl;
                    }
                }
                return total_handled;  // 返回成功处理的CQE数量
            } else if (num_completions < 0) {
                setLastError("Error polling completion queue");
                return -1;
            }
            
            // 检查是否超时
            gettimeofday(&current_time, NULL);
            long elapsed_ms = (current_time.tv_sec - start_time.tv_sec) * 1000 +
                             (current_time.tv_usec - start_time.tv_usec) / 1000;
            
            if (elapsed_ms >= timeout_ms) {
                // 超时，没有完成事件
                return 0;
            }     
            
        } while (true);
    } 
    // 如果没有指定超时时间，立即轮询一次
    else if (timeout_ms == 0) {
        // 非阻塞轮询，立即返回
        int num_completions = ibv_poll_cq(cq, MAX_CQE_PER_POLL, wc_array);
        
        if (num_completions > 0) {
            // 处理所有完成事件
            int total_handled = 0;
            for (int i = 0; i < num_completions; i++) {
                int result = handleCompletion(wc_array[i]);
                if (result > 0) {
                    total_handled++;
                } else if (result < 0) {
                    std::cerr << "Error handling completion #" << i << std::endl;
                }
            }
            return total_handled;
        } else if (num_completions < 0) {
            setLastError("Error polling completion queue");
            return -1;
        }
        
        // 没有完成事件
        return 0;
    }
    // timeout_ms < 0 表示阻塞轮询（无限等待）
    else {
        while (true) {
            // 阻塞轮询，每次轮询多个CQE
            int num_completions = ibv_poll_cq(cq, MAX_CQE_PER_POLL, wc_array);
            
            if (num_completions > 0) {
                // 处理所有完成事件
                int total_handled = 0;
                for (int i = 0; i < num_completions; i++) {
                    int result = handleCompletion(wc_array[i]);
                    if (result > 0) {
                        total_handled++;
                    } else if (result < 0) {
                        std::cerr << "Error handling completion #" << i << std::endl;
                    }
                }
                return total_handled;
            } else if (num_completions < 0) {
                setLastError("Error polling completion queue");
                return -1;
            }
            
            // 短暂休眠，避免忙等待消耗CPU
            usleep(100); // 休眠100微秒
        }
    }
}

// 处理完成事件
int UDRdmaManager::handleCompletion(ibv_wc& wc) {
    if (wc.status != IBV_WC_SUCCESS) {
        std::cerr << "Work completion error: " << ibv_wc_status_str(wc.status) 
                  << " (opcode: " << wc.opcode << ", wr_id: " << wc.wr_id << ")" << std::endl;
        
        // 根据错误类型处理
        switch (wc.status) {
            case IBV_WC_RETRY_EXC_ERR:
                setLastError("Retry exceeded error");
                break;
            case IBV_WC_RNR_RETRY_EXC_ERR:
                setLastError("RNR retry exceeded error");
                break;
            case IBV_WC_LOC_QP_OP_ERR:
                setLastError("Local QP operation error");
                break;
            case IBV_WC_LOC_PROT_ERR:
                setLastError("Local protection error");
                break;
            case IBV_WC_WR_FLUSH_ERR:
                setLastError("Work request flushed error");
                break;
            default:
                setLastError("Unknown completion error");
                break;
        }
        
        // 返回负值表示错误
        return -static_cast<int>(wc.status);
    }
    
    // 成功完成，返回工作请求ID或操作类型
    std::cout << "Completion success: opcode=" << wc.opcode 
              << ", byte_len=" << wc.byte_len 
              << ", wr_id=" << wc.wr_id << std::endl;
    
    // 根据操作类型返回不同的正值
    switch (wc.opcode) {
        case IBV_WC_SEND:
            return 1;  // 发送完成
        case IBV_WC_RECV:
            return 2;  // 接收完成
        case IBV_WC_RDMA_WRITE:
            return 3;  // RDMA写完成
        case IBV_WC_RDMA_READ:
            return 4;  // RDMA读完成
        default:
            return wc.wr_id > 0 ? wc.wr_id : 1;  // 返回wr_id或默认值
    }
}

// ============ 接收缓冲区相关方法实现 ============
// 初始化静态原子变量
std::atomic<uint64_t> UDRdmaManager::wr_id_counter_{0};

size_t UDRdmaManager::post_recv(size_t n) {
    if (!m_initialized || !m_resources.qp) {
        setLastError("RDMA manager not initialized or QP not available");
        return 0;
    }
    
    if (n == 0) {
        return 0;
    }
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
    size_t posted_count = 0;
    
    for (size_t i = 0; i < n; ++i) {
        if (!post_single_recv_internal()) {
            // 如果发布失败，停止继续发布
            break;
        }
        posted_count++;
    }
    
    return posted_count;
}

bool UDRdmaManager::post_single_recv_internal() {
    if (!recv_buffer_) {
        setLastError("Receive buffer not created");
        return false;
    }
    
    // 检查接收缓冲区是否有可用空间
    if (recv_buffer_->available_blocks() == 0) {
        // 接收缓冲区已满，无法发布新的接收请求
        return false;
    }
    
    // 获取下一个接收位置的指针
    unsigned char* recv_ptr = recv_buffer_->get_next_receive_ptr();
    if (!recv_ptr) {
        setLastError("Failed to get next receive pointer from buffer");
        return false;
    }
    
    // 获取接收缓冲区的MR信息
    uint32_t lkey = getRecvBufferLkey();
    if (lkey == 0) {
        setLastError("Recv buffer MR not properly registered");
        return false;
    }
    
    // 准备接收工作请求
    struct ibv_recv_wr wr, *bad_wr = nullptr;
    struct ibv_sge sge;
    
    // 设置SGE
    sge.addr = reinterpret_cast<uintptr_t>(recv_ptr);
    sge.length = static_cast<uint32_t>(recv_buffer_->total_block_size());
    sge.lkey = lkey;
    
    // 设置接收工作请求
    memset(&wr, 0, sizeof(wr));
    
    // 使用全局递增的wr_id
    wr.wr_id = wr_id_counter_.fetch_add(1, std::memory_order_relaxed);
    
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.next = nullptr;
    
    // 发布接收请求
    int ret = ibv_post_recv(m_resources.qp, &wr, &bad_wr);
    if (ret != 0) {
        setLastError(ret);
        return false;
    }
    
    return true;
}


bool UDRdmaManager::updateLocalAddress(uint32_t new_local_ip, uint16_t new_local_port) {
    // 检查是否与当前地址相同
    if (local_ip_ == new_local_ip && local_port_ == new_local_port) {
        return true;  // 地址未改变
    }
    
    std::cout << "Updating local address from " 
              << local_ip_ << ":" << local_port_
              << " to " << new_local_ip << ":" << new_local_port << std::endl;
    
    // 更新本地地址
    local_ip_ = new_local_ip;
    local_port_ = new_local_port;
    
    // 重新配置发送缓冲区
    if (send_buffer_) {
        // 注意：这里可能需要重新创建或重新配置SequentialUdpBuffer
        // 或者SequentialUdpBuffer应该支持动态更新本地地址
        // 暂时记录警告
        std::cout << "Warning: Local address changed, but buffers may still use old address" 
                  << std::endl;
    }
    
    return true;
}

bool UDRdmaManager::queryGid() {
    // 查询GID
    int gid_index = 4;  //
    
    ibv_gid gid;
    int ret = ibv_query_gid(m_resources.context, m_resources.device.port_num, 
                           gid_index, &gid);
    
    if (ret != 0) {
        std::cerr << "Failed to query GID: " << strerror(errno) << std::endl;
        m_gid_initialized = false;
        memset(m_gid, 0, 16);
        return false;
    }
    
    // 复制GID到缓存
    memcpy(m_gid, gid.raw, 16);
    m_gid_initialized = true;
    
    // 打印GID（调试用）
    std::cout << "GID queried successfully: ";
    for (int i = 0; i < 16; i++) {
        printf("%02x", m_gid[i]);
        if (i % 2 == 1 && i != 15) printf(":");
    }
    printf("\n");
    
    return true;
}







SequentialUdpBuffer::SequentialUdpBuffer(uint32_t local_ip, uint16_t local_port,
                                         size_t block_count)
    : buffer_(block_count * BLOCK_TOTAL_SIZE),
      block_valid_(block_count, false),
      block_count_(block_count),
      local_ip_(local_ip),
      local_port_(local_port),
      head_(0),
      tail_(0),
      used_blocks_(0),
      next_receive_index_(0) {
    
    if (block_count == 0) {
        throw std::invalid_argument("block_count must be greater than 0");
    }
}

// ============ 缓冲区信息 ============
size_t SequentialUdpBuffer::block_count() const noexcept { 
    return block_count_; 
}

size_t SequentialUdpBuffer::data_capacity() const noexcept { 
    return DATA_SIZE; 
}

size_t SequentialUdpBuffer::total_block_size() const noexcept { 
    return BLOCK_TOTAL_SIZE; 
}

size_t SequentialUdpBuffer::header_size() const noexcept { 
    return HEADER_SIZE; 
}

// ============ 缓冲区状态 ============
size_t SequentialUdpBuffer::available_blocks() const noexcept { 
    return block_count_ - used_blocks_.load(std::memory_order_acquire); 
}

size_t SequentialUdpBuffer::used_blocks() const noexcept { 
    return used_blocks_.load(std::memory_order_acquire); 
}

bool SequentialUdpBuffer::has_data() const noexcept { 
    return used_blocks_.load(std::memory_order_acquire) > 0; 
}

bool SequentialUdpBuffer::is_full() const noexcept { 
    return used_blocks_.load(std::memory_order_acquire) >= block_count_; 
}

bool SequentialUdpBuffer::is_empty() const noexcept { 
    return used_blocks_.load(std::memory_order_acquire) == 0; 
}

// ============ 本地地址信息 ============
uint32_t SequentialUdpBuffer::local_ip() const noexcept { 
    return local_ip_; 
}

uint16_t SequentialUdpBuffer::local_port() const noexcept { 
    return local_port_; 
}

// ============ 发送缓冲区接口 ============
bool SequentialUdpBuffer::write_block(const void* data, size_t len) {
    if (!data || len == 0 || len > DATA_SIZE) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    
    // 如果缓冲区已满，直接返回false（丢弃数据）
    if (is_full()) {
        return false;
    }
    
    // 获取当前tail位置
    size_t current_tail = tail_.load(std::memory_order_acquire);
    
    // 计算块在缓冲区中的偏移
    size_t block_offset_bytes = current_tail * BLOCK_TOTAL_SIZE;
    
    // 写入头部到块的起始位置
    BlockHeader* header = reinterpret_cast<BlockHeader*>(&buffer_[block_offset_bytes]);
    header->source_ip = local_ip_;       // 使用绑定的本地IP
    header->source_port = local_port_;   // 使用绑定的本地端口
    header->data_length = static_cast<uint16_t>(len);
    
    // 计算数据部分的起始位置（头部之后）
    size_t data_offset_bytes = block_offset_bytes + HEADER_SIZE;
    unsigned char* data_ptr = &buffer_[data_offset_bytes];
    
    // 写入数据
    memcpy(data_ptr, data, len);
    
    // 标记块为有效
    block_valid_[current_tail] = true;
    
    // 更新tail指针（环形）
    size_t next_tail = (current_tail + 1) % block_count_;
    tail_.store(next_tail, std::memory_order_release);
    
    // 增加已使用块数
    used_blocks_.fetch_add(1, std::memory_order_release);
    
    return true;
}

bool SequentialUdpBuffer::mark_block_sent_completed() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    
    if (is_empty()) {
        return false;  // 没有数据可标记
    }
    
    // 获取当前head位置
    size_t current_head = head_.load(std::memory_order_acquire);
    
    // 标记这个块为无效（可重用）
    block_valid_[current_head] = false;
    
    // 更新head指针（环形）
    size_t next_head = (current_head + 1) % block_count_;
    head_.store(next_head, std::memory_order_release);
    
    // 减少已使用块数
    size_t old_used = used_blocks_.fetch_sub(1, std::memory_order_acq_rel);
    if (old_used <= 1) {
        used_blocks_.store(0, std::memory_order_release);
    }
    
    return true;
}

// ============ 接收缓冲区接口 ============
ssize_t SequentialUdpBuffer::read_block(void *buf, size_t nbytes,
                                        struct sockaddr *srcAddr, socklen_t *addrlen) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    
    // 如果没有数据，直接返回0
    if (is_empty()) {
        return 0;
    }
    
    // 获取当前head位置
    size_t current_head = head_.load(std::memory_order_acquire);
    
    // 计算块在缓冲区中的偏移
    size_t block_offset_bytes = current_head * BLOCK_TOTAL_SIZE;
    
    // 读取头部信息
    BlockHeader* header = reinterpret_cast<BlockHeader*>(&buffer_[block_offset_bytes]);
    
    // 检查块是否有效
    if (!block_valid_[current_head]) {
        return 0;  // 块无效
    }
    
    size_t data_len = header->data_length;
    
    // 如果提供的缓冲区太小，返回错误
    if (nbytes < data_len) {
        return -1;  // 缓冲区不足
    }
    
    // 计算数据部分的起始位置
    size_t data_offset_bytes = block_offset_bytes + HEADER_SIZE;
    unsigned char* data_ptr = &buffer_[data_offset_bytes];
    
    // 复制数据到用户缓冲区
    memcpy(buf, data_ptr, data_len);
    
    // 如果提供了srcAddr，填充源地址信息
    if (srcAddr != nullptr && addrlen != nullptr && *addrlen >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in* sin = reinterpret_cast<struct sockaddr_in*>(srcAddr);
        sin->sin_family = AF_INET;
        sin->sin_addr.s_addr = header->source_ip;
        sin->sin_port = header->source_port;
        *addrlen = sizeof(struct sockaddr_in);
    }
    
    // 标记块为无效（已读取）
    block_valid_[current_head] = false;
    
    // 更新head指针（环形）
    size_t next_head = (current_head + 1) % block_count_;
    head_.store(next_head, std::memory_order_release);
    
    // 减少已使用块数
    size_t old_used = used_blocks_.fetch_sub(1, std::memory_order_acq_rel);
    if (old_used <= 1) {
        used_blocks_.store(0, std::memory_order_release);
    }
    
    return static_cast<ssize_t>(data_len);
}

bool SequentialUdpBuffer::mark_block_received_completed() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    
    // 检查是否已满
    if (is_full()) {
        return false;  // 缓冲区已满，无法添加新块
    }
    
    // 获取当前tail位置
    size_t current_tail = tail_.load(std::memory_order_acquire);
    
    // 标记这个块为有效
    block_valid_[current_tail] = true;
    
    // 更新tail指针（环形）
    size_t next_tail = (current_tail + 1) % block_count_;
    tail_.store(next_tail, std::memory_order_release);
    
    // 增加已使用块数
    used_blocks_.fetch_add(1, std::memory_order_release);
    
    return true;
}

// ============ 直接内存访问（用于RDMA） ============
unsigned char* SequentialUdpBuffer::get_next_receive_ptr() {
    // 原子地获取并递增next_receive_index_
    size_t current_index = next_receive_index_.load(std::memory_order_acquire);
    
    // 计算块的起始地址
    size_t block_offset = current_index * BLOCK_TOTAL_SIZE;
    unsigned char* block_ptr = buffer_.data() + block_offset;
    
    // 递增next_receive_index_（环形）
    size_t next_index = (current_index + 1) % block_count_;
    next_receive_index_.store(next_index, std::memory_order_release);
    
    return block_ptr;
}


unsigned char* SequentialUdpBuffer::get_next_send_ptr() {
    // 发送缓冲区使用：获取下一个要发送数据的位置
    size_t current_head = head_.load(std::memory_order_acquire);
    return &buffer_[current_head * BLOCK_TOTAL_SIZE];
}

unsigned char* SequentialUdpBuffer::get_head_ptr() {
    size_t current_head = head_.load(std::memory_order_acquire);
    return &buffer_[current_head * BLOCK_TOTAL_SIZE];
}

unsigned char* SequentialUdpBuffer::get_tail_ptr() {
    size_t current_tail = tail_.load(std::memory_order_acquire);
    return &buffer_[current_tail * BLOCK_TOTAL_SIZE];
}

// ============ RDMA内存注册接口 ============
unsigned char* SequentialUdpBuffer::buffer_data() noexcept { 
    return buffer_.data(); 
}

const unsigned char* SequentialUdpBuffer::buffer_data() const noexcept { 
    return buffer_.data(); 
}

size_t SequentialUdpBuffer::buffer_size() const noexcept { 
    return buffer_.size(); 
}

// ============ 指针位置信息 ============
size_t SequentialUdpBuffer::get_head_index() const noexcept { 
    return head_.load(std::memory_order_acquire); 
}

size_t SequentialUdpBuffer::get_tail_index() const noexcept { 
    return tail_.load(std::memory_order_acquire); 
}

void SequentialUdpBuffer::clear() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    
    head_.store(0, std::memory_order_release);
    tail_.store(0, std::memory_order_release);
    used_blocks_.store(0, std::memory_order_release);
    
    // 重置所有块的有效标志
    std::fill(block_valid_.begin(), block_valid_.end(), false);
}