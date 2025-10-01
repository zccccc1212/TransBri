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
SoR_connection::SoR_connection(int fd /*fd is the key to find sor conn*/, int gidindex){
	m_fd = fd;
	m_gidindex = gidindex;// 144 : 4  ; 155 : 2;

	find_gid();
	resources_init();
	int rc = create_rdma_resources();
	if(rc == 1) // create rdma res success
	{

	}
	else{

	}

}

SoR_connection::~SoR_connection(){

}

void SoR_connection::resources_init(){
	memset(&m_res, 0 ,sizeof(m_res));
}


int SoR_connection::create_rdma_resources(){
	struct ibv_device **dev_list = NULL;
    struct ibv_qp_init_attr qp_init_attr;
    struct ibv_device *ib_dev = NULL;
    size_t size;
    //int i;
    int mr_flags = 0;
    int cq_size = 0;
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
	ib_dev = dev_list[1];

	/* get device handle */
    my_res.ib_ctx = ibv_open_device(ib_dev);
    if(!my_res.ib_ctx)
    {
        fprintf(stderr, "failed to open device \n");
        rc = 1;
        goto resources_create_exit;
    }

	ibv_free_device_list(dev_list);
	dev_list = NULL;
    ib_dev = NULL;

    /* query port properties */
    if(ibv_query_port(my_res.ib_ctx, 1, &my_res.port_attr))// 端口
    {
        fprintf(stderr, "ibv_query_port on port 0 failed\n");
        rc = 1;
        goto resources_create_exit;
    }


    /* allocate Protection Domain */
    my_res.pd = ibv_alloc_pd(my_res.ib_ctx);
    if(!my_res.pd)
    {
        fprintf(stderr, "ibv_alloc_pd failed\n");
        rc = 1;
        goto resources_create_exit;
    }

    /* each side will send only one WR, so Completion Queue with 1 entry is enough */
    cq_size = 1;
    my_res.cq = ibv_create_cq(my_res.ib_ctx, cq_size, NULL, NULL, 0);
    if(!my_res.cq)
    {
        fprintf(stderr, "failed to create CQ with %u entries\n", cq_size);
        rc = 1;
        goto resources_create_exit;
    }

    /* allocate the memory buffer that will hold the data */
	// recv buffer
    size = MR_SIZE;
    my_res.recv_buf = (char *) malloc(size);
    if(!my_res.recv_buf)
    {
        fprintf(stderr, "failed to malloc %Zu bytes to memory buffer\n", size);
        rc = 1;
        goto resources_create_exit;
    }
    memset(my_res.recv_buf, 0 , size);

    /* register the memory buffer */
    mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE ;
    my_res.recv_mr = ibv_reg_mr(my_res.pd, my_res.recv_buf, size, mr_flags);
	if(!my_res.recv_mr)
    {
        fprintf(stderr, "ibv_reg_mr failed with mr_flags=0x%x\n", mr_flags);
        rc = 1;
        goto resources_create_exit;
    }
	fprintf(stdout, "MR was registered with addr=%p, lkey=0x%x, rkey=0x%x, flags=0x%x\n",
            my_res.recv_buf, my_res.recv_mr->lkey, my_res.recv_mr->rkey, mr_flags);

	// send buffer
	/* allocate the memory buffer that will hold the data */
    size = MR_SIZE;
    my_res.send_buf = (char *) malloc(size);
    if(!my_res.send_buf)
    {
        fprintf(stderr, "failed to malloc %Zu bytes to memory buffer\n", size);
        rc = 1;
        goto resources_create_exit;
    }
    memset(my_res.send_buf, 0 , size);


    /* register the memory buffer */
    mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE ;
    my_res.send_mr = ibv_reg_mr(my_res.pd, my_res.send_buf, size, mr_flags);
	if(!my_res.send_mr)
    {
        fprintf(stderr, "ibv_reg_mr failed with mr_flags=0x%x\n", mr_flags);
        rc = 1;
        goto resources_create_exit;
    }
	fprintf(stdout, "MR was registered with addr=%p, lkey=0x%x, rkey=0x%x, flags=0x%x\n",
            my_res.send_buf, my_res.send_mr->lkey, my_res.send_mr->rkey, mr_flags);
	

	/* create the Queue Pair */
	memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.sq_sig_all = 1;
    qp_init_attr.send_cq = my_res.cq;
    qp_init_attr.recv_cq = my_res.cq;
    qp_init_attr.cap.max_send_wr = 1;//TODO : set to approperiate wr number
    qp_init_attr.cap.max_recv_wr = 1;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;
    my_res.qp = ibv_create_qp(my_res.pd, &qp_init_attr);

    if(!my_res.qp)
    {
        fprintf(stderr, "failed to create QP\n");
        rc = 1;
        goto resources_create_exit;
    }
    fprintf(stdout, "QP was created, QP number=0x%x\n", my_res.qp->qp_num);

resources_create_exit:
	if(rc)
    {
        /* Error encountered, cleanup */
        if(my_res.qp)
        {
            ibv_destroy_qp(my_res.qp);
            my_res.qp = NULL;
        }
		if(my_res.recv_mr)
        {
            ibv_dereg_mr(my_res.recv_mr);
            my_res.recv_mr = NULL;
        }
        if(my_res.send_mr)
        {
            ibv_dereg_mr(my_res.send_mr);
            my_res.send_mr = NULL;
        }
        if(my_res.recv_buf)
        {
            free(my_res.recv_buf);
            my_res.recv_buf = NULL;
        }
		if(my_res.send_buf)
        {
            free(my_res.send_buf);
            my_res.send_buf = NULL;
        }
        if(my_res.cq)
        {
            ibv_destroy_cq(my_res.cq);
            my_res.cq = NULL;
        }
        if(my_res.pd)
        {
            ibv_dealloc_pd(my_res.pd);
            my_res.pd = NULL;
        }
        if(my_res.ib_ctx)
        {
            ibv_close_device(my_res.ib_ctx);
            my_res.ib_ctx = NULL;
        }
        if(dev_list)
        {
            ibv_free_device_list(dev_list);
            dev_list = NULL;
        }
        if(my_res.sock >= 0)
        {
            if(close(my_res.sock))
            {
                fprintf(stderr, "failed to close socket\n");
            }
            my_res.sock = -1;
        }
    }
    return rc;
}

int SoR_connection::find_gid(){
	int rc = ibv_query_gid(my_res.ib_ctx, 1, m_gidindex, &my_gid);// 144
	if(rc)
    {
        fprintf(stderr, "could not get gid for port 1 index %d\n", gidindex);
    }
	return rc;
}

int SoR_connection::connect_to_reer(){

	struct cm_con_data_t local_con_data;
    struct cm_con_data_t remote_con_data;
    struct cm_con_data_t tmp_con_data;


	//prepare local rdma data to trans 
	local_con_data.addr = htonll((uintptr_t)my_res.recv_buf);
    local_con_data.rkey = htonl(my_res.recv_mr->rkey);
    local_con_data.qp_num = htonl(my_res.qp->qp_num);
    local_con_data.lid = htons(my_res.port_attr.lid);

	memcpy(local_con_data.gid, &my_gid, 16);
    fprintf(stdout, "\nLocal LID = 0x%x\n", my_res.port_attr.lid);

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
    memcpy(remote_con_data.gid, tmp_con_data.gid, 16);

	/* save the remote side attributes, we will need it for the post SR */
    my_res.remote_props = remote_con_data;
    //fprintf(stdout, "  Remote address = 0x%"PRIx64"\n", remote_con_data.addr);
    fprintf(stdout, " Remote rkey = 0x%x\n", remote_con_data.rkey);
    fprintf(stdout, " Remote QP number = 0x%x\n", remote_con_data.qp_num);
    fprintf(stdout, " Remote LID = 0x%x\n", remote_con_data.lid);

	int rc = modify_qp();
	if(rc){

	}

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

int SoR_connection::post_send(){

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





