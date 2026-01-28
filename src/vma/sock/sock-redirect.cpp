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


#include "sock-redirect.h"

//zc add
#include <infiniband/verbs.h>

#include <sys/time.h>
#include <dlfcn.h>
#include <iostream>
#include <fcntl.h>

#include "utils/compiler.h"
#include "utils/lock_wrapper.h"
#include <vma/proto/ip_frag.h>
#include <vma/dev/buffer_pool.h>
#include <vma/dev/ring_profile.h>

#include <vma/event/event_handler_manager.h>
#include <vma/event/vlogger_timer_handler.h>
#include <vma/iomux/poll_call.h>
#include <vma/iomux/select_call.h>
#include <vma/iomux/epfd_info.h>
#include <vma/iomux/epoll_wait_call.h>
#include <vma/util/sys_vars.h>
#include <vma/proto/route_table_mgr.h>
#include <vma/proto/vma_lwip.h>
#include <vma/main.h>
#include "vma/vma_extra.h"

#include <vma/sock/sockinfo_tcp.h>

#include "fd_collection.h"
#include "vma/util/instrumentation.h"

#include <vma/dev/ring.h>

using namespace std;





#define MODULE_NAME 		"srdr:"

#define srdr_logpanic		__log_panic
#define srdr_logerr		__log_err
#define srdr_logwarn		__log_warn
#define srdr_loginfo		__log_info
#define srdr_logdbg		__log_dbg
#define srdr_logfunc		__log_func
#define srdr_logfuncall		__log_funcall

#define srdr_logdbg_entry	__log_entry_dbg
#define srdr_logfunc_entry	__log_entry_func
#define srdr_logfuncall_entry	__log_entry_funcall

#define srdr_logdbg_exit	__log_exit_dbg
#define srdr_logfunc_exit	__log_exit_func

#define EP_MAX_EVENTS (int)((INT_MAX / sizeof(struct epoll_event)))

struct os_api orig_os_api;
struct sigaction g_act_prev;
sighandler_t g_sighandler = NULL;
class ring_simple;
class ring_eth_direct;


#define RECV_SIZE   1048576


template<typename T>
void assign_dlsym(T &ptr, const char *name) {
	ptr = reinterpret_cast<T>(dlsym(RTLD_NEXT, name));
}

#define FD_MAP_SIZE 		(g_p_fd_collection ? g_p_fd_collection->get_fd_map_size() : 1024)

#define DO_GLOBAL_CTORS() do { \
	int __res = do_global_ctors(); \
	if (__res) { \
		vlog_printf(VLOG_ERROR, "%s vma failed to start errno: %s\n", \
			__FUNCTION__, strerror(errno)); \
		if (safe_mce_sys().exception_handling == vma_exception_handling::MODE_EXIT) { \
			exit(-1); \
		} \
		return -1; \
	} \
} while (0)

#define GET_ORIG_FUNC(__name) \
	if (!orig_os_api.__name) { \
		dlerror(); \
		assign_dlsym(orig_os_api.__name, #__name); \
		const char* fcntlstr = "fcntl64"; \
		char *dlerror_str = dlerror(); \
		if (!orig_os_api.__name || dlerror_str) { \
			if (strcmp(fcntlstr, #__name) != 0) { \
				__log_warn("dlsym returned with error '%s' when looking for '%s'", \
			           (!dlerror_str? "":dlerror_str), #__name); \
			} \
			else { \
				__log_dbg("dlsym returned with error '%s' when looking for '%s'", \
					(!dlerror_str? "":dlerror_str), #__name); \
			} \
		} else { \
			__log_dbg("dlsym found %p for '%s()'", orig_os_api.__name , #__name); \
		} \
	}

#define SET_EXTRA_API(dst, func, mask) do { \
		vma_api->dst = func; \
		vma_api->vma_extra_supported_mask |= mask; \
} while(0);

#define VERIFY_PASSTROUGH_CHANGED(__ret, __func_and_params__) do { \
		bool passthrough = p_socket_object->isPassthrough(); \
		__ret = __func_and_params__; \
		if (!passthrough && p_socket_object->isPassthrough()) { \
			handle_close(__fd, false, true); \
		} \
} while(0);

void get_orig_funcs()
{
	// Save pointer to original functions
	GET_ORIG_FUNC(socket);
	GET_ORIG_FUNC(close);
	GET_ORIG_FUNC(close);
	GET_ORIG_FUNC(__res_iclose);
	GET_ORIG_FUNC(shutdown);
	GET_ORIG_FUNC(listen);
	GET_ORIG_FUNC(accept);
	GET_ORIG_FUNC(accept4);
	GET_ORIG_FUNC(bind);
	GET_ORIG_FUNC(connect);
	GET_ORIG_FUNC(setsockopt);
	GET_ORIG_FUNC(getsockopt);
	GET_ORIG_FUNC(fcntl);
	GET_ORIG_FUNC(fcntl64);
	GET_ORIG_FUNC(ioctl);
	GET_ORIG_FUNC(getsockname);
	GET_ORIG_FUNC(getpeername);
	GET_ORIG_FUNC(read);
	GET_ORIG_FUNC(__read_chk);
	GET_ORIG_FUNC(readv);
	GET_ORIG_FUNC(recv);
	GET_ORIG_FUNC(__recv_chk);
	GET_ORIG_FUNC(recvmsg);
	GET_ORIG_FUNC(recvmmsg);
	GET_ORIG_FUNC(recvfrom);
	GET_ORIG_FUNC(__recvfrom_chk);
	GET_ORIG_FUNC(write);
	GET_ORIG_FUNC(writev);
	GET_ORIG_FUNC(send);
	GET_ORIG_FUNC(sendmsg);
	GET_ORIG_FUNC(sendmmsg);
	GET_ORIG_FUNC(sendto);
	GET_ORIG_FUNC(sendfile);
	GET_ORIG_FUNC(sendfile64);
	GET_ORIG_FUNC(select);
	GET_ORIG_FUNC(pselect);
	GET_ORIG_FUNC(poll);
	GET_ORIG_FUNC(__poll_chk);
	GET_ORIG_FUNC(ppoll);
	GET_ORIG_FUNC(__ppoll_chk);
	GET_ORIG_FUNC(epoll_create);
	GET_ORIG_FUNC(epoll_create1);
	GET_ORIG_FUNC(epoll_ctl);
	GET_ORIG_FUNC(epoll_wait);
	GET_ORIG_FUNC(epoll_pwait);
	GET_ORIG_FUNC(socketpair);
	GET_ORIG_FUNC(pipe);
	GET_ORIG_FUNC(open);
	GET_ORIG_FUNC(creat);
	GET_ORIG_FUNC(dup);
	GET_ORIG_FUNC(dup2);
	GET_ORIG_FUNC(clone);
	GET_ORIG_FUNC(fork);
	GET_ORIG_FUNC(vfork);
	GET_ORIG_FUNC(daemon);
	GET_ORIG_FUNC(sigaction);
	GET_ORIG_FUNC(signal);
}

const char* socket_get_domain_str(int domain)
{
	switch (domain) {
	case AF_INET:		return "AF_INET";
	case AF_INET6:		return "AF_INET6";
	case AF_UNSPEC:		return "AF_UNSPEC";
	case AF_LOCAL:		return "AF_LOCAL";
	default:
		break;
	}
	return "";
}

const char* socket_get_type_str(int type)
{
	switch (type) {
	case SOCK_STREAM:	return "SOCK_STREAM";
	case SOCK_DGRAM:	return "SOCK_DGRAM";
	case SOCK_RAW:		return "SOCK_RAW";
	default:
		break;
	}
	return "";
}

// Format a sockaddr into a string for logging
char* sprintf_sockaddr(char* buf, int buflen, const struct sockaddr* _addr, socklen_t _addrlen)
{
	if ((_addrlen >= sizeof(struct sockaddr_in)) && (get_sa_family(_addr) == AF_INET)) {
		in_addr_t in_addr = get_sa_ipv4_addr(_addr);
		in_port_t in_port = get_sa_port(_addr);
		/* cppcheck-suppress wrongPrintfScanfArgNum */
		snprintf(buf, buflen, "AF_INET, addr=%d.%d.%d.%d, port=%d", NIPQUAD(in_addr), ntohs(in_port));
	}
	else {
		snprintf(buf, buflen, "sa_family=%d", get_sa_family(_addr));
	}
	return buf;
}

#define VMA_DBG_SEND_MCPKT_COUNTER_STR "VMA_DBG_SEND_MCPKT_COUNTER"
#define VMA_DBG_SEND_MCPKT_MCGROUP_STR "VMA_DBG_SEND_MCPKT_MCGROUP"
static int dbg_check_if_need_to_send_mcpkt_setting = -1; // 1-Init, 0-Disabled,  N>0-send mc packet on the Nth socket() call
static int dbg_check_if_need_to_send_mcpkt_counter = 1;
static int dbg_check_if_need_to_send_mcpkt_prevent_nested_calls = 0;

void dbg_send_mcpkt()
{
	int fd = 0;
	char *env_ptr = NULL;
	if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		vlog_printf(VLOG_WARNING, "send_mc_packet_test:%d: socket() errno %d %m", __LINE__, errno);
		exit(1);
	}

	struct sockaddr_in addr_in;
	struct sockaddr* p_addr = (struct sockaddr*)&addr_in;

	addr_in.sin_family = AF_INET;
	addr_in.sin_port = INPORT_ANY;
	addr_in.sin_addr.s_addr = INADDR_ANY;
	if ((env_ptr = getenv(VMA_DBG_SEND_MCPKT_MCGROUP_STR)) == NULL) {
		vlog_printf(VLOG_WARNING, "send_mc_packet_test:%d: Need to set '%s' parameter to dest ip (dot format)\n", __LINE__, VMA_DBG_SEND_MCPKT_MCGROUP_STR);
		exit(2);
	}
	if (1 != inet_pton(AF_INET, env_ptr, &addr_in.sin_addr)) {
		vlog_printf(VLOG_WARNING, "send_mc_packet_test:%d: Invalid input IP address: '%s' errno %d %m\n", __LINE__, env_ptr, errno);
		exit(3);
	}

	const char msgbuf[256] = "Hello Alex";

	vlog_printf(VLOG_WARNING, "send_mc_packet_test:%d: Sending MC test packet to address: %d.%d.%d.%d [%s]\n", __LINE__, NIPQUAD(get_sa_ipv4_addr(p_addr)), VMA_DBG_SEND_MCPKT_MCGROUP_STR);
	if (sendto(fd, msgbuf, strlen(msgbuf), 0, p_addr, sizeof(struct sockaddr)) < 0)
		vlog_printf(VLOG_ERROR, "sendto mc_packet failed! errno %d %s\n", errno, strerror(errno));
	close(fd);
}

void dbg_check_if_need_to_send_mcpkt()
{
	if (dbg_check_if_need_to_send_mcpkt_prevent_nested_calls)
		return;
	dbg_check_if_need_to_send_mcpkt_prevent_nested_calls = 1;

	// Read user setting
	if (dbg_check_if_need_to_send_mcpkt_setting == -1) {
		// Default will be 'Disbaled'
		dbg_check_if_need_to_send_mcpkt_setting++;

		// Then we will read the user settings
		char *env_ptr = NULL;
		if ((env_ptr = getenv(VMA_DBG_SEND_MCPKT_COUNTER_STR)) != NULL) {
			dbg_check_if_need_to_send_mcpkt_setting = atoi(env_ptr);
		}
		if (dbg_check_if_need_to_send_mcpkt_setting > 0) {
			vlog_printf(VLOG_WARNING, "send_mc_packet_test: *************************************************************\n");
			vlog_printf(VLOG_WARNING, "send_mc_packet_test: Send test MC packet setting is: %d [%s]\n", dbg_check_if_need_to_send_mcpkt_setting, VMA_DBG_SEND_MCPKT_COUNTER_STR);
			vlog_printf(VLOG_WARNING, "send_mc_packet_test: If you don't know what this means don't use '%s' VMA configuration parameter!\n", VMA_DBG_SEND_MCPKT_COUNTER_STR);
			vlog_printf(VLOG_WARNING, "send_mc_packet_test: *************************************************************\n");
		}
	}

	// Test for action
	if (dbg_check_if_need_to_send_mcpkt_setting > 0) {
		if (dbg_check_if_need_to_send_mcpkt_counter == dbg_check_if_need_to_send_mcpkt_setting)
		{
			// Actual send mc packet
			dbg_send_mcpkt();
		}
		else {
			vlog_printf(VLOG_WARNING, "send_mc_packet_test:%d: Skipping this socket() call\n", __LINE__);
		}
		dbg_check_if_need_to_send_mcpkt_counter++;
	}
	dbg_check_if_need_to_send_mcpkt_prevent_nested_calls--;
}

void handle_close(int fd, bool cleanup, bool passthrough)
{
	
	srdr_logfunc("Cleanup fd=%d", fd);

	if (g_p_fd_collection) {
		// Remove fd from all existing epoll sets
		g_p_fd_collection->remove_from_all_epfds(fd, passthrough);

		if (fd_collection_get_sockfd(fd)) {
			g_p_fd_collection->del_sockfd(fd, cleanup);
		}
		if (fd_collection_get_epfd(fd)) {
			g_p_fd_collection->del_epfd(fd, cleanup);
		}

	}
}


//-----------------------------------------------------------------------------
// extended API functions
//-----------------------------------------------------------------------------

extern "C"
int vma_register_recv_callback(int __fd, vma_recv_callback_t __callback, void *__context)
{
	srdr_logfunc_entry("fd=%d", __fd);

	socket_fd_api* p_socket_object = NULL;
	p_socket_object = fd_collection_get_sockfd(__fd);
	if (p_socket_object) {
		p_socket_object->register_callback(__callback, __context);
		return 0;
	}
	errno = EINVAL;
	return -1;
}

extern "C"
int vma_recvfrom_zcopy(int __fd, void *__buf, size_t __nbytes, int *__flags,
	               struct sockaddr *__from, socklen_t *__fromlen)
{
	srdr_logfuncall_entry("fd=%d", __fd);

	socket_fd_api* p_socket_object = NULL;
	p_socket_object = fd_collection_get_sockfd(__fd);
	if (p_socket_object) {
		struct iovec piov[1];
		piov[0].iov_base = __buf;
		piov[0].iov_len = __nbytes;
		*__flags |= MSG_VMA_ZCOPY;
		return p_socket_object->rx(RX_RECVFROM, piov, 1, __flags, __from, __fromlen);

	}
	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.recvfrom) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END
	return orig_os_api.recvfrom(__fd, __buf, __nbytes, *__flags, __from, __fromlen);
}

extern "C"
int vma_free_packets(int __fd, struct vma_packet_t *pkts, size_t count)
{
	socket_fd_api* p_socket_object = NULL;
	p_socket_object = fd_collection_get_sockfd(__fd);
	if (p_socket_object) {
		return p_socket_object->free_packets(pkts, count);
	}

	errno = EINVAL;
	return -1;
}

static int dummy_vma_socketxtreme_poll(int fd, struct vma_completion_t* completions, unsigned int ncompletions, int flags)
{
	NOT_IN_USE(fd);
        NOT_IN_USE(completions);
        NOT_IN_USE(ncompletions);
        NOT_IN_USE(flags);
	VLOG_PRINTF_ONCE_THEN_ALWAYS(VLOG_WARNING, VLOG_DEBUG,
			"socketXtreme was not enabled during runtime. Set %s to use. Ignoring...", SYS_VAR_SOCKETXTREME);
	errno = EOPNOTSUPP;
	return -1;
}

extern "C"
int vma_socketxtreme_poll(int fd, struct vma_completion_t* completions, unsigned int ncompletions, int flags)
{
	int ret_val = -1;
	cq_channel_info* cq_ch_info = NULL;

	cq_ch_info = g_p_fd_collection->get_cq_channel_fd(fd);

	if (likely(cq_ch_info)) {
		ring* p_ring = cq_ch_info->get_ring();

		ret_val = p_ring->socketxtreme_poll(completions, ncompletions, flags);
#ifdef RDTSC_MEASURE_RX_PROCCESS_BUFFER_TO_RECIVEFROM
	RDTSC_TAKE_END(g_rdtsc_instr_info_arr[RDTSC_FLOW_PROCCESS_RX_BUFFER_TO_RECIVEFROM]);
#endif //RDTSC_MEASURE_RX_PROCCESS_BUFFER_TO_RECIVEFROM

#ifdef RDTSC_MEASURE_RX_LWIP_TO_RECEVEFROM
	RDTSC_TAKE_END(g_rdtsc_instr_info_arr[RDTSC_FLOW_RX_LWIP_TO_RECEVEFROM]);
#endif //RDTSC_MEASURE_RX_LWIP_TO_RECEVEFROM

#ifdef RDTSC_MEASURE_RX_CQE_RECEIVEFROM
	RDTSC_TAKE_END(g_rdtsc_instr_info_arr[RDTSC_FLOW_RX_CQE_TO_RECEIVEFROM]);
#endif //RDTSC_MEASURE_RX_CQE_RECEIVEFROM

#ifdef RDTSC_MEASURE_RECEIVEFROM_TO_SENDTO
	RDTSC_TAKE_START(g_rdtsc_instr_info_arr[RDTSC_FLOW_RECEIVEFROM_TO_SENDTO]);
#endif //RDTSC_MEASURE_RECEIVEFROM_TO_SENDTO
	return ret_val;
	}
	else {
		errno = EBADFD;
		return ret_val;
	}
}

static int dummy_vma_socketxtreme_free_vma_packets(struct vma_packet_desc_t *packets, int num)
{
	NOT_IN_USE(packets);
	NOT_IN_USE(num);
	VLOG_PRINTF_ONCE_THEN_ALWAYS(VLOG_WARNING, VLOG_DEBUG,
			"socketXtreme was not enabled during runtime. Set %s to use. Ignoring...", SYS_VAR_SOCKETXTREME);
	errno = EOPNOTSUPP;
	return -1;
}

extern "C"
int vma_socketxtreme_free_vma_packets(struct vma_packet_desc_t *packets, int num)
{
	mem_buf_desc_t* desc = NULL;
	socket_fd_api* p_socket_object = NULL;

	if (likely(packets)) {
		for (int i = 0; i < num; i++) {
			desc = (mem_buf_desc_t*)packets[i].buff_lst;
			if (desc) {
				p_socket_object = (socket_fd_api*)desc->rx.context;
				ring_slave* rng = desc->p_desc_owner;
				if (p_socket_object) {
					p_socket_object->free_buffs(packets[i].total_len);
				}
				if (rng) {
					rng->reclaim_recv_buffers(desc);
				} else {
					goto err;
				}
			} else {
				goto err;
			}
		}
	}
	else {
		goto err;
	}

	return 0;

err:
	errno = EINVAL;
	return -1;
}

static int dummy_vma_socketxtreme_ref_vma_buff(vma_buff_t *buff)
{
	NOT_IN_USE(buff);
	VLOG_PRINTF_ONCE_THEN_ALWAYS(VLOG_WARNING, VLOG_DEBUG,
			"socketXtreme was not enabled during runtime. Set %s to use. Ignoring...", SYS_VAR_SOCKETXTREME);
	errno = EOPNOTSUPP;
	return -1;
}

extern "C"
int vma_socketxtreme_ref_vma_buff(vma_buff_t *buff)
{
	int ret_val = 0;
	mem_buf_desc_t* desc = NULL;

	if (likely(buff)) {
		desc = (mem_buf_desc_t*)buff;
		ret_val = desc->lwip_pbuf_inc_ref_count();
	}
	else {
		errno = EINVAL;
		ret_val = -1;
	}
	return ret_val;
}

static int dummy_vma_socketxtreme_free_vma_buff(vma_buff_t *buff)
{
	NOT_IN_USE(buff);
	VLOG_PRINTF_ONCE_THEN_ALWAYS(VLOG_WARNING, VLOG_DEBUG,
			"socketXtreme was not enabled during runtime. Set %s to use. Ignoring...", SYS_VAR_SOCKETXTREME);
	errno = EOPNOTSUPP;
	return -1;
}

extern "C"
int vma_socketxtreme_free_vma_buff(vma_buff_t *buff)
{
	int ret_val = 0;
	mem_buf_desc_t* desc = NULL;

	if (likely(buff)) {
		desc = (mem_buf_desc_t*)buff;
		ring_slave* rng = desc->p_desc_owner;
		ret_val = rng->reclaim_recv_single_buffer(desc);
	}
	else {
		errno = EINVAL;
		ret_val = -1;
	}
	return ret_val;
}

extern "C"
int vma_get_socket_rings_num(int fd)
{
	socket_fd_api* p_socket_object = NULL;
	p_socket_object = fd_collection_get_sockfd(fd);
	if (p_socket_object && p_socket_object->check_rings()) {
		return p_socket_object->get_rings_num();
	}

	return 0;
}

extern "C"
int vma_get_socket_rings_fds(int fd, int *ring_fds, int ring_fds_sz)
{
	int* p_rings_fds = NULL;
	socket_fd_api* p_socket_object = NULL;
	int rings_num = 0;
	
	if (ring_fds_sz <= 0 || ring_fds == NULL) {
        	errno = EINVAL;
        	return -1;
	}
	p_socket_object = fd_collection_get_sockfd(fd);
	if (p_socket_object && p_socket_object->check_rings()) {
		p_rings_fds = p_socket_object->get_rings_fds(rings_num);
		for (int i = 0; i < min(ring_fds_sz, rings_num); i++) {
			ring_fds[i] = p_rings_fds[i];
		}
	}

	return min(ring_fds_sz, rings_num);
}

extern "C"
int vma_get_socket_tx_ring_fd(int sock_fd, struct sockaddr *to, socklen_t tolen)
{
	socket_fd_api* p_socket_object = fd_collection_get_sockfd(sock_fd);

	if (!p_socket_object) {
		errno = EINVAL;
		return -1;
	}
	return p_socket_object->get_socket_tx_ring_fd(to, tolen);
}

extern "C"
int vma_add_conf_rule(const char *config_line)
{
	srdr_logdbg("adding conf rule: %s", config_line);

	int ret = __vma_parse_config_line(config_line);

	if (*g_p_vlogger_level >= VLOG_DEBUG)
		__vma_print_conf_file(__instance_list);

	return ret;
}

extern "C"
int vma_thread_offload(int offload, pthread_t tid)
{
	if (g_p_fd_collection) {
		g_p_fd_collection->offloading_rule_change_thread(offload, tid);
	} else {
		return -1;
	}

	return 0;
}

extern "C"
int vma_dump_fd_stats(int fd, int log_level)
{
	if (g_p_fd_collection) {
		g_p_fd_collection->statistics_print(fd, log_level::from_int(log_level));
		return 0;
	}
	return -1;
}

extern "C"
int vma_add_ring_profile(vma_ring_type_attr *profile, vma_ring_profile_key *res)
{
	if (!g_p_ring_profile) {
		vlog_printf(VLOG_DEBUG, "%s g_p_ring_profile is null\n",__func__);
		return -1;
	}
	*res = g_p_ring_profile->add_profile(profile);
	return 0;
}

extern "C"
int vma_modify_ring(struct vma_modify_ring_attr *mr_data)
{
	srdr_logfunc_entry("ring_fd=%d, mr_data=%p ", mr_data->ring_fd, mr_data);
	int ret = -1;
	cq_channel_info* p_cq_ch_info = g_p_fd_collection->get_cq_channel_fd(mr_data->ring_fd);
	if (likely(p_cq_ch_info)) {
		ring_simple* p_ring = dynamic_cast<ring_simple*>(p_cq_ch_info->get_ring());
		if (likely(p_ring)) {
			if (VMA_MODIFY_RING_CQ_ARM & mr_data->comp_bit_mask) {
				if (RING_ETH_DIRECT == p_ring->get_type()) {
					ret = p_ring->ack_and_arm_cq(CQT_TX);
				} else {
					vlog_printf(VLOG_ERROR, "Ring type [%d] is not supported\n",
							p_ring->get_type());
				}
			} else if (VMA_MODIFY_RING_CQ_MODERATION & mr_data->comp_bit_mask) {
				p_ring->modify_cq_moderation(mr_data->cq_moderation.cq_moderation_period_usec,
						mr_data->cq_moderation.cq_moderation_count);
				ret = 0;
			} else {
				vlog_printf(VLOG_ERROR, "comp_mask [0x%x] is not supported\n",
						mr_data->comp_bit_mask);
			}
		} else {
			vlog_printf(VLOG_ERROR, "could not find ring_simple,"
					" got fd %d\n", mr_data->ring_fd);
		}
	} else {
		vlog_printf(VLOG_ERROR, "could not find p_cq_ch_info, got fd "
							"%d\n", mr_data->ring_fd);
	}

	return ret;
}

extern "C"
int vma_get_socket_netowrk_header(int __fd, void *ptr, uint16_t *len)
{
	srdr_logdbg_entry("fd=%d, ptr=%p len=%d", __fd, ptr, *len);

	socket_fd_api* p_socket_object = fd_collection_get_sockfd(__fd);

	if (p_socket_object) {
		return p_socket_object->get_socket_network_ptr(ptr, *len);
	}
	errno = EINVAL;
	return -1;
}

extern "C"
int vma_get_ring_direct_descriptors(int __fd,
				    struct vma_mlx_hw_device_data *data)
{
	srdr_logdbg_entry("fd=%d, ptr=%p ", __fd, data);

	cq_channel_info* p_cq_ch_info = g_p_fd_collection->get_cq_channel_fd(__fd);
	if (p_cq_ch_info) {
		ring_simple* p_ring = dynamic_cast<ring_simple *>(p_cq_ch_info->get_ring());
		if (likely(p_ring)) {
			return p_ring->get_ring_descriptors(*data);
		} else {
			vlog_printf(VLOG_ERROR, "could not find ring_simple,"
					" got fd %d\n", __fd);
			return -1;
		}
	} else {
		vlog_printf(VLOG_ERROR, "could not find p_cq_ch_info, got fd "
							"%d\n", __fd);
		return -1;
	}
}

extern "C"
int vma_reg_mr_on_ring(int __fd, void *addr, size_t length, uint32_t *lkey)
{
	srdr_logdbg_entry("fd=%d, addr=%p length %zd key %p", __fd, addr, length, lkey);

	if (!lkey) {
		vlog_printf(VLOG_DEBUG, "key is null fd %d, addr %p, length %zd\n",
				__fd, addr, length);
		errno = EINVAL;
		return -1;
	}
	cq_channel_info* p_cq_ch_info = g_p_fd_collection->get_cq_channel_fd(__fd);
	if (p_cq_ch_info) {
		ring* p_ring = p_cq_ch_info->get_ring();
		if (likely(p_ring)) {
			return p_ring->reg_mr(addr, length, *lkey);
		} else {
			vlog_printf(VLOG_ERROR, "could not find ring, got fd "
					"%d\n", __fd);
			return -1;
		}
	} else {
		vlog_printf(VLOG_ERROR, "could not find p_cq_ch_info, got fd "
							"%d\n", __fd);
		return -1;
	}
}

extern "C"
int vma_dereg_mr_on_ring(int __fd, void *addr, size_t length)
{
	srdr_logdbg_entry("fd=%d, addr=%p ", __fd, addr);

	cq_channel_info* p_cq_ch_info = g_p_fd_collection->get_cq_channel_fd(__fd);
	if (p_cq_ch_info) {
		ring* p_ring = p_cq_ch_info->get_ring();
		if (likely(p_ring)) {
			return p_ring->dereg_mr(addr, length);
		} else {
			vlog_printf(VLOG_ERROR, "could not find ring, got fd "
					"%d\n", __fd);
			return -1;
		}
	} else {
		vlog_printf(VLOG_ERROR, "could not find p_cq_ch_info, got fd "
							"%d\n", __fd);
		return -1;
	}
}

extern "C"
int vma_get_dpcp_devices(uintptr_t **devices, size_t *devices_num)
{
#ifdef DEFINED_DPCP
	ib_context_map_t *ib_ctx_map = NULL;
	ib_ctx_handler *p_ib_ctx_h = NULL;
	size_t found_devs = 0;

	if (!devices_num) {
		return EINVAL;
	}

	ib_ctx_map = g_p_ib_ctx_handler_collection->get_ib_cxt_list();
	if (ib_ctx_map) {
		ib_context_map_t::iterator iter;

		for (iter = ib_ctx_map->begin(); iter != ib_ctx_map->end(); iter++) {
			p_ib_ctx_h = iter->second;
			if (p_ib_ctx_h->get_dpcp_adapter()) {
				if (devices && (found_devs < *devices_num)) {
					devices[found_devs] = (uintptr_t*)p_ib_ctx_h->get_dpcp_adapter();
				}
				found_devs++;
			}
		}
	}

	*devices_num = found_devs;
	srdr_logdbg_entry("returned %zd devices", found_devs);

	return 0;
#else
	NOT_IN_USE(devices);
	NOT_IN_USE(devices_num);
	VLOG_PRINTF_ONCE_THEN_ALWAYS(VLOG_WARNING, VLOG_DEBUG,
			"vma_get_dpcp_devices is no supported");
	errno = EOPNOTSUPP;
	return -1;
#endif /* DEFINED_DPCP */
}

//-----------------------------------------------------------------------------
//  replacement functions
//-----------------------------------------------------------------------------

/* Create a new socket of type TYPE in domain DOMAIN, using
   protocol PROTOCOL.  If PROTOCOL is zero, one is chosen automatically.
   Returns a file descriptor for the new socket, or -1 for errors.  */
extern "C"
EXPORT_SYMBOL
int socket(int __domain, int __type, int __protocol)
{
	return socket_internal(__domain, __type, __protocol, true);
}

// allow calling our socket(...) implementation safely from within libvma.so
// this is critical in case VMA was loaded using dlopen and not using LD_PRELOAD
// TODO: look for additional such functions/calls
int socket_internal(int __domain, int __type, int __protocol, bool check_offload /*= false*/)
{
	bool offload_sockets = (__type & 0xf) == SOCK_DGRAM || (__type & 0xf) == SOCK_STREAM;

	if (offload_sockets)
		DO_GLOBAL_CTORS();

	dbg_check_if_need_to_send_mcpkt();

	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.socket) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END    

	int fd = orig_os_api.socket(__domain, __type, __protocol);


	vlog_printf(VLOG_DEBUG, "ENTER: %s(domain=%s(%d), type=%s(%d), protocol=%d) = %d\n",__func__, socket_get_domain_str(__domain), __domain, socket_get_type_str(__type), __type, __protocol, fd);
	if (fd < 0) {
		return fd;
	}
	// zc add
	int ret = 0;

	if(my_g_p_fd_collection){
		ret = my_g_p_fd_collection->add_socketfd(fd, __type);
	}
	else{
		printf("my_g_p_fd_collection is null \n");
		return fd;
	}

	if(ret == 1)//add sockfd succeed
	{
		printf("add sockfd succeed\n");
		return fd;
	}

	return fd;


	if (g_p_fd_collection) {
		// Sanity check to remove any old sockinfo object using the same fd!!
		handle_close(fd, true);

		// Create new sockinfo object for this new socket
		if (offload_sockets)
			g_p_fd_collection->addsocket(fd, __domain, __type, check_offload);
	}

	return fd;
}

extern "C"
EXPORT_SYMBOL
int close(int __fd)
{
	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.close) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	srdr_logdbg_entry("fd=%d", __fd);

	handle_close(__fd);

	return orig_os_api.close(__fd);
}

extern "C"
EXPORT_SYMBOL
void __res_iclose(res_state statp, bool free_addr)
{
	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.__res_iclose) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	srdr_logdbg_entry("");
	for (int ns = 0; ns < statp->_u._ext.nscount; ns++) {
		int sock = statp->_u._ext.nssocks[ns];
		if (sock != -1) {
			handle_close(sock);
		}
	}
	orig_os_api.__res_iclose(statp, free_addr);
}

/* Shut down all or part of the connection open on socket FD.
   HOW determines what to shut down:
     SHUT_RD   = No more receptions;
     SHUT_WR   = No more transmissions;
     SHUT_RDWR = No more receptions or transmissions.
   Returns 0 on success, -1 for errors.  */
extern "C"
EXPORT_SYMBOL
int shutdown(int __fd, int __how)
{
	srdr_logdbg_entry("fd=%d, how=%d", __fd, __how);

	socket_fd_api* p_socket_object = NULL;
	p_socket_object = fd_collection_get_sockfd(__fd);
	if (p_socket_object)
		return p_socket_object->shutdown(__how);

	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.shutdown) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	return orig_os_api.shutdown(__fd, __how);
}

extern "C"
EXPORT_SYMBOL
int listen(int __fd, int backlog)
{
	srdr_logdbg_entry("fd=%d, backlog=%d", __fd, backlog);




	
		// zc add

	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.listen) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	int ret = orig_os_api.listen(__fd, backlog);

	Socket_transbridge* p_socket = NULL;
	p_socket = my_fd_collection_get_sockfd(__fd);

	if(p_socket){
		p_socket->listen(backlog);
	}
	return ret;


	socket_fd_api* p_socket_object = NULL;
	p_socket_object = fd_collection_get_sockfd(__fd);

	if (p_socket_object) {
		ret = p_socket_object->prepareListen(); // for verifying that the socket is really offloaded
		if (ret < 0)
			return ret; //error
		if (ret > 0) { //Passthrough
			handle_close(__fd, false, true);
			p_socket_object = NULL;
		}
	}
	if (p_socket_object) {
		return p_socket_object->listen(backlog);
	}

	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.listen) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	return orig_os_api.listen(__fd, backlog);
}

extern "C"
EXPORT_SYMBOL
int accept(int __fd, struct sockaddr *__addr, socklen_t *__addrlen)
{
	//zc add

	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.accept) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	int myfd = orig_os_api.accept(__fd, __addr, __addrlen);

	if(my_g_p_fd_collection)
		my_g_p_fd_collection->add_socketfd(myfd, SOCK_STREAM);

	Socket_transbridge* p_socket = NULL;
	p_socket = my_fd_collection_get_sockfd(myfd);
	if(p_socket){
		p_socket->accept();
	}

	return myfd;

//


	socket_fd_api* p_socket_object = NULL;
	p_socket_object = fd_collection_get_sockfd(__fd);
	if (p_socket_object)
		return p_socket_object->accept(__addr, __addrlen);

	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.accept) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	return orig_os_api.accept(__fd, __addr, __addrlen);
}

extern "C"
EXPORT_SYMBOL
int accept4(int __fd, struct sockaddr *__addr, socklen_t *__addrlen, int __flags)
{
	socket_fd_api* p_socket_object = NULL;
	p_socket_object = fd_collection_get_sockfd(__fd);
	if (p_socket_object)
		return p_socket_object->accept4(__addr, __addrlen, __flags);

	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.accept4) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	return orig_os_api.accept4(__fd, __addr, __addrlen, __flags);
}

/* Give the socket FD the local address ADDR (which is LEN bytes long).  */
extern "C"
EXPORT_SYMBOL
int bind(int __fd, const struct sockaddr *__addr, socklen_t __addrlen)
{
	int errno_tmp = errno;

	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.bind) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	// zc add
	int ret = orig_os_api.bind(__fd, __addr, __addrlen);

	Socket_transbridge* p_socket = NULL;
	p_socket = my_fd_collection_get_sockfd(__fd);
	if(p_socket){
		p_socket->bind();
	}
	
	return ret;
	//




	char buf[256];
	NOT_IN_USE(buf); /* to suppress warning in case VMA_MAX_DEFINED_LOG_LEVEL */
	srdr_logdbg_entry("fd=%d, %s", __fd, sprintf_sockaddr(buf, 256, __addr, __addrlen));

	ret = 0;
	socket_fd_api* p_socket_object = NULL;
	p_socket_object = fd_collection_get_sockfd(__fd);
	if (p_socket_object) {
		ret = p_socket_object->bind(__addr, __addrlen);
		if (p_socket_object->isPassthrough()) {
			handle_close(__fd, false, true);
			if (ret) {
				ret = orig_os_api.bind(__fd, __addr, __addrlen);
			}
		}
	}
	else {
		ret = orig_os_api.bind(__fd, __addr, __addrlen);
	}

	if (ret >= 0) {
		/* Restore errno on function entry in case success */
		errno = errno_tmp;
		srdr_logdbg_exit("returned with %d", ret);
	} else {
		srdr_logdbg_exit("failed (errno=%d %m)", errno);
	}

	return ret;
}

/* Open a connection on socket FD to peer at ADDR (which LEN bytes long).
   For connectionless socket types, just set the default address to send to
   and the only address from which to accept transmissions.
   Return 0 on success, -1 for errors.

   This function is a cancellation point and therefore not marked with
   __THROW.  */
extern "C"
EXPORT_SYMBOL
int connect(int __fd, const struct sockaddr *__to, socklen_t __tolen)
{


	// zc add
	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.connect) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END
	int ret = orig_os_api.connect(__fd, __to, __tolen);

	Socket_transbridge* p_socket = NULL;
	p_socket = my_fd_collection_get_sockfd(__fd);
	if(p_socket){
		p_socket->connect();
	}
	//printf("Contents of server's buffer: '%s'\n", res.buf);

	return ret;


	int errno_tmp = errno;

	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.connect) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	char buf[256];
	NOT_IN_USE(buf); /* to suppress warning in case VMA_MAX_DEFINED_LOG_LEVEL */
	srdr_logdbg_entry("fd=%d, %s", __fd, sprintf_sockaddr(buf, 256, __to, __tolen));

	ret = 0;
	socket_fd_api* p_socket_object = NULL;
	p_socket_object = fd_collection_get_sockfd(__fd);

	/* Zero port is reserved and can not be used as destination port
	 * but it is not checked here to be consistent with Linux behaivour
	 */
	if (__to && (get_sa_family(__to) == AF_INET) && p_socket_object) {
		ret = p_socket_object->connect(__to, __tolen);
		if (p_socket_object->isPassthrough()) {
			handle_close(__fd, false, true);
			if (ret) {
				ret = orig_os_api.connect(__fd, __to, __tolen);
			}
		}
	}
	else {
		if (p_socket_object) {
			p_socket_object->setPassthrough();
		}
		ret = orig_os_api.connect(__fd, __to, __tolen);
	}

	if (ret >= 0) {
		/* Restore errno on function entry in case success */
		errno = errno_tmp;
		srdr_logdbg_exit("returned with %d", ret);
	} else {
		srdr_logdbg_exit("failed (errno=%d %m)", errno);
	}

	return ret;
}

/* Set socket FD's option OPTNAME at protocol level LEVEL
   to *OPTVAL (which is OPTLEN bytes long).
   Returns 0 on success, -1 for errors.  */
extern "C"
EXPORT_SYMBOL
int setsockopt(int __fd, int __level, int __optname,
	       __const void *__optval, socklen_t __optlen)
{

	srdr_logdbg_entry("fd=%d, level=%d, optname=%d", __fd, __level, __optname);
        
    if (NULL == __optval) {
        errno = EFAULT;
        return -1;
    }
	
	// zc add
	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.setsockopt) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	Socket_transbridge* p_socket = NULL;
	p_socket = my_fd_collection_get_sockfd(__fd);
	if(p_socket){
		return p_socket->setsockopt(__level, __optname, __optval, __optlen);
	}

	int ret = 0;
	socket_fd_api* p_socket_object = NULL;

	p_socket_object = fd_collection_get_sockfd(__fd);
	if (p_socket_object) {
		VERIFY_PASSTROUGH_CHANGED(ret, p_socket_object->setsockopt(__level, __optname, __optval, __optlen));
	}
	else {
		BULLSEYE_EXCLUDE_BLOCK_START
		if (!orig_os_api.setsockopt) get_orig_funcs();
		BULLSEYE_EXCLUDE_BLOCK_END
		ret = orig_os_api.setsockopt(__fd, __level, __optname, __optval, __optlen);
	}

	if (ret >= 0)
		srdr_logdbg_exit("returned with %d", ret);
	else
		srdr_logdbg_exit("failed (errno=%d %m)", errno);
	return ret;
}

/* Get socket FD's option OPTNAME at protocol level LEVEL
   to *OPTVAL (which is OPTLEN bytes long).
   Returns 0 on success, -1 for errors.  */
extern "C"
EXPORT_SYMBOL
int getsockopt(int __fd, int __level, int __optname,
	       void *__optval, socklen_t *__optlen)
{
	srdr_logdbg_entry("fd=%d, level=%d, optname=%d", __fd, __level, __optname);

	if (__fd == -1 && __level == SOL_SOCKET && __optname == SO_VMA_GET_API &&
	    __optlen && *__optlen >= sizeof(struct vma_api_t*)) {
		DO_GLOBAL_CTORS();
		bool enable_socketxtreme = safe_mce_sys().enable_socketxtreme;
		srdr_logdbg("User request for VMA Extra API pointers");
		struct vma_api_t *vma_api = new struct vma_api_t();

		vma_api->vma_extra_supported_mask = 0;
		SET_EXTRA_API(register_recv_callback, vma_register_recv_callback, VMA_EXTRA_API_REGISTER_RECV_CALLBACK);
		SET_EXTRA_API(recvfrom_zcopy, vma_recvfrom_zcopy, VMA_EXTRA_API_RECVFROM_ZCOPY);
		SET_EXTRA_API(free_packets, vma_free_packets, VMA_EXTRA_API_FREE_PACKETS);
		SET_EXTRA_API(add_conf_rule, vma_add_conf_rule, VMA_EXTRA_API_ADD_CONF_RULE);
		SET_EXTRA_API(thread_offload, vma_thread_offload, VMA_EXTRA_API_THREAD_OFFLOAD);
		SET_EXTRA_API(get_socket_rings_num, vma_get_socket_rings_num, VMA_EXTRA_API_GET_SOCKET_RINGS_NUM);
		SET_EXTRA_API(get_socket_rings_fds, vma_get_socket_rings_fds, VMA_EXTRA_API_GET_SOCKET_RINGS_FDS);
		SET_EXTRA_API(get_socket_tx_ring_fd, vma_get_socket_tx_ring_fd, VMA_EXTRA_API_GET_SOCKET_TX_RING_FD);
		SET_EXTRA_API(vma_add_ring_profile, vma_add_ring_profile, VMA_EXTRA_API_ADD_RING_PROFILE);
		SET_EXTRA_API(get_socket_network_header, vma_get_socket_netowrk_header, VMA_EXTRA_API_GET_SOCKET_NETWORK_HEADER);
		SET_EXTRA_API(get_ring_direct_descriptors, vma_get_ring_direct_descriptors, VMA_EXTRA_API_GET_RING_DIRECT_DESCRIPTORS);
		SET_EXTRA_API(register_memory_on_ring, vma_reg_mr_on_ring, VMA_EXTRA_API_REGISTER_MEMORY_ON_RING);
		SET_EXTRA_API(deregister_memory_on_ring, vma_dereg_mr_on_ring, VMA_EXTRA_API_DEREGISTER_MEMORY_ON_RING);
		SET_EXTRA_API(socketxtreme_free_vma_packets, enable_socketxtreme ? vma_socketxtreme_free_vma_packets : dummy_vma_socketxtreme_free_vma_packets, VMA_EXTRA_API_SOCKETXTREME_FREE_VMA_PACKETS);
		SET_EXTRA_API(socketxtreme_poll, enable_socketxtreme ? vma_socketxtreme_poll : dummy_vma_socketxtreme_poll, VMA_EXTRA_API_SOCKETXTREME_POLL);
		SET_EXTRA_API(socketxtreme_ref_vma_buff, enable_socketxtreme ? vma_socketxtreme_ref_vma_buff : dummy_vma_socketxtreme_ref_vma_buff, VMA_EXTRA_API_SOCKETXTREME_REF_VMA_BUFF);
		SET_EXTRA_API(socketxtreme_free_vma_buff, enable_socketxtreme ? vma_socketxtreme_free_vma_buff : dummy_vma_socketxtreme_free_vma_buff, VMA_EXTRA_API_SOCKETXTREME_FREE_VMA_BUFF);
		SET_EXTRA_API(dump_fd_stats, vma_dump_fd_stats, VMA_EXTRA_API_DUMP_FD_STATS);
		SET_EXTRA_API(vma_modify_ring, vma_modify_ring, VMA_EXTRA_API_MODIFY_RING);
		SET_EXTRA_API(get_dpcp_devices, vma_get_dpcp_devices, VMA_EXTRA_API_GET_DPCP_DEVICES);
		*((vma_api_t**)__optval) = vma_api;
		return 0;
	}

	// zc add
	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.getsockopt) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	Socket_transbridge* p_socket = NULL;
	p_socket = my_fd_collection_get_sockfd(__fd);
	if(p_socket){
		return p_socket->getsockopt(__level, __optname, __optval, __optlen);
	}

	int ret = 0;
	socket_fd_api* p_socket_object = NULL;
	p_socket_object = fd_collection_get_sockfd(__fd);
	if (p_socket_object) {
		VERIFY_PASSTROUGH_CHANGED(ret, p_socket_object->getsockopt(__level, __optname, __optval, __optlen));
	} else {
		BULLSEYE_EXCLUDE_BLOCK_START
		if (!orig_os_api.getsockopt) get_orig_funcs();
		BULLSEYE_EXCLUDE_BLOCK_END
		ret = orig_os_api.getsockopt(__fd, __level, __optname, __optval, __optlen);
	}

	if (ret >= 0)
		srdr_logdbg_exit("returned with %d", ret);
	else
		srdr_logdbg_exit("failed (errno=%d %m)", errno);
	return ret;
}

/* Do the file control operation described by CMD on FD.
   The remaining arguments are interpreted depending on CMD.

   This function is a cancellation point and therefore not marked with
   __THROW.
   NOTE: VMA throw will never occur during handling of any command.
   VMA will only throw in case VMA doesn't know to handle a command and the
   user requested explicitly that VMA will throw an exception in such a case
   by setting VMA_EXCEPTION_HANDLING accordingly (see README.txt)
   */
extern "C"
EXPORT_SYMBOL
int fcntl(int __fd, int __cmd, ...)
{
	srdr_logfunc_entry("fd=%d, cmd=%d", __fd, __cmd);

	int res = -1;
	va_list va;
	va_start(va, __cmd);
	unsigned long int arg = va_arg(va, unsigned long int);
	va_end(va);

	int ret = 0;
	socket_fd_api* p_socket_object = NULL;
	p_socket_object = fd_collection_get_sockfd(__fd);
	if (p_socket_object) {
		VERIFY_PASSTROUGH_CHANGED(res, p_socket_object->fcntl(__cmd, arg));
	} else {
		BULLSEYE_EXCLUDE_BLOCK_START
		if (!orig_os_api.fcntl) get_orig_funcs();
		BULLSEYE_EXCLUDE_BLOCK_END
		res = orig_os_api.fcntl(__fd, __cmd, arg);
	}

	if (__cmd == F_DUPFD) {
		handle_close(__fd);
	}

	if (ret >= 0)
		srdr_logfunc_exit("returned with %d", ret);
	else
		srdr_logfunc_exit("failed (errno=%d %m)", errno);
	return res;
}

/* Do the file control operation described by CMD on FD.
   The remaining arguments are interpreted depending on CMD.

   This function is a cancellation point and therefore not marked with
   __THROW.
   NOTE: VMA throw will never occur during handling of any command.
   VMA will only throw in case VMA doesn't know to handle a command and the
   user requested explicitly that VMA will throw an exception in such a case
   by setting VMA_EXCEPTION_HANDLING accordingly (see README.txt)
   */

extern "C"
EXPORT_SYMBOL
int fcntl64(int __fd, int __cmd, ...)
{
	srdr_logfunc_entry("fd=%d, cmd=%d", __fd, __cmd);

	int res = -1;
	va_list va;
	va_start(va, __cmd);
	unsigned long int arg = va_arg(va, unsigned long int);
	va_end(va);

	int ret = 0;
	socket_fd_api* p_socket_object = NULL;
	p_socket_object = fd_collection_get_sockfd(__fd);
	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.fcntl64) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END
	if (p_socket_object && orig_os_api.fcntl64) {
		VERIFY_PASSTROUGH_CHANGED(res, p_socket_object->fcntl64(__cmd, arg));
	}
	else {
			if (!orig_os_api.fcntl64) {
				srdr_logfunc_exit("failed (errno=%d %m)", errno);
				VLOG_PRINTF_ONCE_THEN_ALWAYS(VLOG_ERROR, VLOG_DEBUG,
					"fcntl64 was not found during runtime. Set %s to appripriate debug level to see datails. Ignoring...", SYS_VAR_LOG_LEVEL);
				errno = EOPNOTSUPP;
				return -1;
			}
			else {
				res = orig_os_api.fcntl64(__fd, __cmd, arg);
			}
	}

	if (__cmd == F_DUPFD) {
		handle_close(__fd);
	}

	if (ret >= 0)
		srdr_logfunc_exit("returned with %d", ret);
	else
		srdr_logfunc_exit("failed (errno=%d %m)", errno);
	return res;
}


/* Perform the I/O control operation specified by REQUEST on FD.
   One argument may follow; its presence and type depend on REQUEST.
   Return value depends on REQUEST.  Usually -1 indicates error. */
extern "C"
EXPORT_SYMBOL
int ioctl (int __fd, unsigned long int __request, ...)
{
	srdr_logfunc_entry("fd=%d, request=%d", __fd, __request);

	int res = -1;
	va_list va;
	va_start(va, __request);
	unsigned long int arg = va_arg(va, unsigned long int);
	va_end(va);

	int ret = 0;

	socket_fd_api* p_socket_object = NULL;
	p_socket_object = fd_collection_get_sockfd(__fd);
	if (p_socket_object && arg) {
		VERIFY_PASSTROUGH_CHANGED(res, p_socket_object->ioctl(__request, arg));
	} else {
		BULLSEYE_EXCLUDE_BLOCK_START
		if (!orig_os_api.ioctl) get_orig_funcs();
		BULLSEYE_EXCLUDE_BLOCK_END
		res = orig_os_api.ioctl(__fd, __request, arg);
	}

	if (ret >= 0)
		srdr_logfunc_exit("returned with %d", ret);
	else
		srdr_logfunc_exit("failed (errno=%d %m)", errno);
	return res;
}

extern "C"
EXPORT_SYMBOL
int getsockname(int __fd, struct sockaddr *__name, socklen_t *__namelen)
{
	srdr_logdbg_entry("fd=%d", __fd);

	int ret = 0;
	socket_fd_api* p_socket_object = NULL;
	p_socket_object = fd_collection_get_sockfd(__fd);
	if (p_socket_object) {
		ret = p_socket_object->getsockname(__name, __namelen);

		if (safe_mce_sys().trigger_dummy_send_getsockname) {
			char buf[264] = {0};
			struct iovec msg_iov = {&buf, sizeof(buf)};
			struct msghdr msg = {NULL, 0, &msg_iov, 1, NULL, 0, 0};
			int ret_send = sendmsg(__fd, &msg, VMA_SND_FLAGS_DUMMY);
			srdr_logdbg("Triggered dummy message for socket fd=%d (ret_send=%d)", __fd, ret_send);
			NOT_IN_USE(ret_send);
		}
	}
	else {
		BULLSEYE_EXCLUDE_BLOCK_START
		if (!orig_os_api.getsockname) get_orig_funcs();
		BULLSEYE_EXCLUDE_BLOCK_END
		ret = orig_os_api.getsockname(__fd, __name, __namelen);
	}

	if (ret >= 0)
		srdr_logdbg_exit("returned with %d", ret);
	else
		srdr_logdbg_exit("failed (errno=%d %m)", errno);
	return ret;
}

extern "C"
EXPORT_SYMBOL
int getpeername(int __fd, struct sockaddr *__name, socklen_t *__namelen)
{
	srdr_logdbg_entry("fd=%d", __fd);

	int ret = 0;
	socket_fd_api* p_socket_object = NULL;
	p_socket_object = fd_collection_get_sockfd(__fd);
	if (p_socket_object) {
		ret = p_socket_object->getpeername(__name, __namelen);
	}
	else {
		BULLSEYE_EXCLUDE_BLOCK_START
		if (!orig_os_api.getpeername) get_orig_funcs();
		BULLSEYE_EXCLUDE_BLOCK_END
		ret = orig_os_api.getpeername(__fd, __name, __namelen);
	}

	if (ret >= 0)
		srdr_logdbg_exit("returned with %d", ret);
	else
		srdr_logdbg_exit("failed (errno=%d %m)", errno);
	return ret;
}


/* Read NBYTES into BUF from FD.  Return the
   number read, -1 for errors or 0 for EOF.

   This function is a cancellation point and therefore not marked with
   __THROW.  */
extern "C"
EXPORT_SYMBOL
ssize_t read(int __fd, void *__buf, size_t __nbytes)
{
	srdr_logfuncall_entry("fd=%d", __fd);


	// zc add
	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.read) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END
	

	Socket_transbridge* p_socket = NULL;
	p_socket = my_fd_collection_get_sockfd(__fd);
	if(p_socket){
		return p_socket->read(__buf,  __nbytes);
	}

	return orig_os_api.read(__fd, __buf, __nbytes);
	//printf("Contents of server's buffer: '%s'\n", res.buf);

	//return ret;



	socket_fd_api* p_socket_object = NULL;
	p_socket_object = fd_collection_get_sockfd(__fd);
	if (p_socket_object) {
		struct iovec piov[1];
		piov[0].iov_base = __buf;
		piov[0].iov_len = __nbytes;
		int dummy_flags = 0;
		return p_socket_object->rx(RX_READ, piov, 1, &dummy_flags);
	}

	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.read) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	return orig_os_api.read(__fd, __buf, __nbytes);
}

#if defined HAVE___READ_CHK
/* Checks that the buffer is big enough to contain the number of bytes
 * the user requests to read. If the buffer is too small, aborts,
 * else read NBYTES into BUF from FD.  Return the
   number read, -1 for errors or 0 for EOF.

   This function is a cancellation point and therefore not marked with
   __THROW.  */
extern "C"
EXPORT_SYMBOL
ssize_t __read_chk(int __fd, void *__buf, size_t __nbytes, size_t __buflen)
{
	srdr_logfuncall_entry("fd=%d", __fd);

	socket_fd_api* p_socket_object = NULL;
	p_socket_object = fd_collection_get_sockfd(__fd);
	if (p_socket_object) {
		BULLSEYE_EXCLUDE_BLOCK_START
		if (__nbytes > __buflen) {
		    srdr_logpanic("buffer overflow detected");
		}
		BULLSEYE_EXCLUDE_BLOCK_END

		struct iovec piov[1];
		piov[0].iov_base = __buf;
		piov[0].iov_len = __nbytes;
		int dummy_flags = 0;
		return p_socket_object->rx(RX_READ, piov, 1, &dummy_flags);
	}
	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.__read_chk) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	return orig_os_api.__read_chk(__fd, __buf, __nbytes, __buflen);
}
#endif

/* Read COUNT blocks into VECTOR from FD.  Return the
   number of bytes read, -1 for errors or 0 for EOF.

   This function is a cancellation point and therefore not marked with
   __THROW.  */

extern "C"
EXPORT_SYMBOL
ssize_t readv(int __fd, const struct iovec *iov, int iovcnt)
{
	srdr_logfuncall_entry("fd=%d", __fd);

	socket_fd_api* p_socket_object = NULL;
	p_socket_object = fd_collection_get_sockfd(__fd);
	if (p_socket_object) {
		struct iovec* piov = (struct iovec*)iov;
		int dummy_flags = 0;
		return p_socket_object->rx(RX_READV, piov, iovcnt, &dummy_flags);
	}
	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.readv) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	return orig_os_api.readv(__fd, iov, iovcnt);
}

/* Read N bytes into BUF from socket FD.
   Returns the number read or -1 for errors.

   This function is a cancellation point and therefore not marked with
   __THROW.  */
extern "C"
EXPORT_SYMBOL
ssize_t recv(int __fd, void *__buf, size_t __nbytes, int __flags)
{
	srdr_logfuncall_entry("fd=%d", __fd);

	// zc add
	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.recv) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END
	

	Socket_transbridge* p_socket = NULL;
	p_socket = my_fd_collection_get_sockfd(__fd);
	if(p_socket){
		return p_socket->recv(__buf,  __nbytes, __flags);
	}

	return orig_os_api.recv(__fd, __buf, __nbytes, __flags);


	socket_fd_api* p_socket_object = NULL;
	p_socket_object = fd_collection_get_sockfd(__fd);
	if (p_socket_object) {
		struct iovec piov[1];
		piov[0].iov_base = __buf;
		piov[0].iov_len = __nbytes;
		return p_socket_object->rx(RX_RECV, piov, 1, &__flags);
	}
	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.recv) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	return orig_os_api.recv(__fd, __buf, __nbytes, __flags);
}

#if defined HAVE___RECV_CHK
/* Checks that the buffer is big enough to contain the number of bytes
   the user requests to read. If the buffer is too small, aborts,
   else read N bytes into BUF from socket FD.
   Returns the number read or -1 for errors.

   This function is a cancellation point and therefore not marked with
   __THROW.  */
extern "C"
EXPORT_SYMBOL
ssize_t __recv_chk(int __fd, void *__buf, size_t __nbytes, size_t __buflen, int __flags)
{
	srdr_logfuncall_entry("fd=%d", __fd);

	socket_fd_api* p_socket_object = NULL;
	p_socket_object = fd_collection_get_sockfd(__fd);
	if (p_socket_object) {
		BULLSEYE_EXCLUDE_BLOCK_START
		if (__nbytes > __buflen) {
			srdr_logpanic("buffer overflow detected");
		}
		BULLSEYE_EXCLUDE_BLOCK_END

		struct iovec piov[1];
		piov[0].iov_base = __buf;
		piov[0].iov_len = __nbytes;
		return p_socket_object->rx(RX_RECV, piov, 1, &__flags);
	}
	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.__recv_chk) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	return orig_os_api.__recv_chk(__fd, __buf, __nbytes, __buflen, __flags);
}
#endif

/* Receive a message as described by MESSAGE from socket FD.
   Returns the number of bytes read or -1 for errors.

   This function is a cancellation point and therefore not marked with
   __THROW.  */
extern "C"
EXPORT_SYMBOL
ssize_t recvmsg(int __fd, struct msghdr *__msg, int __flags)
{
	srdr_logfuncall_entry("fd=%d", __fd);

	if (__msg == NULL) {
		srdr_logdbg("NULL msghdr");
		errno = EINVAL;
		return -1;
	}

	socket_fd_api* p_socket_object = NULL;
	p_socket_object = fd_collection_get_sockfd(__fd);
	if (p_socket_object) {
		__msg->msg_flags = 0;
		return p_socket_object->rx(RX_RECVMSG, __msg->msg_iov, __msg->msg_iovlen, &__flags, (__SOCKADDR_ARG)__msg->msg_name, (socklen_t*)&__msg->msg_namelen, __msg);
	}
	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.recvmsg) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	return orig_os_api.recvmsg(__fd, __msg, __flags);
}

/* The following definitions are for kernels previous to 2.6.32 which dont support recvmmsg */
#ifndef HAVE_STRUCT_MMSGHDR
#ifndef __INTEL_COMPILER
struct mmsghdr {
    struct msghdr msg_hdr;  // Message header
    unsigned int  msg_len;  // Number of received bytes for header
};
#endif
#endif

#ifndef MSG_WAITFORONE
#define MSG_WAITFORONE  0x10000 //recvmmsg(): block until 1+ packets avail
#endif

/* Receive multiple messages as described by MESSAGE from socket FD.
   Returns the number of messages received or -1 for errors.

   This function is a cancellation point and therefore not marked with
   __THROW.  */
extern "C"
EXPORT_SYMBOL
#ifdef RECVMMSG_WITH_CONST_TIMESPEC
int recvmmsg(int __fd, struct mmsghdr *__mmsghdr, unsigned int __vlen, int __flags, const struct timespec *__timeout)
#else
int recvmmsg(int __fd, struct mmsghdr *__mmsghdr, unsigned int __vlen, int __flags, struct timespec *__timeout)
#endif
{
        int num_of_msg=0;
        struct timespec start_time = TIMESPEC_INITIALIZER, current_time = TIMESPEC_INITIALIZER, delta_time = TIMESPEC_INITIALIZER;

        srdr_logfuncall_entry("fd=%d, mmsghdr length=%d flags=%x", __fd, __vlen, __flags);

        if (__mmsghdr == NULL) {
                srdr_logdbg("NULL mmsghdr");
                errno = EINVAL;
                return -1;
        }

        if (__timeout) {
        	gettime(&start_time);
        }
        socket_fd_api* p_socket_object = NULL;
        p_socket_object = fd_collection_get_sockfd(__fd);
        if (p_socket_object) {
        	int ret = 0;
                for (unsigned int i=0; i<__vlen; i++) {
                       int flags = __flags;
                       __mmsghdr[i].msg_hdr.msg_flags = 0;
                       ret = p_socket_object->rx(RX_RECVMSG, __mmsghdr[i].msg_hdr.msg_iov, __mmsghdr[i].msg_hdr.msg_iovlen, &flags,
                                                         (__SOCKADDR_ARG)__mmsghdr[i].msg_hdr.msg_name, (socklen_t*)&__mmsghdr[i].msg_hdr.msg_namelen,  &__mmsghdr[i].msg_hdr);
                       if (ret < 0){
                               break;
                       }
                       num_of_msg++;
                       __mmsghdr[i].msg_len = ret;
                       if ((i==0) && (flags & MSG_WAITFORONE)) {
                               __flags |= MSG_DONTWAIT;
                       }
                       if (__timeout) {
                	       gettime(&current_time);
                	       ts_sub(&current_time, &start_time, &delta_time);
                	       if (ts_cmp(&delta_time, __timeout, >)) {
                		       break;
                	       }
                       }
                }
                if (num_of_msg || ret == 0) {
                	//todo save ret for so_error if ret != 0(see kernel)
                	return num_of_msg;
                } else {
                	return ret;
                }
        }
        BULLSEYE_EXCLUDE_BLOCK_START
        if (!orig_os_api.recvmmsg) get_orig_funcs();
        BULLSEYE_EXCLUDE_BLOCK_END

        return orig_os_api.recvmmsg(__fd, __mmsghdr, __vlen, __flags, __timeout);
}


/* Read N bytes into BUF through socket FD.
   If ADDR is not NULL, fill in *ADDR_LEN bytes of it with tha address of
   the sender, and store the actual size of the address in *ADDR_LEN.
   Returns the number of bytes read or -1 for errors.

   This function is a cancellation point and therefore not marked with
   __THROW.  */
extern "C"
EXPORT_SYMBOL
ssize_t recvfrom(int __fd, void *__buf, size_t __nbytes, int __flags,
		 struct sockaddr *__from, socklen_t *__fromlen)
{
	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.recvfrom) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	//zc add 
	Socket_transbridge* p_socket = NULL;
	p_socket = my_fd_collection_get_sockfd(__fd);
	if(p_socket){
		return p_socket->recvfrom(__buf, __nbytes, __flags, __from, __fromlen);
	}

	return orig_os_api.recvfrom(__fd, __buf, __nbytes, __flags, __from, __fromlen);




	ssize_t ret_val = 0;

	srdr_logfuncall_entry("fd=%d", __fd);

	socket_fd_api* p_socket_object = NULL;
	p_socket_object = fd_collection_get_sockfd(__fd);
	if (p_socket_object) {
		struct iovec piov[1];
		piov[0].iov_base = __buf;
		piov[0].iov_len = __nbytes;
		ret_val = p_socket_object->rx(RX_RECVFROM, piov, 1, &__flags, __from, __fromlen);
	}
	else {
		BULLSEYE_EXCLUDE_BLOCK_START
		if (!orig_os_api.recvfrom) get_orig_funcs();
		BULLSEYE_EXCLUDE_BLOCK_END
		ret_val = orig_os_api.recvfrom(__fd, __buf, __nbytes, __flags, __from, __fromlen);
	}
#ifdef RDTSC_MEASURE_RX_PROCCESS_BUFFER_TO_RECIVEFROM
	RDTSC_TAKE_END(g_rdtsc_instr_info_arr[RDTSC_FLOW_PROCCESS_RX_BUFFER_TO_RECIVEFROM]);
#endif //RDTSC_MEASURE_RX_PROCCESS_BUFFER_TO_RECIVEFROM

#ifdef RDTSC_MEASURE_RX_LWIP_TO_RECEVEFROM
	RDTSC_TAKE_END(g_rdtsc_instr_info_arr[RDTSC_FLOW_RX_LWIP_TO_RECEVEFROM]);
#endif //RDTSC_MEASURE_RX_LWIP_TO_RECEVEFROM

#ifdef RDTSC_MEASURE_RX_CQE_RECEIVEFROM
	RDTSC_TAKE_END(g_rdtsc_instr_info_arr[RDTSC_FLOW_RX_CQE_TO_RECEIVEFROM]);
#endif //RDTSC_MEASURE_RX_CQE_RECEIVEFROM

#ifdef RDTSC_MEASURE_RECEIVEFROM_TO_SENDTO
	RDTSC_TAKE_START(g_rdtsc_instr_info_arr[RDTSC_FLOW_RECEIVEFROM_TO_SENDTO]);
#endif //RDTSC_MEASURE_RECEIVEFROM_TO_SENDTO
	return ret_val;
}

#if defined HAVE___RECVFROM_CHK
/* Checks that the buffer is big enough to contain the number of bytes
   the user requests to read. If the buffer is too small, aborts,
   else read N bytes into BUF through socket FD.
   If ADDR is not NULL, fill in *ADDR_LEN bytes of it with tha address of
   the sender, and store the actual size of the address in *ADDR_LEN.
   Returns the number of bytes read or -1 for errors.

   This function is a cancellation point and therefore not marked with
   __THROW.  */
extern "C"
EXPORT_SYMBOL
ssize_t __recvfrom_chk(int __fd, void *__buf, size_t __nbytes, size_t __buflen, int __flags,
		 struct sockaddr *__from, socklen_t *__fromlen)
{
	srdr_logfuncall_entry("fd=%d", __fd);

	socket_fd_api* p_socket_object = NULL;
	p_socket_object = fd_collection_get_sockfd(__fd);
	if (p_socket_object) {
		BULLSEYE_EXCLUDE_BLOCK_START
		if (__nbytes > __buflen) {
		    srdr_logpanic("buffer overflow detected");
		}
		BULLSEYE_EXCLUDE_BLOCK_END

		struct iovec piov[1];
		piov[0].iov_base = __buf;
		piov[0].iov_len = __nbytes;
		return p_socket_object->rx(RX_RECVFROM, piov, 1, &__flags, __from, __fromlen);
	}
	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.__recvfrom_chk) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	return orig_os_api.__recvfrom_chk(__fd, __buf, __nbytes, __buflen, __flags, __from, __fromlen);
}
#endif

/* Write N bytes of BUF to FD.  Return the number written, or -1.

   This function is a cancellation point and therefore not marked with
   __THROW.  */
extern "C"
EXPORT_SYMBOL
ssize_t write(int __fd, __const void *__buf, size_t __nbytes)
{
	srdr_logfuncall_entry("fd=%d, nbytes=%d", __fd, __nbytes);


	//zc add 
	Socket_transbridge* p_socket = NULL;
	p_socket = my_fd_collection_get_sockfd(__fd);
	if(p_socket){
		return p_socket->write(__buf, __nbytes);
	}

	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.write) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	return orig_os_api.write(__fd, __buf, __nbytes);



	socket_fd_api* p_socket_object = NULL;
	p_socket_object = fd_collection_get_sockfd(__fd);
	if (p_socket_object) {
		struct iovec piov[1] = {{(void*)__buf, __nbytes}};
		vma_tx_call_attr_t tx_arg;

		tx_arg.opcode = TX_WRITE;
		tx_arg.attr.msg.iov = piov;
		tx_arg.attr.msg.sz_iov = 1;

		return p_socket_object->tx(tx_arg);
	}
	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.write) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	return orig_os_api.write(__fd, __buf, __nbytes);
}

/* Write IOCNT blocks from IOVEC to FD.  Return the number written, or -1.

   This function is a cancellation point and therefore not marked with
   __THROW.  */
extern "C"
EXPORT_SYMBOL
ssize_t writev(int __fd, const struct iovec *iov, int iovcnt)
{
	srdr_logfuncall_entry("fd=%d, %d iov blocks", __fd, iovcnt);

	socket_fd_api* p_socket_object = NULL;
	p_socket_object = fd_collection_get_sockfd(__fd);
	if (p_socket_object) {
		vma_tx_call_attr_t tx_arg;

		tx_arg.opcode = TX_WRITEV;
		tx_arg.attr.msg.iov = (struct iovec *)iov;
		tx_arg.attr.msg.sz_iov = iovcnt;

		return p_socket_object->tx(tx_arg);
	}
	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.writev) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	return orig_os_api.writev(__fd, iov, iovcnt);
}


/* Send N bytes of BUF to socket FD.  Returns the number sent or -1.

   This function is a cancellation point and therefore not marked with
   __THROW.  */
extern "C"
EXPORT_SYMBOL
ssize_t send(int __fd, __const void *__buf, size_t __nbytes, int __flags)
{
	srdr_logfuncall_entry("fd=%d, nbytes=%d", __fd, __nbytes);

	//zc add 
	Socket_transbridge* p_socket = NULL;
	p_socket = my_fd_collection_get_sockfd(__fd);
	if(p_socket){
		return p_socket->send(__buf,  __nbytes,  __flags);
	}
	

	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.send) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	return orig_os_api.send(__fd, __buf, __nbytes, __flags);


	socket_fd_api* p_socket_object = NULL;
	p_socket_object = fd_collection_get_sockfd(__fd);
	if (p_socket_object) {
		struct iovec piov[1] = {{(void*)__buf, __nbytes}};
		vma_tx_call_attr_t tx_arg;

		tx_arg.opcode = TX_SEND;
		tx_arg.attr.msg.iov = piov;
		tx_arg.attr.msg.sz_iov = 1;
		tx_arg.attr.msg.flags = __flags;

		return p_socket_object->tx(tx_arg);
	}

	// Ignore dummy messages for OS
	if (unlikely(IS_DUMMY_PACKET(__flags))) {
		errno = EINVAL;
		return -1;
	}
	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.send) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	return orig_os_api.send(__fd, __buf, __nbytes, __flags);
}

/* Sends a message as described by MESSAGE to socket FD.
   Returns the number of bytes read or -1 for errors.

   This function is a cancellation point and therefore not marked with
   __THROW.  */
extern "C"
EXPORT_SYMBOL
ssize_t sendmsg(int __fd, __const struct msghdr *__msg, int __flags)
{
	srdr_logfuncall_entry("fd=%d", __fd);

	socket_fd_api* p_socket_object = NULL;
	p_socket_object = fd_collection_get_sockfd(__fd);
	if (p_socket_object) {
		vma_tx_call_attr_t tx_arg;

		tx_arg.opcode = TX_SENDMSG;
		tx_arg.attr.msg.iov = __msg->msg_iov;
		tx_arg.attr.msg.sz_iov = (ssize_t)__msg->msg_iovlen;
		tx_arg.attr.msg.flags = __flags;
		tx_arg.attr.msg.addr = (struct sockaddr *)(__CONST_SOCKADDR_ARG)__msg->msg_name;
		tx_arg.attr.msg.len = (socklen_t)__msg->msg_namelen;

		return p_socket_object->tx(tx_arg);
	}

	// Ignore dummy messages for OS
	if (unlikely(IS_DUMMY_PACKET(__flags))) {
		errno = EINVAL;
		return -1;
	}
	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.sendmsg) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	return orig_os_api.sendmsg(__fd, __msg, __flags);

}

/* Send multiple messages as described by MESSAGE from socket FD.
   Returns the number of messages sent or -1 for errors.

   This function is a cancellation point and therefore not marked with
   __THROW.  */
extern "C"
EXPORT_SYMBOL
int sendmmsg(int __fd, struct mmsghdr *__mmsghdr, unsigned int __vlen, int __flags)
{
	int num_of_msg=0;

	srdr_logfuncall_entry("fd=%d, mmsghdr length=%d flags=%x", __fd, __vlen, __flags);

	if (__mmsghdr == NULL) {
		srdr_logdbg("NULL mmsghdr");
		errno = EINVAL;
		return -1;
	}

	socket_fd_api* p_socket_object = NULL;
	p_socket_object = fd_collection_get_sockfd(__fd);
	if (p_socket_object) {
		for (unsigned int i=0; i<__vlen; i++) {
			vma_tx_call_attr_t tx_arg;

			tx_arg.opcode = TX_SENDMSG;
			tx_arg.attr.msg.iov = __mmsghdr[i].msg_hdr.msg_iov;
			tx_arg.attr.msg.sz_iov = (ssize_t)__mmsghdr[i].msg_hdr.msg_iovlen;
			tx_arg.attr.msg.flags = __flags;
			tx_arg.attr.msg.addr = (struct sockaddr *)(__SOCKADDR_ARG)__mmsghdr[i].msg_hdr.msg_name;
			tx_arg.attr.msg.len = (socklen_t)__mmsghdr[i].msg_hdr.msg_namelen;

			int ret = p_socket_object->tx(tx_arg);
			if (ret < 0){
				if (num_of_msg)
					return num_of_msg;
				else
					return ret;
			}
			num_of_msg++;
			__mmsghdr[i].msg_len = ret;
		}
		return num_of_msg;
	}

	// Ignore dummy messages for OS
	if (unlikely(IS_DUMMY_PACKET(__flags))) {
		errno = EINVAL;
		return -1;
	}
	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.sendmmsg) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	return orig_os_api.sendmmsg(__fd, __mmsghdr, __vlen, __flags);
}

/* Send N bytes of BUF on socket FD to peer at address ADDR (which is
   ADDR_LEN bytes long).  Returns the number sent, or -1 for errors.

   This function is a cancellation point and therefore not marked with
   __THROW.  */
extern "C"
EXPORT_SYMBOL
ssize_t sendto(int __fd, __const void *__buf, size_t __nbytes, int __flags,
	       const struct sockaddr *__to, socklen_t __tolen)
{

	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.sendto) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	//zc add 
	Socket_transbridge* p_socket = NULL;
	p_socket = my_fd_collection_get_sockfd(__fd);
	if(p_socket){
		return p_socket->sendto(__buf, __nbytes, __flags, __to, __tolen);
	}

	return orig_os_api.sendto(__fd, __buf, __nbytes, __flags, __to, __tolen);

#ifdef RDTSC_MEASURE_TX_SENDTO_TO_AFTER_POST_SEND
	RDTSC_TAKE_START(g_rdtsc_instr_info_arr[RDTSC_FLOW_SENDTO_TO_AFTER_POST_SEND]);
#endif //RDTSC_MEASURE_TX_SENDTO_TO_AFTER_POST_SEND

#ifdef RDTSC_MEASURE_RECEIVEFROM_TO_SENDTO
	RDTSC_TAKE_END(g_rdtsc_instr_info_arr[RDTSC_FLOW_RECEIVEFROM_TO_SENDTO]);
#endif //RDTSC_MEASURE_TX_SENDTO_TO_AFTER_POST_SEND
	srdr_logfuncall_entry("fd=%d, nbytes=%d", __fd, __nbytes);

	socket_fd_api* p_socket_object = NULL;
	p_socket_object = fd_collection_get_sockfd(__fd);
	if (p_socket_object) {
		struct iovec piov[1] = {{(void*)__buf, __nbytes}};
		vma_tx_call_attr_t tx_arg;

		tx_arg.opcode = TX_SENDTO;
		tx_arg.attr.msg.iov = piov;
		tx_arg.attr.msg.sz_iov = 1;
		tx_arg.attr.msg.flags = __flags;
		tx_arg.attr.msg.addr = (struct sockaddr *)__to;
		tx_arg.attr.msg.len = __tolen;

		return p_socket_object->tx(tx_arg);
	}

	// Ignore dummy messages for OS
	if (unlikely(IS_DUMMY_PACKET(__flags))) {
		errno = EINVAL;
		return -1;
	}


	return orig_os_api.sendto(__fd, __buf, __nbytes, __flags, __to, __tolen);
}

static ssize_t sendfile_helper(socket_fd_api* p_socket_object, int in_fd, __off64_t *offset, size_t count)
{
	ssize_t totSent = 0;
	struct stat64 stat_buf;
	__off64_t orig_offset = 0;
	__off64_t cur_offset = 0;
	struct iovec piov[1] = {{NULL, 0}};
	vma_tx_call_attr_t tx_arg;
	sockinfo* s = (sockinfo*)p_socket_object;

	if (p_socket_object->get_type() != FD_TYPE_SOCKET) {
		errno = EBADF;
		return -1;
	}

	orig_offset = lseek64(in_fd, 0, SEEK_CUR);
	if (orig_offset < 0) {
		errno = ESPIPE;
		return -1;
	}

	cur_offset = (offset ? *offset : orig_offset);
	if (offset && (lseek64(in_fd, cur_offset, SEEK_SET) == -1)) {
		errno = EINVAL;
		return -1;
	}

	if ((fstat64(in_fd, &stat_buf) == -1) ||
		((__off64_t)stat_buf.st_size < (__off64_t)(cur_offset + count))) {
		errno = EOVERFLOW;
		return -1;
	}

	if (PROTO_TCP == s->get_protocol()) {
		tx_arg.opcode = TX_FILE;
		tx_arg.attr.msg.iov = piov;
		tx_arg.attr.msg.sz_iov = 1;

		piov[0].iov_base = (void *)&in_fd;
		piov[0].iov_len = count;

		totSent = p_socket_object->tx(tx_arg);
	} else {
		__off64_t pa_offset = 0;
		size_t pa_count = 0;
		struct flock64 lock;

		tx_arg.opcode = TX_WRITE;
		tx_arg.attr.msg.iov = piov;
		tx_arg.attr.msg.sz_iov = 1;

		/* The off argument of mmap() is constrained to be aligned and
		 * sized according to the value returned by sysconf()
		 */
		pa_offset = cur_offset & ~(sysconf(_SC_PAGE_SIZE) - 1);
		pa_count = count + cur_offset - pa_offset;

		lock.l_type = F_RDLCK;
		lock.l_whence = SEEK_SET;
		lock.l_start = pa_offset;
		lock.l_len = pa_count;
		lock.l_pid = 0;

		/* try to use mmap() approach */
		if (-1 != (fcntl(in_fd, F_SETLK, &lock))) {
			void *addr = NULL;
			addr = mmap64(NULL, pa_count, PROT_READ, MAP_SHARED | MAP_NORESERVE, in_fd, pa_offset);
			if (MAP_FAILED != addr) {
				ssize_t toRead, numSent = 0;

				while (count > 0) {
					toRead = min(sysconf(_SC_PAGE_SIZE), (ssize_t)count);

					piov[0].iov_base = (void *)((uintptr_t)addr + cur_offset - pa_offset + totSent);
					piov[0].iov_len = toRead;

					numSent = p_socket_object->tx(tx_arg);
					if (numSent == -1) {
						break;
					}

					count -= numSent;
					totSent += numSent;
				}
				(void)munmap(addr, pa_count);
			}
			lock.l_type = F_UNLCK;
			(void)fcntl(in_fd, F_SETLK, &lock);
		}

		/* fallback on read() approach */
		if (totSent <= 0) {
			char buf[sysconf(_SC_PAGE_SIZE)];
			ssize_t toRead, numRead, numSent = 0;

			while (count > 0) {
				toRead = min(sizeof(buf), count);
				numRead = orig_os_api.read(in_fd, buf, toRead);
				if (numRead <= 0) {
					break;
				}

				piov[0].iov_base = (void *)buf;
				piov[0].iov_len = numRead;

				numSent = p_socket_object->tx(tx_arg);
				if (numSent == -1) {
					break;
				}

				count -= numSent;
				totSent += numSent;
			}
		}
	}

	if (totSent > 0) {
		if (offset != NULL) {
			(void)lseek64(in_fd, (orig_offset), SEEK_SET);
			*offset = *offset + totSent;
		} else {
			(void)lseek64(in_fd, (orig_offset + totSent), SEEK_SET);
		}
	}

	return totSent;
}

extern "C"
EXPORT_SYMBOL
ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count)
{
	srdr_logfuncall_entry("out_fd=%d, in_fd=%d, offset=%p, *offset=%zu, count=%d", out_fd, in_fd, offset, offset ? *offset : 0, count);

	socket_fd_api* p_socket_object = fd_collection_get_sockfd(out_fd);
	if (!p_socket_object) {
		if (!orig_os_api.sendfile) get_orig_funcs();
		return orig_os_api.sendfile(out_fd, in_fd, offset, count);
	}

	return sendfile_helper(p_socket_object, in_fd, offset, count);
}

extern "C"
EXPORT_SYMBOL
ssize_t sendfile64(int out_fd, int in_fd, __off64_t *offset, size_t count)
{
	srdr_logfuncall_entry("out_fd=%d, in_fd=%d, offset=%p, *offset=%zu, count=%d", out_fd, in_fd, offset, offset ? *offset : 0, count);

	socket_fd_api* p_socket_object = fd_collection_get_sockfd(out_fd);
	if (!p_socket_object) {
		if (!orig_os_api.sendfile64) get_orig_funcs();
		return orig_os_api.sendfile64(out_fd, in_fd, offset, count);
	}

	return sendfile_helper(p_socket_object, in_fd, offset, count);
}

// Format a fd_set into a string for logging
// Check nfd to know how many 32 bits hexs do we want to sprintf into user buffer
const char* sprintf_fdset(char* buf, int buflen, int __nfds, fd_set *__fds)
{
	if (buflen<1)
		return "(null)";
	buf[0] = '\0';

	if ((__nfds <= 0) || (__fds == NULL))
		return "(null)";

	int fdsize = 1 + ((__nfds-1) / (8*sizeof(uint32_t)));
	switch (fdsize) {
	case 1:
		snprintf(buf, buflen, "%08x", ((uint32_t*)__fds)[0]);
		break;
	case 2:
		snprintf(buf, buflen, "%08x %08x", ((uint32_t*)__fds)[1], ((uint32_t*)__fds)[0]);
		break;
	case 3:
		snprintf(buf, buflen, "%08x %08x %08x", ((uint32_t*)__fds)[2], ((uint32_t*)__fds)[1], ((uint32_t*)__fds)[0]);
		break;
	case 4:
		snprintf(buf, buflen, "%08x %08x %08x %08x", ((uint32_t*)__fds)[3], ((uint32_t*)__fds)[2], ((uint32_t*)__fds)[1], ((uint32_t*)__fds)[0]);
		break;
	case 5:
		snprintf(buf, buflen, "%08x %08x %08x %08x %08x", ((uint32_t*)__fds)[4], ((uint32_t*)__fds)[3], ((uint32_t*)__fds)[2], ((uint32_t*)__fds)[1], ((uint32_t*)__fds)[0]);
		break;
	case 6:
		snprintf(buf, buflen, "%08x %08x %08x %08x %08x %08x", ((uint32_t*)__fds)[5], ((uint32_t*)__fds)[4], ((uint32_t*)__fds)[3], ((uint32_t*)__fds)[2], ((uint32_t*)__fds)[1], ((uint32_t*)__fds)[0]);
		break;
	default:
		buf[0] = '\0';
	}
	return buf;
}

/* Check the first NFDS descriptors each in READFDS (if not NULL) for read
   readiness, in WRITEFDS (if not NULL) for write readiness, and in EXCEPTFDS
   (if not NULL) for exceptional conditions.  If TIMis not NULL, time out
   after waiting the interval specified therein.  Returns the number of ready
   descriptors, or -1 for errors.

   This function is a cancellation point and therefore not marked with
   __THROW.  */
static int select_helper(int __nfds,
	   fd_set *__readfds,
	   fd_set * __writefds,
	   fd_set * __exceptfds,
	   struct timeval * __timeout,
	   const sigset_t *__sigmask = NULL)
{
	int off_rfds_buffer[__nfds];
	io_mux_call::offloaded_mode_t off_modes_buffer[__nfds];

	if (g_vlogger_level >= VLOG_FUNC) {
		const int tmpbufsize = 256;
		char tmpbuf[tmpbufsize], tmpbuf2[tmpbufsize];
		NOT_IN_USE(tmpbufsize); /* to suppress warning in case VMA_MAX_DEFINED_LOG_LEVEL */
		NOT_IN_USE(tmpbuf); /* to suppress warning in case VMA_MAX_DEFINED_LOG_LEVEL */
		NOT_IN_USE(tmpbuf2); /* to suppress warning in case VMA_MAX_DEFINED_LOG_LEVEL */
		srdr_logfunc("readfds: %s, writefds: %s",
			   sprintf_fdset(tmpbuf, tmpbufsize, __nfds, __readfds), 
			   sprintf_fdset(tmpbuf2, tmpbufsize, __nfds, __writefds));
	}

	try {
		select_call scall(off_rfds_buffer, off_modes_buffer,
		                  __nfds, __readfds, __writefds, __exceptfds, __timeout, __sigmask);
		int rc = scall.call();

		if (g_vlogger_level >= VLOG_FUNC) {
			const int tmpbufsize = 256;
			char tmpbuf[tmpbufsize], tmpbuf2[tmpbufsize];
			NOT_IN_USE(tmpbufsize); /* to suppress warning in case VMA_MAX_DEFINED_LOG_LEVEL */
			NOT_IN_USE(tmpbuf); /* to suppress warning in case VMA_MAX_DEFINED_LOG_LEVEL */
			NOT_IN_USE(tmpbuf2); /* to suppress warning in case VMA_MAX_DEFINED_LOG_LEVEL */
			srdr_logfunc_exit("readfds: %s, writefds: %s",
				   sprintf_fdset(tmpbuf, tmpbufsize, __nfds, __readfds),
				   sprintf_fdset(tmpbuf2, tmpbufsize, __nfds, __writefds));
		}

		return rc;
	}
	catch (io_mux_call::io_error&) {
		srdr_logfunc_exit("io_mux_call::io_error (errno=%d %m)", errno);
		return -1;
	}
}

extern "C"
EXPORT_SYMBOL
int select(int __nfds,
	   fd_set *__readfds,
	   fd_set * __writefds,
	   fd_set * __exceptfds,
	   struct timeval * __timeout)
{
	if (!g_p_fd_collection) {
		BULLSEYE_EXCLUDE_BLOCK_START
		if (!orig_os_api.select) get_orig_funcs();
		BULLSEYE_EXCLUDE_BLOCK_END
		return orig_os_api.select(__nfds, __readfds, __writefds, __exceptfds, __timeout);
	}

	if (__timeout) {
		srdr_logfunc_entry("nfds=%d, timeout=(%d sec, %d usec)",
				                   __nfds, __timeout->tv_sec, __timeout->tv_usec);
	} else {
		srdr_logfunc_entry("nfds=%d, timeout=(infinite)", __nfds);
	}

	return select_helper(__nfds, __readfds, __writefds, __exceptfds, __timeout);
}

extern "C"
EXPORT_SYMBOL
int pselect(int __nfds,
	    fd_set *__readfds,
	    fd_set *__writefds,
	    fd_set *__errorfds,
	    const struct timespec *__timeout,
	    const sigset_t *__sigmask)
{
	if (!g_p_fd_collection) {
		BULLSEYE_EXCLUDE_BLOCK_START
		if (!orig_os_api.pselect) get_orig_funcs();
		BULLSEYE_EXCLUDE_BLOCK_END
		return orig_os_api.pselect(__nfds, __readfds, __writefds, __errorfds, __timeout, __sigmask);
	}

	struct timeval select_time;
	if (__timeout) {
		srdr_logfunc_entry("nfds=%d, timeout=(%d sec, %d nsec)",
					           __nfds, __timeout->tv_sec, __timeout->tv_nsec);
		select_time.tv_sec = __timeout->tv_sec;
		select_time.tv_usec = __timeout->tv_nsec / 1000;
	} else {
		srdr_logfunc_entry("nfds=%d, timeout=(infinite)", __nfds);
	}

	return select_helper(__nfds, __readfds, __writefds, __errorfds, __timeout ? &select_time : NULL, __sigmask);
}

/* Poll the file descriptors described by the NFDS structures starting at
   FDS.  If TIMis nonzero and not -1, allow TIMmilliseconds for
   an event to occur; if TIMis -1, block until an event occurs.
   Returns the number of file descriptors with events, zero if timed out,
   or -1 for errors.  */
static int poll_helper(struct pollfd *__fds, nfds_t __nfds, int __timeout, const sigset_t *__sigmask = NULL)
{
	int off_rfd_buffer[__nfds];
	io_mux_call::offloaded_mode_t off_modes_buffer[__nfds];
	int lookup_buffer[__nfds];
	pollfd working_fds_arr[__nfds + 1];

	try {
		poll_call pcall(off_rfd_buffer, off_modes_buffer, lookup_buffer, working_fds_arr,
		                __fds, __nfds, __timeout, __sigmask);
		
		int rc = pcall.call();
		srdr_logfunc_exit("rc = %d", rc);
		return rc;
	}
	catch (io_mux_call::io_error&) {
		srdr_logfunc_exit("io_mux_call::io_error (errno=%d %m)", errno);
		return -1;
	}
}

extern "C"
EXPORT_SYMBOL
int poll(struct pollfd *__fds, nfds_t __nfds, int __timeout)
{
	if (!g_p_fd_collection) {
		BULLSEYE_EXCLUDE_BLOCK_START
		if (!orig_os_api.poll) get_orig_funcs();
		BULLSEYE_EXCLUDE_BLOCK_END
		return orig_os_api.poll(__fds, __nfds, __timeout);
	}

	srdr_logfunc_entry("nfds=%d, timeout=(%d milli-sec)", __nfds, __timeout);

	return poll_helper(__fds, __nfds, __timeout);
}

#if defined HAVE___POLL_CHK
extern "C"
EXPORT_SYMBOL
int __poll_chk(struct pollfd *__fds, nfds_t __nfds, int __timeout, size_t __fdslen)
{
	if (!g_p_fd_collection) {
		BULLSEYE_EXCLUDE_BLOCK_START
		if (!orig_os_api.__poll_chk) get_orig_funcs();
		BULLSEYE_EXCLUDE_BLOCK_END
		return orig_os_api.__poll_chk(__fds, __nfds, __timeout, __fdslen);
	}

	BULLSEYE_EXCLUDE_BLOCK_START
	if (__fdslen / sizeof(*__fds) < __nfds) {
	    srdr_logpanic("buffer overflow detected");
	}
	BULLSEYE_EXCLUDE_BLOCK_END

	srdr_logfunc_entry("nfds=%d, timeout=(%d milli-sec)", __nfds, __timeout);

	return poll_helper(__fds, __nfds, __timeout);
}
#endif

extern "C"
EXPORT_SYMBOL
int ppoll(struct pollfd *__fds, nfds_t __nfds, const struct timespec *__timeout, const sigset_t *__sigmask)
{
	if (!g_p_fd_collection) {
		BULLSEYE_EXCLUDE_BLOCK_START
		if (!orig_os_api.ppoll) get_orig_funcs();
		BULLSEYE_EXCLUDE_BLOCK_END
		return orig_os_api.ppoll(__fds, __nfds, __timeout, __sigmask);
	}

	int timeout = (__timeout == NULL) ? -1 :
	           (__timeout->tv_sec * 1000 + __timeout->tv_nsec / 1000000);

	srdr_logfunc_entry("nfds=%d, timeout=(%d milli-sec)", __nfds, timeout);

	return poll_helper(__fds, __nfds, timeout, __sigmask);
}

#if defined HAVE___PPOLL_CHK
extern "C"
EXPORT_SYMBOL
int __ppoll_chk(struct pollfd *__fds, nfds_t __nfds, const struct timespec *__timeout, const sigset_t *__sigmask, size_t __fdslen)
{
	if (!g_p_fd_collection) {
		BULLSEYE_EXCLUDE_BLOCK_START
		if (!orig_os_api.__ppoll_chk) get_orig_funcs();
		BULLSEYE_EXCLUDE_BLOCK_END
		return orig_os_api.__ppoll_chk(__fds, __nfds, __timeout, __sigmask, __fdslen);
	}

	BULLSEYE_EXCLUDE_BLOCK_START
	if (__fdslen / sizeof(*__fds) < __nfds) {
	    srdr_logpanic("buffer overflow detected");
	}

	BULLSEYE_EXCLUDE_BLOCK_END

	int timeout = (__timeout == NULL) ? -1 :
	           (__timeout->tv_sec * 1000 + __timeout->tv_nsec / 1000000);

	srdr_logfunc_entry("nfds=%d, timeout=(%d milli-sec)", __nfds, timeout);

	return poll_helper(__fds, __nfds, timeout, __sigmask);
}
#endif

static void vma_epoll_create(int epfd, int size)
{
	if (g_p_fd_collection) {
		// Sanity check to remove any old sockinfo object using the same fd!!
		handle_close(epfd, true);

		// insert epfd to fd_collection as epfd_info
		g_p_fd_collection->addepfd(epfd, size);
	}
}

/* Creates an epoll instance.  Returns fd for the new instance.
   The "size" parameter is a hint specifying the number of file
   descriptors to be associated with the new instance.  The fd
   returned by epoll_create() should be closed with close().  */
extern "C"
EXPORT_SYMBOL
int epoll_create(int __size)
{
	DO_GLOBAL_CTORS();

	if (__size <= 0 ) {
		srdr_logdbg("invalid size (size=%d) - must be a positive integer\n", __size);
		errno = EINVAL;
		return -1;
	}

	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.epoll_create) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	int epfd = orig_os_api.epoll_create(__size + 1);  // +1 for the cq epfd
	srdr_logdbg("ENTER: (size=%d) = %d\n", __size, epfd);

	if (epfd <=0)
		return epfd;

	vma_epoll_create(epfd, 8);

	return epfd;
}

extern "C"
EXPORT_SYMBOL
int epoll_create1(int __flags)
{
	DO_GLOBAL_CTORS();

	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.epoll_create1) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	int epfd = orig_os_api.epoll_create1(__flags);
	srdr_logdbg("ENTER: (flags=%d) = %d\n", __flags, epfd);

	if (epfd <=0)
		return epfd;

	vma_epoll_create(epfd, 8);

	return epfd;
}

/* Manipulate an epoll instance "epfd". Returns 0 in case of success,
   -1 in case of error ("errno" variable will contain the specific
   error code). The "op" parameter is one of the EPOLL_CTL_*
   constants defined above. The "fd" parameter is the target of the
   operation. The "event" parameter describes which events the caller
   is interested in and any associated user data.  */
extern "C"
EXPORT_SYMBOL
int epoll_ctl(int __epfd, int __op, int __fd, struct epoll_event *__event)
{
	const static char * op_names[] = {
	     "<null>",
	     "ADD",
	     "DEL",
	     "MOD"
	};
	NOT_IN_USE(op_names); /* to suppress warning in case VMA_MAX_DEFINED_LOG_LEVEL */
	if (__event) {
		srdr_logfunc_entry("epfd=%d, op=%s, fd=%d, events=%#x, data=%x", 
			__epfd, op_names[__op], __fd, __event->events, __event->data.u64);
	}
	else {
		srdr_logfunc_entry("epfd=%d, op=%s, fd=%d, event=NULL", __epfd, op_names[__op], __fd);
	}

	int rc = -1;
	epfd_info *epfd_info = fd_collection_get_epfd(__epfd);
	if (!epfd_info) {
		errno = EBADF;
	}
	else {
		// TODO handle race - if close() gets here..
		rc = epfd_info->ctl(__op, __fd, __event);
	}
	
	srdr_logfunc_exit("rc = %d", rc);
	return rc;
}

/* Wait for events on an epoll instance "epfd". Returns the number of
   triggered events returned in "events" buffer. Or -1 in case of
   error with the "errno" variable set to the specific error code. The
   "events" parameter is a buffer that will contain triggered
   events. The "maxevents" is the maximum number of events to be
   returned ( usually size of "events" ). The "timeout" parameter
   specifies the maximum wait time in milliseconds (-1 == infinite).  */
inline int epoll_wait_helper(int __epfd, struct epoll_event *__events, int __maxevents, int __timeout, const sigset_t *__sigmask = NULL)
{
	if (__maxevents <= 0 || __maxevents > EP_MAX_EVENTS) {
		srdr_logdbg("invalid value for maxevents: %d", __maxevents);
		errno = EINVAL;
		return -1;
	}

	epoll_event extra_events_buffer[__maxevents];

	try {
		epoll_wait_call epcall(extra_events_buffer, NULL,
				__epfd, __events, __maxevents, __timeout, __sigmask);

		int rc = epcall.get_current_events(); // returns ready nfds
		if (rc <= 0) {
			// if no ready nfds available then check all lower level queues (VMA ring's and OS queues)
			epcall.init_offloaded_fds();
			rc = epcall.call();
		}

		srdr_logfunc_exit("rc = %d", rc);
		return rc;
	}
	catch (io_mux_call::io_error&) {
		srdr_logfunc_exit("io_mux_call::io_error (errno=%d %m)", errno);
		return -1;
	}
}

extern "C"
EXPORT_SYMBOL
int epoll_wait(int __epfd, struct epoll_event *__events, int __maxevents, int __timeout)
{
	srdr_logfunc_entry("epfd=%d, maxevents=%d, timeout=(%d milli-sec)", __epfd, __maxevents, __timeout);

	return epoll_wait_helper(__epfd, __events, __maxevents, __timeout);
}

extern "C"
EXPORT_SYMBOL
int epoll_pwait(int __epfd, struct epoll_event *__events, int __maxevents, int __timeout, const sigset_t *__sigmask)
{
	srdr_logfunc_entry("epfd=%d, maxevents=%d, timeout=(%d milli-sec)", __epfd, __maxevents, __timeout);

	return epoll_wait_helper(__epfd, __events, __maxevents, __timeout, __sigmask);
}

/* Create two new sockets, of type TYPE in domain DOMand using
   protocol PROTOCOL, which are connected to each other, and put file
   descriptors for them in FDS[0] and FDS[1].  If PROTOCOL is zero,
   one will be chosen automatically.  Returns 0 on success, -1 for errors.  */
extern "C"
EXPORT_SYMBOL
int socketpair(int __domain, int __type, int __protocol, int __sv[2])
{
	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.socketpair) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	int ret = orig_os_api.socketpair(__domain, __type, __protocol, __sv);

	srdr_logdbg("(domain=%s(%d) type=%s(%d) protocol=%d, fd[%d,%d]) = %d\n", socket_get_domain_str(__domain), __domain, socket_get_type_str(__type), __type, __protocol, __sv[0], __sv[1], ret);

	// Sanity check to remove any old sockinfo object using the same fd!!
	if (ret == 0 && g_p_fd_collection) {
		handle_close(__sv[0], true);
		handle_close(__sv[1], true);
	}

	return ret;
}

/* Create a one-way communication channel (pipe).
   If successful, two file descriptors are stored in PIPEDES;
   bytes written on PIPEDES[1] can be read from PIPEDES[0].
   Returns 0 if successful, -1 if not.  */
extern "C"
EXPORT_SYMBOL
int pipe(int __filedes[2])
{
	bool offload_pipe = safe_mce_sys().mce_spec == MCE_SPEC_29WEST_LBM_29 ||
			    safe_mce_sys().mce_spec == MCE_SPEC_WOMBAT_FH_LBM_554;
	if (offload_pipe)
		DO_GLOBAL_CTORS();

	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.pipe) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	int ret = orig_os_api.pipe(__filedes);
	srdr_logdbg("(fd[%d,%d]) = %d\n", __filedes[0], __filedes[1], ret);

	if (ret == 0 && g_p_fd_collection) {
		// Sanity check to remove any old sockinfo object using the same fd!!
		int fdrd = __filedes[0];
		handle_close(fdrd, true);
		int fdwr = __filedes[1];
		handle_close(fdwr, true);

		// Create new pipeinfo object for this new fd pair
		if (offload_pipe)
			g_p_fd_collection->addpipe(fdrd, fdwr);
	}

	return ret;
}

extern "C"
EXPORT_SYMBOL
int open(__const char *__file, int __oflag, ...)
{
	va_list va;
	va_start(va, __oflag);
	mode_t mode = va_arg(va, mode_t);

	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.open) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	int fd = orig_os_api.open(__file, __oflag, mode);
	va_end(va);

	srdr_logdbg("(file=%s, flags=%#x, mode=%#x) = %d\n", __file, __oflag, mode, fd);

	// Sanity check to remove any old sockinfo object using the same fd!!
	handle_close(fd, true);

	return fd;
}

extern "C"
EXPORT_SYMBOL
int creat(const char *__pathname, mode_t __mode)
{
	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.creat) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	int fd = orig_os_api.creat(__pathname, __mode);

	srdr_logdbg("(pathname=%s, mode=%#x) = %d\n", __pathname, __mode, fd);

	// Sanity check to remove any old sockinfo object using the same fd!!
	handle_close(fd, true);

	return fd;
}

/* Duplicate FD, returning a new file descriptor on the same file.  */
extern "C"
EXPORT_SYMBOL
int dup(int __fd)
{
	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.dup) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	int fid = orig_os_api.dup(__fd);

	srdr_logdbg("(fd=%d) = %d\n", __fd, fid);

	// Sanity check to remove any old sockinfo object using the same fd!!
	handle_close(fid, true);

	return fid;
}

/* Duplicate FD to FD2, closing FD2 and making it open on the same file.  */
extern "C"
EXPORT_SYMBOL
int dup2(int __fd, int __fd2)
{
	if (safe_mce_sys().close_on_dup2 && __fd != __fd2) {
		srdr_logdbg("oldfd=%d, newfd=%d. Closing %d in VMA.\n", __fd, __fd2, __fd2);
		handle_close(__fd2);
	}

	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.dup2) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	int fid = orig_os_api.dup2(__fd, __fd2);

	srdr_logdbg("(fd=%d, fd2=%d) = %d\n",  __fd, __fd2, fid);

	// Sanity check to remove any old sockinfo object using the same fd!!
	handle_close(fid, true);

	return fid;
}

#ifdef _CHANGE_CLONE_PROTO_IN_SLES_10_
extern "C"
EXPORT_SYMBOL
int clone(int (*__fn)(void *), void *__child_stack, int __flags, void *__arg)
{
	srdr_logfunc_entry("flags=%#x", __flags);

	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.clone) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	return orig_os_api.clone(__fn, __child_stack, __flags, __arg);
}
#endif

/* Clone the calling process, creating an exact copy.
   Return -1 for errors, 0 to the new process,
   and the process ID of the new process to the old process.  */

extern "C"
EXPORT_SYMBOL
pid_t fork(void)
{
	srdr_logdbg("ENTER: **********\n");

	if (!g_init_global_ctors_done) {
		set_env_params();
		prepare_fork();
	}

	if (!g_init_ibv_fork_done)
		srdr_logdbg("ERROR: ibv_fork_init failed, the effect of an application calling fork() is undefined!!\n");

	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.fork) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	pid_t pid = orig_os_api.fork();
	if (pid == 0) {
		g_is_forked_child = true;
		srdr_logdbg_exit("Child Process: returned with %d", pid);
		// Child's process - restart module
		vlog_stop();


		// In case of child process, we want all global objects to re-construct
		reset_globals();

		g_init_global_ctors_done = false;
		sock_redirect_exit();

		safe_mce_sys().get_env_params();
		vlog_start("VMA", safe_mce_sys().log_level, safe_mce_sys().log_filename, safe_mce_sys().log_details, safe_mce_sys().log_colors);
		if (vma_rdma_lib_reset()) {
			srdr_logerr("Child Process: rdma_lib_reset failed %d %s",
					errno, strerror(errno));
		}
		srdr_logdbg_exit("Child Process: starting with %d", getpid());
		g_is_forked_child = false;
		sock_redirect_main();
	}
	else if (pid > 0) {
		srdr_logdbg_exit("Parent Process: returned with %d", pid);
	}
	else {
		srdr_logdbg_exit("failed (errno=%d %m)", errno);
	}

	return pid;
}

/* Redirect vfork to fork  */
extern "C"
EXPORT_SYMBOL
pid_t vfork(void)
{
	return fork();
}

/* Put the program in the background, and dissociate from the controlling
   terminal.  If NOCHDIR is zero, do `chdir ("/")'.  If NOCLOSE is zero,
   redirects stdin, stdout, and stderr to /dev/null.  */
extern "C"
EXPORT_SYMBOL
int daemon(int __nochdir, int __noclose)
{
	srdr_logdbg("ENTER: ***** (%d, %d) *****\n", __nochdir, __noclose);

	if (!g_init_global_ctors_done) {
		set_env_params();
		prepare_fork();
	}

	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.daemon) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	int ret = orig_os_api.daemon(__nochdir, __noclose);
	if (ret == 0) {
		g_is_forked_child = true;
		srdr_logdbg_exit("returned with %d", ret);

		// Child's process - restart module
		vlog_stop();

		// In case of child process, we want all global objects to re-construct
		reset_globals();

		g_init_global_ctors_done = false;
		sock_redirect_exit();

		safe_mce_sys().get_env_params();
		vlog_start("VMA", safe_mce_sys().log_level, safe_mce_sys().log_filename, safe_mce_sys().log_details, safe_mce_sys().log_colors);
		if (vma_rdma_lib_reset()) {
			srdr_logerr("Child Process: rdma_lib_reset failed %d %s",
					errno, strerror(errno));
		}
		srdr_logdbg_exit("Child Process: starting with %d", getpid());
		g_is_forked_child = false;
		sock_redirect_main();
	}
	else {
		srdr_logdbg_exit("failed (errno=%d %m)", errno);
	}
	return ret;
}

static void handler_intr(int sig)
{
	switch (sig) {
	case SIGINT:
		g_b_exit = true;
		srdr_logdbg("Catch Signal: SIGINT (%d)\n", sig);
		break;
	default:
		srdr_logdbg("Catch Signal: %d\n", sig);
		break;
	}

	if (g_act_prev.sa_handler)
		g_act_prev.sa_handler(sig);
}

extern "C"
EXPORT_SYMBOL
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{
	int ret = 0;

	BULLSEYE_EXCLUDE_BLOCK_START
	if (!orig_os_api.sigaction) get_orig_funcs();
	BULLSEYE_EXCLUDE_BLOCK_END

	if (safe_mce_sys().handle_sigintr) {
		srdr_logdbg_entry("signum=%d, act=%p, oldact=%p", signum, act, oldact);

		switch (signum) {
		case SIGINT:
			if (oldact && g_act_prev.sa_handler) {
				*oldact = g_act_prev;
			}
			if (act) {
				struct sigaction vma_action;
				vma_action.sa_handler = handler_intr;
				vma_action.sa_flags = 0;
				sigemptyset(&vma_action.sa_mask);

				ret = orig_os_api.sigaction(SIGINT, &vma_action, NULL);

				if (ret < 0) {
					srdr_logdbg("Failed to register VMA SIGINT handler, calling to original sigaction handler\n");
					break;
				}
				srdr_logdbg("Registered VMA SIGINT handler\n");
				g_act_prev = *act;
			}
			if (ret >= 0)
				srdr_logdbg_exit("returned with %d", ret);
			else
				srdr_logdbg_exit("failed (errno=%d %m)", errno);

			return ret;
			break;
		default:
			break;
		}
	}
	ret = orig_os_api.sigaction(signum, act, oldact);

	if (safe_mce_sys().handle_sigintr) {
		if (ret >= 0)
			srdr_logdbg_exit("returned with %d", ret);
		else
			srdr_logdbg_exit("failed (errno=%d %m)", errno);
	}
	return ret;
}

static void handle_signal(int signum)
{
	srdr_logdbg_entry("Caught signal! signum=%d", signum);

	if (signum == SIGINT) {
		g_b_exit = true;
	}

	if (g_sighandler) {
		g_sighandler(signum);
	}
}

extern "C"
EXPORT_SYMBOL
sighandler_t signal(int signum, sighandler_t handler)
{
	if (!orig_os_api.signal) get_orig_funcs();

	if (safe_mce_sys().handle_sigintr) {
		srdr_logdbg_entry("signum=%d, handler=%p", signum, handler);

		if (handler && handler != SIG_ERR && handler != SIG_DFL && handler != SIG_IGN) {
			// Only SIGINT is supported for now
			if (signum == SIGINT) {
				g_sighandler = handler;
				return orig_os_api.signal(SIGINT, &handle_signal);
			}
		}
	}

	return orig_os_api.signal(signum, handler);
}



//zc add
//Socket_transbridge impl
Socket_transbridge::Socket_transbridge(int fd){
	m_fd = fd;
}

Socket_transbridge::~Socket_transbridge(){

}




//Socket_tb_tcp impl
Socket_tb_tcp::Socket_tb_tcp(int fd) 
    : Socket_transbridge(fd) 
{
    // 验证这是一个TCP socket
    
    setType(SOCKET_TYPE_TCP);
    // 其他TCP特有的初始化，如设置TCP_NODELAY等
}

Socket_tb_tcp::~Socket_tb_tcp(){

}



int Socket_tb_tcp::socket(){
	
	return m_fd;
}

int Socket_tb_tcp::accept(){

	
	if(g_p_conn_collection){
		g_p_conn_collection->add_sorconn(m_fd);
	}

	SoR_connection* p_sor_conn = sorconn_collection_get_conn(m_fd);
	p_sor_conn->connect_to_peer();

	//p_sor_conn->post_receive();


	return m_fd;
}


int Socket_tb_tcp::connect(){
	//如何区分144和155，如何让各自找到自己对应的gid index，这是一个问题

	if(g_p_conn_collection){
		g_p_conn_collection->add_sorconn(m_fd);
	}

	SoR_connection* p_sor_conn = sorconn_collection_get_conn(m_fd);
	p_sor_conn->connect_to_peer();

	//p_sor_conn->post_receive();

	return m_fd;
}

int Socket_tb_tcp::bind(){
	
	
	return 0;
}

int Socket_tb_tcp::listen(int backlog){
	
	if(backlog){

	}
	islistenserver = true;
	return 0;
}

ssize_t Socket_tb_tcp::write(__const void *__buf, size_t __nbytes) {
    return tx(__buf, __nbytes, 0);
}


ssize_t Socket_tb_tcp::read(void *__buf, size_t __nbytes){
	return rx(__buf, __nbytes, 0);
}


ssize_t Socket_tb_tcp::rx(void *__buf, size_t __nbytes, int __flags){
    size_t total_read = 0;
    size_t need_to_read;
    SoR_connection* p_sor_conn = sorconn_collection_get_conn(m_fd);
    
    if (p_sor_conn == nullptr) {
        return orig_os_api.recv(m_fd, __buf, __nbytes, __flags);
    }
    
    // 检查当前数据量
    size_t recved_sz = p_sor_conn->m_recv_rb->total_size();
    
    if (recved_sz > 0) {
        goto process_recv_data;
    } 
    else {
        p_sor_conn->poll_recv_completion();
        recved_sz = p_sor_conn->m_recv_rb->total_size();
        if (recved_sz > 0) {
            goto process_recv_data;
        }
        else {
            // 等待数据到达（带超时）
            p_sor_conn->m_recv_rb->wait_for_data(1, 5000); // 5秒超时
            
            // 再次检查数据量
            recved_sz = p_sor_conn->m_recv_rb->total_size();
            if (recved_sz > 0) {
                goto process_recv_data;
            }
            else {
                // 等待超时或数据仍然不足，读取现有数据，应该是没数据
                need_to_read = recved_sz;
                if (need_to_read == 0) {
                    // 没有数据，根据是否阻塞设置错误码
                    if (__flags & MSG_DONTWAIT) {
                        errno = EAGAIN;
                        return -1;
                    }
                    // 对于阻塞socket，返回0表示连接关闭？
                    return 0;
                }
				else{
					printf("may be some wrong , shouldn't hapen head\n");
				}
            }
        }
    }

process_recv_data:
    // 处理完整的数据
	need_to_read = __nbytes > recved_sz ? recved_sz : __nbytes;
    
	p_sor_conn->m_recv_rb->read(__buf, need_to_read);

	total_read += need_to_read;

	p_sor_conn->post_receive_for_recv_window();

	if(p_sor_conn->get_recv_window() <= 10*1024*1024){//当发送方维护的接收缓冲区小于10MB时，更新
		p_sor_conn->update_my_recv_window_add(total_read);
		p_sor_conn->sync_remote_recv_window();
	}
	else{
		p_sor_conn->update_my_recv_window_add(total_read);
	}
    return total_read;
}

ssize_t Socket_tb_tcp::tx(__const void *__buf, size_t __nbytes, int __flags){
	SoR_connection* p_sor_conn = sorconn_collection_get_conn(m_fd);
    ssize_t return_sz = 0;
	size_t remote_recv_buf;
    if (p_sor_conn) {
        size_t total_write = 0;
        
        try {
            // 检查当前可用空间send buf
            size_t available = p_sor_conn->get_send_buf();

			//这里的逻辑也要修改
			//1. 判断发送缓冲区的available
			//2. 够用就写入发送缓冲区
			//3. 判断remote的接收缓冲区的窗口
			//4. 看情况调用ibv_post_send 如果够调用，如果不够，应该直接返回，让轮询线程来更新remote recv buf
			//5. 如果send buf和recv buf都不够，那么阻塞sockt就应该陷入阻塞，等对端来更新remote recv buf

            if (available >= __nbytes) {
                // send buf有足够空间，写入数据
				p_sor_conn->write_data_in_send_buf(__buf, __nbytes);
                total_write += __nbytes;
            } else {
				//send buf空间不足，不应该直接等待，而是先看接收缓冲区有没有可能已经更新了，如果更新了那么应该先发送一次数据
				remote_recv_buf = p_sor_conn->get_remote_recv_buf();
				if(remote_recv_buf > 0){
					p_sor_conn->post_send_notify_with_imm();
				}
				//这里应该poll send cq更新发送缓冲区一次
				p_sor_conn->wait_send_buf(1); // 

				available = p_sor_conn->get_send_buf();

				if (available >= __nbytes) {
					p_sor_conn->write_data_in_send_buf(__buf, __nbytes);
					total_write += __nbytes;
				}
				else{
					// 还是不够，就说明send buf也满，remote recv buf也满了，所以应该等对端更新remote recv buf
					p_sor_conn->wait_remote_recv_buf();// 如果是阻塞的话，这个时间照理来说要非常长 轮询线程必须启动，或者说wait期间也让他去轮询
					remote_recv_buf = p_sor_conn->get_remote_recv_buf();
					if(remote_recv_buf > 0){
						p_sor_conn->post_send_notify_with_imm();
					}
                	p_sor_conn->wait_send_buf(__nbytes); 
					available = p_sor_conn->get_send_buf();
	
                	if (available >= __nbytes) {
                    	p_sor_conn->write_data_in_send_buf(__buf, __nbytes);
						total_write += __nbytes;
                	} else {
                    // 虽然等待了，但可能只获得了部分空间
                   		p_sor_conn->write_data_in_send_buf(__buf, available);
						total_write += available;
                	}
				}
            }
            return_sz = total_write;

			//write data end ,start to send data
			remote_recv_buf = p_sor_conn->get_remote_recv_buf();
			if(remote_recv_buf > 0){
				p_sor_conn->post_send_notify_with_imm();
			}
			else{
				//don't ibv_post_send and return 
				return return_sz;
			}
        } 
        catch (const std::exception& e) {
            fprintf(stderr, "Send failed: %s\n", e.what());
            return_sz = -1;
            errno = EAGAIN;  // 或者合适的错误码
        }
    } 
	else 
	{
        // 回退到原始系统调用
        return_sz = orig_os_api.send(m_fd, __buf, __nbytes, __flags);
    }

    return return_sz;
}




//如果是非阻塞send，看发送缓冲区有多大，能拷贝多少拷贝多少，然后立即返回
//如果是阻塞send，看发送缓冲区有多大，不够的话就poll cq，直到有足够的发送缓冲区然后拷贝到发送缓冲区然后返回
//现在是只考虑阻塞情况
ssize_t Socket_tb_tcp::send(__const void *__buf, size_t __nbytes, int __flags) {
    if(__flags){

	}
	
	return tx(__buf, __nbytes, __flags);
}

//之前的理解有误
//阻塞模式：recv会等待直到有数据可读（至少1字节）或者出错或连接关闭
//但返回的数据可能小于请求的字节数（只要当前有数据，就会返回当前可用的数据，最多不超过指定的缓冲区大小）。
//非阻塞模式：recv会立即返回当前可用的数据，即使比请求的少，也不会等待。如果没有数据，则返回错误。
//！！！阻塞模式下并不一定是要缓冲区的字节满足__nbytes的大小才会返回
ssize_t Socket_tb_tcp::recv(void *__buf, size_t __nbytes, int __flags) {
	if(__flags){

	}
    return rx(__buf, __nbytes, __flags);
}

//Socket_tb_udp impl
Socket_tb_udp::Socket_tb_udp(int fd) 
    : Socket_transbridge(fd)  // 调用基类构造函数
{
    setType(SOCKET_TYPE_UDP);
	m_rdma_initialized = false;
    // 子类特有的初始化代码
}

Socket_tb_udp::~Socket_tb_udp(){

}

//udp和tcp不同，udp在申请socket以后可能就会马上调用sendto来发送数据了，逻辑上就需要一申请socket以后立马分配rdma资源，
//只要检测到应用程序申请的socket类型是sock——dgram数据报类型的就立马申请ud qp的rdma资源，
//（实际上一个进程可以只使用一个ud qp，现在只考虑最简单的通信情况）
int Socket_tb_udp::socket(){
	return 0 ;
}

//这是对于udp服务器端的，需要绑定到某一个ip地址和port然后再准备接收数据
// 在bind函数中开始注册rdma资源，因为每一个socket最终都会绑定到某一个ip和port，在这一步注册最合理
int Socket_tb_udp::bind() {
    // 先看这个socket有没有bind
	if(isBound()){
		return 0;
	}

    // 如果没有绑定，获取绑定的本地地址信息
    struct sockaddr_in local_addr;
    socklen_t local_len = sizeof(local_addr);
    if (orig_os_api.getsockname(m_fd, (struct sockaddr*)&local_addr, &local_len) < 0) {
        std::cerr << "Failed to get socket name: " << strerror(errno) << std::endl;
        return -1;
    }
    
	extractAddressFromSockaddr(&local_addr);

	int result = 0;
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &local_addr.sin_addr, ip_str, sizeof(ip_str));
    uint16_t port = ntohs(local_addr.sin_port);
    
    std::cout << "UDP socket bound to " << ip_str << ":" << port << std::endl;
    
    // 初始化 RDMA 管理器
    if (!initRdmaManager(local_addr.sin_addr.s_addr, local_addr.sin_port, m_fd)) {
        std::cerr << "Failed to initialize RDMA manager, falling back to regular UDP" << std::endl;
        // 即使 RDMA 初始化失败，仍然返回成功，因为 UDP socket 绑定成功
        // 后续会使用普通 UDP 通信
        return result;
    }

    

    m_rdma_initialized = true;
    std::cout << "RDMA manager initialized successfully after bind" << std::endl;
    
    return result;
}

bool Socket_tb_udp::initRdmaManager(uint32_t local_ip, uint16_t local_port, int sockfd) {
    // 初始化RDMA管理器
    if (!m_rdma_manager->initialize(local_ip, local_port, sockfd)) {
        std::cerr << "Failed to initialize RDMA manager: " << m_rdma_manager->getLastError() << std::endl;
        return false;
    }

	m_pd = m_rdma_manager->getPd();  // 需要UDRdmaManager添加这个方法
    
    return true;
}


//note：按理来说，如果是udp传数据的话应该需要把这次我们通信的ip地址和端口号一起发送到对端
ssize_t Socket_tb_udp::sendto(__const void *__buf, size_t __nbytes, int __flags,
	       const struct sockaddr *__to, socklen_t __tolen){
	//1. 先查ip和port有没有记录，是不是有通信过
	//2. 通过ip和port和对端尝试通过udp通信
	//3. 交换qpn
	//4. 要记录qpn和ip地址和port的对应关系

	if (!__buf || __nbytes == 0 || !__to || __tolen != sizeof(sockaddr_in)) {
        errno = EINVAL;
        return -1;
    }
    
    const sockaddr_in* to_addr = reinterpret_cast<const sockaddr_in*>(__to);
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &to_addr->sin_addr, ip_str, sizeof(ip_str));
    int port = ntohs(to_addr->sin_port);
    
    std::cout << "sendto: Sending to " << ip_str << ":" << port 
              << ", data size: " << __nbytes << std::endl;
    
    // 检查是否已经和对端交换过元数据
    auto& peer_manager = GlobalPeerManager::instance();
    const PeerInfo* peer = peer_manager.get_peer(ip_str, port);
    
    if (!peer) {
        // 第一次通信，需要交换RDMA元数据
        std::cout << "First communication with " << ip_str << ":" << port 
                  << ", exchanging RDMA metadata..." << std::endl;
        
        // 在这里调用 establish_rdma_connection
        if (!establish_rdma_connection(ip_str, port, __to)) {
            std::cerr << "Failed to establish RDMA connection, falling back to normal UDP" << std::endl;
            // 如果建立RDMA连接失败，回退到普通UDP发送
            return orig_os_api.sendto(m_fd, __buf, __nbytes, __flags, __to, __tolen);
        }
         
        std::cout << "RDMA metadata exchange completed with " << ip_str << ":" << port << std::endl;
    }
	// 应该是先调用内核的sendto，让内核绑定到一个端口，然后再出来了才知道自己的ip地址和端口

	// 确保RDMA管理器已初始化
    if (!m_rdma_manager) {
        // 如果没有初始化，尝试使用本地地址初始化
		// 这里应该调用bind初始化，即使是有对端信息，但是这个socket可能是第一次调用，这样的话他就还没有进行rdma的初始化
		// TODO: bind（）
		Socket_tb_udp::bind();
    }
	    
    // 1. 检查数据大小是否超过块的数据容量
    if (__nbytes > m_rdma_manager->send_buffer().data_capacity()) {
        std::cerr << "Data size " << __nbytes << " exceeds block capacity " 
                  << m_rdma_manager->send_buffer().data_capacity() << std::endl;
        errno = EMSGSIZE;
        return -1;
    }
    
    // 2. 将数据写入发送缓冲区
    if (!m_rdma_manager->send_buffer().write_block(__buf, __nbytes)) {
        std::cerr << "Failed to write data to send buffer, buffer may be full" << std::endl;
        errno = EAGAIN;
        return -1;
    }
    
    // 3. 使用RDMA UD发送数据
    bool rdma_success = post_send_to_peer(peer, __nbytes);
    
    if (!rdma_success) {
        std::cerr << "RDMA send failed, falling back to normal UDP" << std::endl;
        // 回退到普通UDP发送
        return orig_os_api.sendto(m_fd, __buf, __nbytes, __flags, __to, __tolen);
    }
    
    return __nbytes;

}



bool Socket_tb_udp::establish_rdma_connection(const char* remote_ip, int remote_port,
                                             const struct sockaddr* to_addr) {
    std::cout << "Establishing RDMA connection to " << remote_ip << ":" << remote_port << std::endl;
    
    // 1. 检查是否已经绑定过，如果没有绑定，则需要先初始化RDMA资源
    if (!m_rdma_initialized) {
        // 获取当前socket的状态，检查是否已经绑定
        struct sockaddr_in test_addr;

        BULLSEYE_EXCLUDE_BLOCK_START
        if (!orig_os_api.getsockname) get_orig_funcs();
        BULLSEYE_EXCLUDE_BLOCK_END

        socklen_t test_len = sizeof(test_addr);
        if (orig_os_api.getsockname(m_fd, (struct sockaddr*)&test_addr, &test_len) == 0) {
            // 如果已经绑定，获取本地地址
            char local_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &test_addr.sin_addr, local_ip, sizeof(local_ip));
            uint16_t local_port = ntohs(test_addr.sin_port);
            
            // 使用已有的本地地址初始化RDMA资源
            if (!initRdmaManager(test_addr.sin_addr.s_addr, local_port, m_fd )) {
                std::cerr << "Failed to initialize RDMA manager with existing bind" << std::endl;
                return false;
            }
        } else {
            // 还没有绑定，我们需要先初始化RDMA资源（使用临时地址）
            std::cout << "Socket not yet bound, initializing RDMA with temporary address" << std::endl;
            
            // 使用0.0.0.0:0作为临时地址初始化RDMA资源
            struct sockaddr_in temp_addr;
            temp_addr.sin_family = AF_INET;
            temp_addr.sin_addr.s_addr = htonl(INADDR_ANY);
            temp_addr.sin_port = 10000;  // 让内核分配端口
            
            if (!initRdmaManager(temp_addr.sin_addr.s_addr, temp_addr.sin_port, m_fd)) {// TODO : QP state modify
                std::cerr << "Failed to initialize RDMA manager with temporary address" << std::endl;
                return false;
            }
            
            std::cout << "RDMA resources initialized with temporary address" << std::endl;
        }
        
        m_rdma_initialized = true;
    }
    
    // 2. 发送本地的RDMA元数据
    if (!send_metadata(m_fd, to_addr, sizeof(sockaddr_in))) {
        std::cerr << "Failed to send RDMA metadata" << std::endl;
        return false;
    }
    
    std::cout << "Sent local RDMA metadata to " << remote_ip << ":" << remote_port << std::endl;
    
    // 3. 接收对端的RDMA元数据
    RDMA_Metadata remote_metadata;
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);
    
    if (!receive_metadata(m_fd, (struct sockaddr*)&from_addr, &from_len, remote_metadata)) {
        std::cerr << "Failed to receive RDMA metadata" << std::endl;
        return false;
    }
    
    // 4. 验证源地址
    char from_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &from_addr.sin_addr, from_ip, sizeof(from_ip));
    int from_port = ntohs(from_addr.sin_port);
    
    if (strcmp(from_ip, remote_ip) != 0 || from_port != remote_port) {
        std::cerr << "Unexpected source address: " << from_ip << ":" << from_port 
                  << ", expected: " << remote_ip << ":" << remote_port << std::endl;
        return false;
    }
    
    // 打印GID（十六进制格式）
    std::string gid_str;
    for (int i = 0; i < 16; i++) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", remote_metadata.gid[i]);
        gid_str += buf;
        if (i % 2 == 1 && i != 15) gid_str += ":";
    }
    
    std::cout << "Received RDMA metadata from " << from_ip << ":" << from_port 
              << " (QPN: " << remote_metadata.qpn 
              << ", GID: " << gid_str
              << ", QKey: " << remote_metadata.qkey << ")" << std::endl;
    
    // 5. 获取内核分配的本地地址（如果之前没有绑定）
    struct sockaddr_in local_addr;
    socklen_t local_len = sizeof(local_addr);
    if (orig_os_api.getsockname(m_fd, (struct sockaddr*)&local_addr, &local_len) == 0) {
        char local_ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &local_addr.sin_addr, local_ip_str, sizeof(local_ip_str));
        uint16_t local_port = ntohs(local_addr.sin_port);
        
        std::cout << "Socket bound to " << local_ip_str << ":" << local_port 
                  << " (assigned by kernel)" << std::endl;
        
        // 6. 如果需要，更新RDMA管理器中的本地地址信息
        updateRdmaLocalAddress(local_addr.sin_addr.s_addr, local_addr.sin_port);
    }
    
    auto& peer_manager = GlobalPeerManager::instance();

    // 7. 添加对端信息到全局映射
    peer_manager.add_peer(remote_ip, remote_port, remote_metadata, m_pd);
    
    // 8. 获取对端的地址句柄（如果不存在则创建）
    ibv_ah* ah = peer_manager.get_or_create_ah(remote_ip, remote_port, m_pd, remote_metadata.port_num);
    if (!ah) {
        std::cerr << "Failed to create AH for peer" << std::endl;
        return false;
    }
    
    std::cout << "RDMA connection established with " << remote_ip << ":" << remote_port << std::endl;
    
    return true;
}

// 发送元数据到对端
bool Socket_tb_udp::send_metadata(int sockfd, const sockaddr* to, socklen_t tolen) {
    // 获取本地RDMA元数据
    RDMA_Metadata local_metadata;
    local_metadata.qpn = m_rdma_manager->getQpNum();
    local_metadata.port_num = m_rdma_manager->getPortNum();
    local_metadata.qkey = m_rdma_manager->getQkey();
    // 获取本地GID
    m_rdma_manager->getGid(local_metadata.gid);  // 需要UDRdmaManager添加这个方法
    
    // 序列化元数据
    char metadata_buffer[RDMA_Metadata::serialized_size()];
    local_metadata.serialize(reinterpret_cast<uint8_t*>(metadata_buffer));
    
    // 获取原始函数指针（如果需要）
    BULLSEYE_EXCLUDE_BLOCK_START
    if (!orig_os_api.sendto) get_orig_funcs();
    BULLSEYE_EXCLUDE_BLOCK_END
    
    // 发送元数据
    ssize_t sent = orig_os_api.sendto(sockfd, metadata_buffer, sizeof(metadata_buffer), 0, to, tolen);
    return sent == sizeof(metadata_buffer);
}

// 接收元数据从对端
bool Socket_tb_udp::receive_metadata(int sockfd, sockaddr* from, socklen_t* fromlen, RDMA_Metadata& metadata) {
    char metadata_buffer[RDMA_Metadata::serialized_size()];
    
    // 获取原始函数指针（如果需要）
    BULLSEYE_EXCLUDE_BLOCK_START
    if (!orig_os_api.recvfrom) get_orig_funcs();
    BULLSEYE_EXCLUDE_BLOCK_END
    
    // 接收元数据
    ssize_t recv_len = orig_os_api.recvfrom(sockfd, metadata_buffer, sizeof(metadata_buffer), 0, from, fromlen);
    if (recv_len != sizeof(metadata_buffer)) {
        return false;
    }
    
    // 反序列化元数据
    metadata.deserialize(reinterpret_cast<const uint8_t*>(metadata_buffer));
    return true;
}


void Socket_tb_udp::updateRdmaLocalAddress(uint32_t new_ip, uint16_t new_port) {
    if (!m_rdma_manager) {
        return;
    }
    
    uint32_t new_ip_host = ntohl(new_ip);
    uint16_t new_port_host = ntohs(new_port);
    
    std::cout << "Updating RDMA local address to: " 
              << new_ip_host << ":" << new_port_host << std::endl;
    
    // 这里需要更新RDMA管理器中的本地地址
    // 注意：如果SequentialUdpBuffer已经创建，可能需要重新创建
    // 或者添加一个更新地址的方法
    m_rdma_manager->updateLocalAddress(new_ip_host, new_port_host);
}


bool Socket_tb_udp::post_send_to_peer(const PeerInfo* peer, size_t data_len) {
    if (!m_rdma_manager || !peer) {
        return false;
    }
    
    // 1. 获取发送缓冲区的下一个可读取块的数据指针
    const unsigned char* data_ptr = m_rdma_manager->send_buffer().get_tail_ptr();
    if (!data_ptr) {
        std::cerr << "No data available in send buffer" << std::endl;
        return false;
    }
    
    // 2. 获取发送缓冲区的MR信息
    auto mr_info = m_rdma_manager->get_memory_regions();
    if (!mr_info.send_mr) {
        std::cerr << "Send MR not registered" << std::endl;
        return false;
    }
    
    // 3. 构建SGE（Scatter/Gather Element）
    struct ibv_sge sge;
    sge.addr = (uintptr_t)data_ptr;
    sge.length = static_cast<uint32_t>(data_len + m_rdma_manager->send_buffer().header_size());
    sge.lkey = mr_info.send_mr->lkey;
    
    // 4. 构建Send Work Request
    struct ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));

	static std::atomic<uint64_t> wr_id_counter{0};  // 函数内静态变量
    wr.wr_id = wr_id_counter.fetch_add(1, std::memory_order_relaxed);//全局递增的wr_id

    wr.next = nullptr;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;  // UD模式使用SEND操作
    
    // UD特定字段
    wr.wr.ud.ah = peer->ah;                     // 地址句柄 
    wr.wr.ud.remote_qpn = peer->metadata.qpn;   // 从metadata获取远程QP号
    wr.wr.ud.remote_qkey = peer->metadata.qkey; // 从metadata获取QKey
    
    // 5. 发布Send Work Request
    struct ibv_send_wr* bad_wr = nullptr;
    
    ibv_qp* qp = m_rdma_manager->getQp();  // 需要添加这个方法到UDRdmaManager
    
    if (!qp) {
        std::cerr << "QP not available" << std::endl;
        return false;
    }
    
    int ret = ibv_post_send(qp, &wr, &bad_wr);
    if (ret != 0) {
        std::cerr << "Failed to post send WR: " << strerror(errno) << std::endl;
        return false;
    }
    
    return true;
}

ssize_t Socket_tb_udp::recvfrom(void *buf, size_t nbytes, int flags,
                               sockaddr *srcAddr, socklen_t *addrlen) {
    // 1. 首先检查接收缓冲区是否有数据
	
	if(m_rdma_initialized)
	{
		if(m_rdma_manager->recv_buffer().has_data()){
			// 直接从接收缓冲区读取数据
        	ssize_t bytes_read = m_rdma_manager->recv_buffer().read_block(
        	    buf, nbytes, srcAddr, addrlen);
	
        	if (bytes_read > 0) {
        	    // 成功从缓冲区读取数据后，发布一个新的接收WR
        	    if (m_rdma_manager->recv_buffer().available_blocks() > 0) {
        	        m_rdma_manager->post_recv(1);
        	    }
        	    return bytes_read;
        	}
	
        	// 如果bytes_read < 0，说明缓冲区太小，返回错误
        	if (bytes_read < 0) {
        	    errno = EMSGSIZE;
        	    return -1;
			}
		}    
    }
    
    // 2. 如果没有数据，根据flags决定行为
    if (flags & MSG_DONTWAIT) {
        // 非阻塞模式，立即返回
        errno = EAGAIN;
        return -1;
    }
    
    // 3. 阻塞模式：设置socket为非阻塞，然后轮询等待
    // 保存原始socket标志
    int original_flags = orig_os_api.fcntl(m_fd, F_GETFL, 0);
    if (original_flags == -1) {
        perror("fcntl(F_GETFL) failed");
        errno = EBADF;
        return -1;
    }
    
    // 临时设置为非阻塞
    if (orig_os_api.fcntl(m_fd, F_SETFL, original_flags | O_NONBLOCK) == -1) {
        perror("fcntl(F_SETFL) failed");
        errno = EBADF;
        return -1;
    }
    
    // 4. 轮询等待：交替检查接收缓冲区和UDP socket
    const int MAX_WAIT_MS = 5000;  // 最大等待5秒
    const int CHECK_INTERVAL_US = 1000;  // 每次检查间隔1ms
    int total_wait_us = 0;
    
    while (total_wait_us < MAX_WAIT_MS * 1000) {
        // 首先检查接收缓冲区
		if(m_rdma_initialized)
		{
			if(m_rdma_manager->recv_buffer().has_data()){
				// 直接从接收缓冲区读取数据
        		ssize_t bytes_read = m_rdma_manager->recv_buffer().read_block(
        		    buf, nbytes, srcAddr, addrlen);
				// 恢复socket标志
        	        orig_os_api.fcntl(m_fd, F_SETFL, original_flags);
        		if (bytes_read > 0) {
        		    // 成功从缓冲区读取数据后，发布一个新的接收WR
        		    if (m_rdma_manager->recv_buffer().available_blocks() > 0) {
        		        m_rdma_manager->post_recv(1);
        		    }
        		    return bytes_read;
        		}

        		// 如果bytes_read < 0，说明缓冲区太小，返回错误
        		if (bytes_read < 0) {
        		    errno = EMSGSIZE;
        		    return -1;
				}
			}    
    	}
        
        // 然后检查UDP socket是否有数据（非阻塞）
        char temp_buf[65536];
        sockaddr_storage temp_addr;
        socklen_t temp_addrlen = sizeof(temp_addr);
        
        ssize_t recv_len = orig_os_api.recvfrom(m_fd, temp_buf, sizeof(temp_buf), 
                                     MSG_DONTWAIT, 
                                     (sockaddr*)&temp_addr, &temp_addrlen);
        
        if (recv_len > 0) {
            // 处理UDP元数据
			sockaddr_in& src_addr_in = *reinterpret_cast<sockaddr_in*>(&temp_addr);
    		handle_udp_metadata(temp_buf, recv_len, src_addr_in, temp_addrlen);
            
            // 恢复socket标志
            orig_os_api.fcntl(m_fd, F_SETFL, original_flags);
            continue;
        } else if (recv_len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            // 真正的错误
            orig_os_api.fcntl(m_fd, F_SETFL, original_flags);
            return -1;
        }
        
        // 等待一小段时间再检查
        usleep(CHECK_INTERVAL_US);
        total_wait_us += CHECK_INTERVAL_US;
    }
    
    // 恢复socket标志
    orig_os_api.fcntl(m_fd, F_SETFL, original_flags);
    
    // 超时
    errno = EAGAIN;
    return -1;
}

// 处理UDP元数据
ssize_t Socket_tb_udp::handle_udp_metadata(char* temp_buf, ssize_t recv_len,
                                          sockaddr_in& src_addr, socklen_t src_len) {
    if(recv_len){
		//maybe not need
	}

    // 获取源地址信息
    char from_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &src_addr.sin_addr, from_ip, sizeof(from_ip));
    int from_port = ntohs(src_addr.sin_port);
    
    std::cout << "Received RDMA metadata from " << from_ip << ":" << from_port << std::endl;
    
    // 1. 解析RDMA元数据
    RDMA_Metadata metadata;
    
    try {
        metadata.deserialize(reinterpret_cast<const uint8_t*>(temp_buf));
    } catch (const std::exception& e) {
        std::cerr << "Failed to parse RDMA metadata: " << e.what() << std::endl;
        // 如果不是有效的RDMA元数据，可能是普通的UDP数据
        // 这里可以回退到普通UDP处理，但根据设计假设，UDP数据都是RDMA元数据
        return -1;
    }
    
    // 打印GID（十六进制格式）
    std::string gid_str;
    for (int i = 0; i < 16; i++) {
        char buf_hex[3];
        snprintf(buf_hex, sizeof(buf_hex), "%02x", metadata.gid[i]);
        gid_str += buf_hex;
        if (i % 2 == 1 && i != 15) gid_str += ":";
    }
    
    std::cout << "RDMA metadata details:" << std::endl;
    std::cout << "  QPN: " << metadata.qpn << std::endl;
    std::cout << "  GID: " << gid_str << std::endl;
    std::cout << "  QKey: " << metadata.qkey << std::endl;
    std::cout << "  Port: " << static_cast<int>(metadata.port_num) << std::endl;
    

    // 2. 检查是否已经保存过这个对端
    auto& peer_manager = GlobalPeerManager::instance();
    const PeerInfo* existing_peer = peer_manager.get_peer(from_ip, from_port);
    
    if (!existing_peer) {
        // 第一次收到这个对端的元数据
        std::cout << "New peer detected, adding to peer list..." << std::endl;
        
        // 获取PD（保护域），假设可以从rdma_manager获取
        ibv_pd* pd = m_rdma_manager->getPd();  // 需要UDRdmaManager添加这个方法
        
        // 保存对端信息
        peer_manager.add_peer(from_ip, from_port, metadata, pd);
        existing_peer = peer_manager.get_peer(from_ip, from_port);
        
        if (!existing_peer) {
            std::cerr << "Failed to add peer to list" << std::endl;
            return -1;
        }
        
        // 为对端创建AH（地址句柄）
        std::cout << "Creating AH for peer..." << std::endl;

        // 使用port_num而不是qkey
        ibv_ah* ah = peer_manager.get_or_create_ah(from_ip, from_port, pd, metadata.port_num);
        if (!ah) {
            std::cerr << "Failed to create AH for peer" << std::endl;
        } else {
            std::cout << "AH created successfully" << std::endl;
        }
        
        // 3. 发送本地的RDMA元数据作为回复
        std::cout << "Sending local RDMA metadata as reply..." << std::endl;
        
        RDMA_Metadata local_metadata;
        local_metadata.qpn = m_rdma_manager->getQpNum();
        local_metadata.port_num = m_rdma_manager->getPortNum();
        local_metadata.qkey = m_rdma_manager->getQkey();
        // 获取本地GID
        m_rdma_manager->getGid(local_metadata.gid);  // 需要UDRdmaManager添加这个方法
        
        // 序列化本地元数据
        char reply_buffer[RDMA_Metadata::serialized_size()];
        try {
            local_metadata.serialize(reinterpret_cast<uint8_t*>(reply_buffer));
        } catch (const std::exception& e) {
            std::cerr << "Failed to serialize local RDMA metadata: " << e.what() << std::endl;
            return -1;
        }
        
        // 通过UDP发送回复
        ssize_t send_result = orig_os_api.sendto(m_fd, reply_buffer, sizeof(reply_buffer), 0,
                                       (sockaddr*)&src_addr, src_len);
        
        if (send_result < 0) {
            std::cerr << "Failed to send RDMA metadata reply: " << strerror(errno) << std::endl;
            return -1;
        }
        
        // 打印GID（十六进制格式）
        std::string local_gid_str;
        for (int i = 0; i < 16; i++) {
            char buf_hex[3];
            snprintf(buf_hex, sizeof(buf_hex), "%02x", local_metadata.gid[i]);
            local_gid_str += buf_hex;
            if (i % 2 == 1 && i != 15) local_gid_str += ":";
        }
        
        std::cout << "Local RDMA metadata sent successfully" << std::endl;
        std::cout << "Local metadata details:" << std::endl;
        std::cout << "  QPN: " << local_metadata.qpn << std::endl;
        std::cout << "  GID: " << local_gid_str << std::endl;
        std::cout << "  QKey: " << local_metadata.qkey << std::endl;
        std::cout << "  Port: " << static_cast<int>(local_metadata.port_num) << std::endl;
        
        // 4. 发送回复后，立即轮询RDMA CQ等待数据
		// 这个地方改变，让轮询现成去负责，这边直接返回
        // 对端在收到我们的元数据回复后，很可能会立即通过RDMA发送实际数据
        std::cout << "Waiting for RDMA data from " << from_ip << ":" << from_port << " ..." << std::endl;
        
    } else {
        // 已经交换过元数据
        std::cout << "Already have RDMA metadata from " << from_ip << ":" << from_port << std::endl;
    }
    
    // 5. 返回0表示元数据交换完成（成功或重复）
    return 0;
}

ssize_t Socket_tb_udp::recvmsg(struct msghdr *msg, int flags) {
    // 1. 参数检查
    if (!msg || !msg->msg_iov || msg->msg_iovlen == 0) {
        errno = EINVAL;
        return -1;
    }
    
    // 假设只有一个接收缓冲区（根据要求）
    if (msg->msg_iovlen > 1) {
        std::cerr << "recvmsg: Multiple receive buffers not supported, using first buffer only" << std::endl;
        // 可以继续执行，但只使用第一个缓冲区
    }
    
    // 获取接收缓冲区信息
    void *buf = msg->msg_iov[0].iov_base;
    size_t nbytes = msg->msg_iov[0].iov_len;
    
    if (!buf || nbytes == 0) {
        errno = EINVAL;
        return -1;
    }
    
    std::cout << "recvmsg: Waiting for data, buffer size: " << nbytes << " bytes" << std::endl;
    
    // 2. 首先检查接收缓冲区是否有数据
    if (m_rdma_initialized) {
        if (m_rdma_manager->recv_buffer().has_data()) {
            std::cout << "recvmsg: Found data in RDMA receive buffer" << std::endl;
            
            // 准备源地址缓冲区
            sockaddr_in src_addr;
            socklen_t addrlen = sizeof(src_addr);
            
            // 直接从接收缓冲区读取数据
            ssize_t bytes_read = m_rdma_manager->recv_buffer().read_block(
                buf, nbytes, reinterpret_cast<sockaddr*>(&src_addr), &addrlen);
            
            if (bytes_read > 0) {
                std::cout << "recvmsg: Read " << bytes_read << " bytes from RDMA buffer" << std::endl;
                
                // 设置源地址到msg结构
                if (msg->msg_name && msg->msg_namelen >= sizeof(src_addr)) {
                    memcpy(msg->msg_name, &src_addr, std::min(static_cast<size_t>(msg->msg_namelen), 
                                                              sizeof(src_addr)));
                    msg->msg_namelen = sizeof(src_addr);
                    
                    // 打印源地址信息
                    char ip_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &src_addr.sin_addr, ip_str, sizeof(ip_str));
                    std::cout << "recvmsg: From " << ip_str << ":" << ntohs(src_addr.sin_port) << std::endl;
                }
                
                // 成功从缓冲区读取数据后，发布一个新的接收WR
                if (m_rdma_manager->recv_buffer().available_blocks() > 0) {
                    m_rdma_manager->post_recv(1);
                }
                
                return bytes_read;
            }
            
            // 如果bytes_read < 0，说明缓冲区太小，返回错误
            if (bytes_read < 0) {
                std::cerr << "recvmsg: Buffer too small for data" << std::endl;
                errno = EMSGSIZE;
                return -1;
            }
        }
    }
    
    // 3. 如果没有数据，根据flags决定行为
    if (flags & MSG_DONTWAIT) {
        // 非阻塞模式，立即返回
        std::cout << "recvmsg: Non-blocking mode, no data available" << std::endl;
        errno = EAGAIN;
        return -1;
    }
    
    // 4. 阻塞模式：设置socket为非阻塞，然后轮询等待
    // 保存原始socket标志
    int original_flags = orig_os_api.fcntl(m_fd, F_GETFL, 0);
    if (original_flags == -1) {
        perror("recvmsg: fcntl(F_GETFL) failed");
        errno = EBADF;
        return -1;
    }
    
    // 临时设置为非阻塞
    if (orig_os_api.fcntl(m_fd, F_SETFL, original_flags | O_NONBLOCK) == -1) {
        perror("recvmsg: fcntl(F_SETFL) failed");
        errno = EBADF;
        return -1;
    }
    
    // 5. 轮询等待：交替检查接收缓冲区和UDP socket
    const int MAX_WAIT_MS = 5000;  // 最大等待5秒
    const int CHECK_INTERVAL_US = 1000;  // 每次检查间隔1ms
    int total_wait_us = 0;
    
    std::cout << "recvmsg: Entering polling loop (max " << MAX_WAIT_MS << " ms)" << std::endl;
    
    while (total_wait_us < MAX_WAIT_MS * 1000) {
        // 首先检查RDMA接收缓冲区
        if (m_rdma_initialized) {
            if (m_rdma_manager->recv_buffer().has_data()) {
                std::cout << "recvmsg: Found data in RDMA buffer during polling" << std::endl;
                
                // 准备源地址缓冲区
                sockaddr_in src_addr;
                socklen_t addrlen = sizeof(src_addr);
                
                // 直接从接收缓冲区读取数据
                ssize_t bytes_read = m_rdma_manager->recv_buffer().read_block(
                    buf, nbytes, reinterpret_cast<sockaddr*>(&src_addr), &addrlen);
                
                if (bytes_read > 0) {
                    // 恢复socket标志
                    orig_os_api.fcntl(m_fd, F_SETFL, original_flags);
                    
                    // 设置源地址到msg结构
                    if (msg->msg_name && msg->msg_namelen >= sizeof(src_addr)) {
                        memcpy(msg->msg_name, &src_addr, std::min(static_cast<size_t>(msg->msg_namelen), 
                                                                  sizeof(src_addr)));
                        msg->msg_namelen = sizeof(src_addr);
                    }
                    
                    std::cout << "recvmsg: Read " << bytes_read << " bytes after polling" << std::endl;
                    
                    // 成功从缓冲区读取数据后，发布一个新的接收WR
                    if (m_rdma_manager->recv_buffer().available_blocks() > 0) {
                        m_rdma_manager->post_recv(1);
                    }
                    
                    return bytes_read;
                }
                
                // 如果bytes_read < 0，说明缓冲区太小
                if (bytes_read < 0) {
                    orig_os_api.fcntl(m_fd, F_SETFL, original_flags);
                    std::cerr << "recvmsg: Buffer too small for data" << std::endl;
                    errno = EMSGSIZE;
                    return -1;
                }
            }
        }
        
        // 然后检查UDP socket是否有数据（非阻塞）
        char temp_buf[65536];
        sockaddr_storage temp_addr;
        socklen_t temp_addrlen = sizeof(temp_addr);
        
        ssize_t recv_len = orig_os_api.recvfrom(m_fd, temp_buf, sizeof(temp_buf), 
                                     MSG_DONTWAIT, 
                                     reinterpret_cast<sockaddr*>(&temp_addr), &temp_addrlen);
        
        if (recv_len > 0) {
            std::cout << "recvmsg: Received " << recv_len << " bytes via UDP" << std::endl;
            
            // 处理UDP元数据
            sockaddr_in& src_addr_in = *reinterpret_cast<sockaddr_in*>(&temp_addr);
            ssize_t handled_len = handle_udp_metadata(temp_buf, recv_len, src_addr_in, temp_addrlen);
            
            if (handled_len > 0) {
                std::cout << "recvmsg: Handled UDP metadata, continue polling" << std::endl;
            }
            
            // 注意：我们不在这里恢复socket标志，因为要继续轮询
            continue;
        } else if (recv_len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            // 真正的错误
            std::cerr << "recvmsg: UDP recvfrom error: " << strerror(errno) << std::endl;
            orig_os_api.fcntl(m_fd, F_SETFL, original_flags);
            return -1;
        }
        
        // 等待一小段时间再检查
        usleep(CHECK_INTERVAL_US);
        total_wait_us += CHECK_INTERVAL_US;
        
        // 定期打印等待状态
        if (total_wait_us % 1000000 == 0) {  // 每1秒打印一次
            std::cout << "recvmsg: Waiting for " << (total_wait_us / 1000000) << " seconds..." << std::endl;
        }
    }
    
    // 恢复socket标志
    orig_os_api.fcntl(m_fd, F_SETFL, original_flags);
    
    // 超时
    std::cout << "recvmsg: Timeout after " << MAX_WAIT_MS << " ms" << std::endl;
    errno = EAGAIN;
    return -1;
}


ssize_t Socket_tb_udp::sendmsg(const struct msghdr *msg, int flags) {
    // 1. 参数检查
    if (!msg || !msg->msg_iov || msg->msg_iovlen == 0) {
        errno = EINVAL;
        return -1;
    }
    
    // 2. 提取目标地址
    const struct sockaddr_in* to_addr = nullptr;
    std::string target_ip;
    int target_port = 0;
    
    if (msg->msg_name && msg->msg_namelen >= sizeof(struct sockaddr_in)) {
        to_addr = reinterpret_cast<const struct sockaddr_in*>(msg->msg_name);
        if (to_addr->sin_family != AF_INET) {
            errno = EAFNOSUPPORT;
            return -1;
        }
        
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &to_addr->sin_addr, ip_str, sizeof(ip_str));
        target_ip = ip_str;
        target_port = ntohs(to_addr->sin_port);
        
        std::cout << "sendmsg: Sending to " << target_ip << ":" << target_port 
                  << ", buffer segments: " << msg->msg_iovlen << std::endl;
    } else if (m_isConnected) {
        // 使用已连接的目标地址
        to_addr = &m_defaultDestAddr;
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &to_addr->sin_addr, ip_str, sizeof(ip_str));
        target_ip = ip_str;
        target_port = ntohs(to_addr->sin_port);
        
        std::cout << "sendmsg: Sending to connected address " << target_ip << ":" << target_port 
                  << ", buffer segments: " << msg->msg_iovlen << std::endl;
    } else {
        // 没有目标地址
        std::cerr << "sendmsg: No destination address provided and socket is not connected" << std::endl;
        errno = EDESTADDRREQ;
        return -1;
    }
    
    // 3. 计算总数据长度
    size_t total_len = 0;
    for (size_t i = 0; i < msg->msg_iovlen; i++) {
        total_len += msg->msg_iov[i].iov_len;
    }
    
    if (total_len == 0) {
        std::cout << "sendmsg: Zero-length message, skipping" << std::endl;
        return 0;
    }
    
    std::cout << "sendmsg: Total data size: " << total_len << " bytes" << std::endl;
    
    // 4. 检查是否已经和对端交换过元数据
    auto& peer_manager = GlobalPeerManager::instance();
    const PeerInfo* peer = peer_manager.get_peer(target_ip, target_port);
    
    if (!peer) {
        // 第一次通信，需要交换RDMA元数据
        std::cout << "First communication with " << target_ip << ":" << target_port 
                  << " via sendmsg, exchanging RDMA metadata..." << std::endl;
        
        // 在这里调用 establish_rdma_connection
        if (!establish_rdma_connection(target_ip.c_str(), target_port, 
                                      reinterpret_cast<const struct sockaddr*>(to_addr))) {
            std::cerr << "Failed to establish RDMA connection via sendmsg, falling back to normal UDP" << std::endl;
            
            // 如果建立RDMA连接失败，回退到普通UDP发送
            // 注意：需要将所有缓冲区数据合并发送
            return fallback_to_normal_sendmsg(msg, flags, to_addr);
        }
         
        std::cout << "RDMA metadata exchange completed with " << target_ip << ":" << target_port << std::endl;
        
        // 重新获取peer信息
        peer = peer_manager.get_peer(target_ip, target_port);
    }
    
    // 5. 确保RDMA管理器已初始化
    if (!m_rdma_manager || !m_rdma_initialized) {
        // 如果没有初始化，尝试使用本地地址初始化
        // 注意：这里可能需要绑定一个随机端口
        std::cout << "RDMA manager not initialized, attempting to bind..." << std::endl;
        
        // 尝试绑定到任意可用端口
        struct sockaddr_in local_addr;
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        local_addr.sin_port = 0;  // 让系统分配端口
        
        socklen_t addrlen = sizeof(local_addr);
        // 使用 orig_os_api.bind 替代 ::bind
        if (orig_os_api.bind(m_fd, (struct sockaddr*)&local_addr, addrlen) < 0) {
            std::cerr << "Failed to bind socket for RDMA initialization: " << strerror(errno) << std::endl;
            return fallback_to_normal_sendmsg(msg, flags, to_addr);
        }
        
        // 获取绑定的端口
        // 使用 orig_os_api.getsockname 替代 ::getsockname
        if (orig_os_api.getsockname(m_fd, (struct sockaddr*)&local_addr, &addrlen) < 0) {
            std::cerr << "Failed to get socket name after bind: " << strerror(errno) << std::endl;
        }
        
        // 初始化RDMA管理器
        if (!initRdmaManager(ntohl(local_addr.sin_addr.s_addr), ntohs(local_addr.sin_port), m_fd)) {
            std::cerr << "Failed to initialize RDMA manager, falling back to normal UDP" << std::endl;
            return fallback_to_normal_sendmsg(msg, flags, to_addr);
        }
    }
    
    // 6. 检查数据大小是否超过块的数据容量
    if (total_len > m_rdma_manager->send_buffer().data_capacity()) {
        std::cerr << "Data size " << total_len << " exceeds block capacity " 
                  << m_rdma_manager->send_buffer().data_capacity() 
                  << ", falling back to normal UDP" << std::endl;
        return fallback_to_normal_sendmsg(msg, flags, to_addr);
    }
    
    // 7. 将多个缓冲区的数据合并写入发送缓冲区
    // 分配临时缓冲区存放合并后的数据
    std::unique_ptr<char[]> temp_buffer(new char[total_len]);
    char* dest_ptr = temp_buffer.get();
    
    for (size_t i = 0; i < msg->msg_iovlen; i++) {
        memcpy(dest_ptr, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len);
        dest_ptr += msg->msg_iov[i].iov_len;
    }
    
    // 8. 将合并后的数据写入RDMA发送缓冲区
    if (!m_rdma_manager->send_buffer().write_block(temp_buffer.get(), total_len)) {
        std::cerr << "Failed to write data to RDMA send buffer, buffer may be full" << std::endl;
        errno = EAGAIN;
        return -1;
    }
    
    // 9. 使用RDMA UD发送数据
    bool rdma_success = post_send_to_peer(peer, total_len);
    
    if (!rdma_success) {
        std::cerr << "RDMA send failed via sendmsg, falling back to normal UDP" << std::endl;
        return fallback_to_normal_sendmsg(msg, flags, to_addr);
    }
    
    std::cout << "sendmsg: Successfully sent " << total_len 
              << " bytes via RDMA to " << target_ip << ":" << target_port << std::endl;
    
    return total_len;
}

// 回退到普通UDP sendmsg的辅助函数
ssize_t Socket_tb_udp::fallback_to_normal_sendmsg(const struct msghdr *msg, int flags, 
                                                  const struct sockaddr_in* to_addr) {
    // 准备发送消息的副本
    struct msghdr send_msg;
    memcpy(&send_msg, msg, sizeof(struct msghdr));
    
    // 如果需要目标地址，设置目标地址
    if (to_addr && !msg->msg_name) {
        send_msg.msg_name = const_cast<struct sockaddr_in*>(to_addr);
        send_msg.msg_namelen = sizeof(struct sockaddr_in);
    }
    
    // 使用 orig_os_api.sendmsg 替代 ::sendmsg
    ssize_t result = orig_os_api.sendmsg(m_fd, &send_msg, flags);
    
    if (result < 0) {
        std::cerr << "Normal UDP sendmsg failed: " << strerror(errno) << std::endl;
    } else {
        std::cout << "Normal UDP sendmsg succeeded, sent " << result << " bytes" << std::endl;
    }
    
    return result;
}


int Socket_tb_udp::ioctl(unsigned long int __request, unsigned long int __arg){
	return orig_os_api.ioctl(m_fd, __request, __arg);
}

int Socket_tb_udp::setsockopt(int __level, int __optname,  __const void *__optval, socklen_t __optlen){
	return orig_os_api.setsockopt(m_fd, __level, __optname, __optval, __optlen);
}

int Socket_tb_udp::getsockopt(int __level, int __optname, void *__optval, socklen_t *__optlen) {
    if (!__optval || !__optlen) {
        errno = EFAULT;
        return -1;
    }
    
    // 如果底层有真实的socket文件描述符，对未处理的level使用原始系统调用
    if (m_fd >= 0) {
        // 处理三个特定的level，其他都直接调用系统调用
        switch (__level) {
            case SOL_SOCKET: {  // 1
                return handle_socket_options(__optname, __optval, __optlen);
            }
            
            case IPPROTO_IP: {  // 0
                return handle_ip_options(__optname, __optval, __optlen);
            }
            
            case 270: {  // 不知道dds设置的什么级别，不管
                return orig_os_api.getsockopt(m_fd, __level, __optname, __optval, __optlen);
            }
            
            default: {
                // 其他情况使用原始系统调用
                return orig_os_api.getsockopt(m_fd, __level, __optname, __optval, __optlen);
            }
        }
    } else {
        std::cout << "something wrong happen" << std::endl;
    }
}

// 处理SOL_SOCKET级别选项
int Socket_tb_udp::handle_socket_options(int optname, void *optval, socklen_t *optlen) {
    int m_fd = get_fd();
    
    // 检查缓冲区大小
    if (!optval || !optlen) {
        errno = EFAULT;
        return -1;
    }
    
    // 根据选项名称处理
    switch (optname) {
        case SO_TYPE: {
            // 返回socket类型 - SOCK_DGRAM
            if (*optlen < sizeof(int)) {
                errno = EINVAL;
                return -1;
            }
            int type = SOCK_DGRAM;
            memcpy(optval, &type, sizeof(type));
            *optlen = sizeof(type);
            return 0;
        }
        
        case SO_ERROR: {
            // 返回socket错误状态
            if (*optlen < sizeof(int)) {
                errno = EINVAL;
                return -1;
            }
            int error = 0; // 假设没有错误
            memcpy(optval, &error, sizeof(error));
            *optlen = sizeof(error);
            return 0;
        }
        
        case SO_BROADCAST: {
            // 返回广播设置
            if (*optlen < sizeof(int)) {
                errno = EINVAL;
                return -1;
            }
            int broadcast = m_broadcastEnabled ? 1 : 0;
            memcpy(optval, &broadcast, sizeof(broadcast));
            *optlen = sizeof(broadcast);
            return 0;
        }
        
        case SO_REUSEADDR: {
            // 如果有底层socket，从系统获取
            if (m_fd >= 0) {
                return orig_os_api.getsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, optval, optlen);
            }
            // 否则返回默认值
            if (*optlen < sizeof(int)) {
                errno = EINVAL;
                return -1;
            }
            int reuse = 1; // 默认启用地址重用
            memcpy(optval, &reuse, sizeof(reuse));
            *optlen = sizeof(reuse);
            return 0;
        }
        
        case SO_REUSEPORT: {
            // 如果有底层socket，从系统获取
            if (m_fd >= 0) {
                return orig_os_api.getsockopt(m_fd, SOL_SOCKET, SO_REUSEPORT, optval, optlen);
            }
            // 否则返回默认值
            if (*optlen < sizeof(int)) {
                errno = EINVAL;
                return -1;
            }
            int reuse = 0; // 默认不启用端口重用
            memcpy(optval, &reuse, sizeof(reuse));
            *optlen = sizeof(reuse);
            return 0;
        }
        
        case SO_RCVBUF: {
            // 如果有RDMA管理器，返回RDMA接收缓冲区大小
            if (m_rdma_manager && m_rdma_manager->isInitialized()) {
                if (*optlen < sizeof(int)) {
                    errno = EINVAL;
                    return -1;
                }
                int rcvbuf = m_rdma_manager->recv_buffer().get_buffer_size();
                memcpy(optval, &rcvbuf, sizeof(rcvbuf));
                *optlen = sizeof(rcvbuf);
                return 0;
            } else if (m_fd >= 0) {
                // 否则从底层socket获取
                return orig_os_api.getsockopt(m_fd, SOL_SOCKET, SO_RCVBUF, optval, optlen);
            } else {
                // 默认值
                if (*optlen < sizeof(int)) {
                    errno = EINVAL;
                    return -1;
                }
                int rcvbuf = 131072; // 128KB
                memcpy(optval, &rcvbuf, sizeof(rcvbuf));
                *optlen = sizeof(rcvbuf);
                return 0;
            }
        }
        
        case SO_SNDBUF: {
            // 如果有RDMA管理器，返回RDMA发送缓冲区大小
            if (m_rdma_manager && m_rdma_manager->isInitialized()) {
                if (*optlen < sizeof(int)) {
                    errno = EINVAL;
                    return -1;
                }
                int sndbuf = m_rdma_manager->send_buffer().get_buffer_size();
                memcpy(optval, &sndbuf, sizeof(sndbuf));
                *optlen = sizeof(sndbuf);
                return 0;
            } else if (m_fd >= 0) {
                // 否则从底层socket获取
                return orig_os_api.getsockopt(m_fd, SOL_SOCKET, SO_SNDBUF, optval, optlen);
            } else {
                // 默认值
                if (*optlen < sizeof(int)) {
                    errno = EINVAL;
                    return -1;
                }
                int sndbuf = 131072; // 128KB
                memcpy(optval, &sndbuf, sizeof(sndbuf));
                *optlen = sizeof(sndbuf);
                return 0;
            }
        }
        
        case SO_RCVTIMEO: {
            // 如果有底层socket，从系统获取
            if (m_fd >= 0) {
                return orig_os_api.getsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, optval, optlen);
            }
            // 如果没有底层socket，返回0表示没有超时
            if (*optlen >= sizeof(struct timeval)) {
                struct timeval tv = {0, 0};
                memcpy(optval, &tv, sizeof(tv));
                *optlen = sizeof(tv);
                return 0;
            } else if (*optlen >= sizeof(struct timespec)) {
                struct timespec ts = {0, 0};
                memcpy(optval, &ts, sizeof(ts));
                *optlen = sizeof(ts);
                return 0;
            } else {
                errno = EINVAL;
                return -1;
            }
        }
        
        case SO_SNDTIMEO: {
            // 如果有底层socket，从系统获取
            if (m_fd >= 0) {
                return orig_os_api.getsockopt(m_fd, SOL_SOCKET, SO_SNDTIMEO, optval, optlen);
            }
            // 如果没有底层socket，返回0表示没有超时
            if (*optlen >= sizeof(struct timeval)) {
                struct timeval tv = {0, 0};
                memcpy(optval, &tv, sizeof(tv));
                *optlen = sizeof(tv);
                return 0;
            } else if (*optlen >= sizeof(struct timespec)) {
                struct timespec ts = {0, 0};
                memcpy(optval, &ts, sizeof(ts));
                *optlen = sizeof(ts);
                return 0;
            } else {
                errno = EINVAL;
                return -1;
            }
        }
        
        // 处理其他常见的socket选项
        case SO_KEEPALIVE:
        case SO_DONTROUTE:
        case SO_LINGER:
        case SO_OOBINLINE:
        case SO_ACCEPTCONN:
        case SO_DEBUG: {
            if (m_fd >= 0) {
                return orig_os_api.getsockopt(m_fd, SOL_SOCKET, optname, optval, optlen);
            }
            // 对于UDP socket，这些选项通常不相关，返回0或适当默认值
            if (*optlen >= sizeof(int)) {
                int value = 0;
                memcpy(optval, &value, sizeof(value));
                *optlen = sizeof(value);
                return 0;
            }
            errno = EINVAL;
            return -1;
        }
        
        case SO_DOMAIN: {
            // 返回地址族
            if (*optlen < sizeof(int)) {
                errno = EINVAL;
                return -1;
            }
            int domain = AF_INET; // 假设IPv4
            memcpy(optval, &domain, sizeof(domain));
            *optlen = sizeof(domain);
            return 0;
        }
        
        case SO_PROTOCOL: {
            // 返回协议
            if (*optlen < sizeof(int)) {
                errno = EINVAL;
                return -1;
            }
            int protocol = IPPROTO_UDP;
            memcpy(optval, &protocol, sizeof(protocol));
            *optlen = sizeof(protocol);
            return 0;
        }
        
        // UDP特定选项，虽然不在SOL_SOCKET级别，但有些应用可能错误地查询
        case UDP_CORK:
        case UDP_SEGMENT:
        case UDP_GRO: {
            // 这些应该是IPPROTO_UDP级别的选项
            // 返回未实现的协议选项错误
            errno = ENOPROTOOPT;
            return -1;
        }
        
        default: {
            // 未知的socket选项，如果有底层socket，尝试系统调用
            if (m_fd >= 0) {
                return orig_os_api.getsockopt(m_fd, SOL_SOCKET, optname, optval, optlen);
            }
            // 没有底层socket，返回未实现
            errno = ENOPROTOOPT;
            return -1;
        }
    }
}

// 处理IPPROTO_IP级别选项
int Socket_tb_udp::handle_ip_options(int optname, void *optval, socklen_t *optlen) {
    int m_fd = get_fd();
    
    // 检查缓冲区大小
    if (!optval || !optlen) {
        errno = EFAULT;
        return -1;
    }
    
    switch (optname) {
        case IP_TTL: {
            // 返回TTL值
            if (*optlen < sizeof(int)) {
                errno = EINVAL;
                return -1;
            }
            if (m_fd >= 0) {
                return orig_os_api.getsockopt(m_fd, IPPROTO_IP, IP_TTL, optval, optlen);
            }
            int ttl = 64; // 默认TTL
            memcpy(optval, &ttl, sizeof(ttl));
            *optlen = sizeof(ttl);
            return 0;
        }
        
        case IP_TOS: {
            // 返回服务类型
            if (*optlen < sizeof(int)) {
                errno = EINVAL;
                return -1;
            }
            if (m_fd >= 0) {
                return orig_os_api.getsockopt(m_fd, IPPROTO_IP, IP_TOS, optval, optlen);
            }
            int tos = 0; // 默认TOS
            memcpy(optval, &tos, sizeof(tos));
            *optlen = sizeof(tos);
            return 0;
        }
        
        case IP_MULTICAST_TTL: {
            // 返回多播TTL
            if (*optlen < sizeof(int)) {
                errno = EINVAL;
                return -1;
            }
            if (m_fd >= 0) {
                return orig_os_api.getsockopt(m_fd, IPPROTO_IP, IP_MULTICAST_TTL, optval, optlen);
            }
            int multicast_ttl = 1; // 默认多播TTL
            memcpy(optval, &multicast_ttl, sizeof(multicast_ttl));
            *optlen = sizeof(multicast_ttl);
            return 0;
        }
        
        case IP_MULTICAST_LOOP: {
            // 返回多播回环设置
            if (*optlen < sizeof(int)) {
                errno = EINVAL;
                return -1;
            }
            if (m_fd >= 0) {
                return orig_os_api.getsockopt(m_fd, IPPROTO_IP, IP_MULTICAST_LOOP, optval, optlen);
            }
            int loop = 1; // 默认启用多播回环
            memcpy(optval, &loop, sizeof(loop));
            *optlen = sizeof(loop);
            return 0;
        }
        
        case IP_MULTICAST_IF: {
            // 返回多播接口
            if (m_fd >= 0) {
                return orig_os_api.getsockopt(m_fd, IPPROTO_IP, IP_MULTICAST_IF, optval, optlen);
            }
            // 如果没有设置，返回默认值
            struct in_addr addr;
            addr.s_addr = INADDR_ANY;
            if (*optlen >= sizeof(struct in_addr)) {
                memcpy(optval, &addr, sizeof(addr));
                *optlen = sizeof(addr);
                return 0;
            }
            errno = EINVAL;
            return -1;
        }
        
        case IP_PKTINFO: {
            // 返回包信息设置
            if (*optlen < sizeof(int)) {
                errno = EINVAL;
                return -1;
            }
            if (m_fd >= 0) {
                return orig_os_api.getsockopt(m_fd, IPPROTO_IP, IP_PKTINFO, optval, optlen);
            }
            int pktinfo = 0; // 默认不启用
            memcpy(optval, &pktinfo, sizeof(pktinfo));
            *optlen = sizeof(pktinfo);
            return 0;
        }
        
        case IP_OPTIONS: {
            // IP选项
            if (m_fd >= 0) {
                return orig_os_api.getsockopt(m_fd, IPPROTO_IP, IP_OPTIONS, optval, optlen);
            }
            // 对于纯RDMA实现，通常没有IP选项
            if (*optlen >= 1) {
                // 返回空选项
                memset(optval, 0, 1);
                *optlen = 0;
                return 0;
            }
            errno = EINVAL;
            return -1;
        }
        
        default: {
            // 其他IP选项，如果有底层socket，尝试系统调用
            if (m_fd >= 0) {
                return orig_os_api.getsockopt(m_fd, IPPROTO_IP, optname, optval, optlen);
            }
            // 没有底层socket，返回未实现
            errno = ENOPROTOOPT;
            return -1;
        }
    }
}


// 单例实例
GlobalPeerManager& GlobalPeerManager::instance() {
    static GlobalPeerManager manager;
    return manager;
}

// 私有构造函数
GlobalPeerManager::GlobalPeerManager() {
    std::cout << "[PeerManager] Global peer manager initialized" << std::endl;
}

// 析构函数
GlobalPeerManager::~GlobalPeerManager() {
    cleanup();
    std::cout << "[PeerManager] Global peer manager destroyed" << std::endl;
}

// 添加对端信息
bool GlobalPeerManager::add_peer(const std::string& ip, uint16_t port, 
                                 const RDMA_Metadata& metadata, ibv_pd* pd) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string key = make_peer_key(ip, port);
    
    // 检查是否已存在
    auto it = peer_map_.find(key);
    if (it != peer_map_.end()) {
        // 已存在，更新元数据
        std::cout << "[PeerManager] Peer already exists, updating: " << key << std::endl;
        it->second.metadata = metadata;
        if (pd != nullptr) {
            it->second.pd = pd;
        }
        return true;
    }
    
    // 创建新的对端信息
    PeerInfo peer;
    peer.ip = ip;
    peer.port = port;
    peer.metadata = metadata;
    peer.pd = pd;
    peer.ah = nullptr;  // 延迟创建AH
    
    // 添加到映射表
    peer_map_[key] = peer;
    
    // 打印GID（十六进制格式）
    std::string gid_str;
    for (int i = 0; i < 16; i++) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", metadata.gid[i]);
        gid_str += buf;
        if (i % 2 == 1 && i != 15) gid_str += ":";
    }
    
    std::cout << "[PeerManager] Added peer: " << key 
              << " (QPN: " << metadata.qpn 
              << ", GID: " << gid_str
              << ", Port: " << static_cast<int>(metadata.port_num) 
              << ")" << std::endl;
    
    return true;
}

// 更新对端信息
bool GlobalPeerManager::update_peer(const std::string& ip, uint16_t port, 
                                    const RDMA_Metadata& metadata) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string key = make_peer_key(ip, port);
    auto it = peer_map_.find(key);
    
    if (it == peer_map_.end()) {
        std::cerr << "[PeerManager] Peer not found: " << key << std::endl;
        return false;
    }
    
    // 如果AH已经存在，并且元数据有变化，需要重新创建AH
    bool need_recreate_ah = false;
    if (it->second.ah != nullptr) {
        // 比较GID是否变化
        bool gid_changed = false;
        for (int i = 0; i < 16; i++) {
            if (it->second.metadata.gid[i] != metadata.gid[i]) {
                gid_changed = true;
                break;
            }
        }
        
        if (gid_changed ||
            it->second.metadata.port_num != metadata.port_num ||
            it->second.metadata.qkey != metadata.qkey) {
            need_recreate_ah = true;
        }
    }
    
    // 更新元数据
    it->second.metadata = metadata;
    
    // 如果需要，重新创建AH
    if (need_recreate_ah && it->second.pd != nullptr) {
        // 销毁旧的AH
        destroy_ah(it->second.ah);
        
        // 创建新的AH
        it->second.ah = create_ah(it->second, it->second.pd, metadata.port_num);
        
        std::cout << "[PeerManager] Recreated AH for peer: " << key << std::endl;
    }
    
    std::cout << "[PeerManager] Updated peer: " << key 
              << " (QPN: " << metadata.qpn << ")" << std::endl;
    
    return true;
}

// 移除对端
bool GlobalPeerManager::remove_peer(const std::string& ip, uint16_t port) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string key = make_peer_key(ip, port);
    auto it = peer_map_.find(key);
    
    if (it == peer_map_.end()) {
        return false;
    }
    
    // 销毁地址句柄
    if (it->second.ah != nullptr) {
        destroy_ah(it->second.ah);
    }
    
    // 从映射表中移除
    peer_map_.erase(it);
    
    std::cout << "[PeerManager] Removed peer: " << key << std::endl;
    
    return true;
}

// 获取对端信息
const PeerInfo* GlobalPeerManager::get_peer(const std::string& ip, uint16_t port) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string key = make_peer_key(ip, port);
    auto it = peer_map_.find(key);
    
    return (it != peer_map_.end()) ? &(it->second) : nullptr;
}

// 获取对端的地址句柄（如果不存在则创建）
ibv_ah* GlobalPeerManager::get_or_create_ah(const std::string& ip, uint16_t port, 
                                            ibv_pd* pd, uint8_t port_num) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string key = make_peer_key(ip, port);
    auto it = peer_map_.find(key);
    
    if (it == peer_map_.end()) {
        std::cerr << "[PeerManager] Peer not found: " << key << std::endl;
        return nullptr;
    }
    
    PeerInfo& peer = it->second;
    
    // 如果AH已经存在，直接返回
    if (peer.ah != nullptr) {
        return peer.ah;
    }
    
    // 如果没有PD信息，使用传入的PD
    if (peer.pd == nullptr && pd != nullptr) {
        peer.pd = pd;
    }
    
    // 使用指定的端口号（如果提供了的话）
    if (port_num != 1 && peer.metadata.port_num == 1) {
        peer.metadata.port_num = port_num;
    }
    
    // 检查PD是否有效
    if (peer.pd == nullptr) {
        std::cerr << "[PeerManager] Cannot create AH: PD is null for peer " 
                  << key << std::endl;
        return nullptr;
    }
    
    // 创建AH
    peer.ah = create_ah(peer, peer.pd, peer.metadata.port_num);
    
    if (peer.ah != nullptr) {
        std::cout << "[PeerManager] Created AH for peer: " << key 
                  << " using PD: " << peer.pd << std::endl;
    }
    
    return peer.ah;
}

// 检查对端是否存在
bool GlobalPeerManager::has_peer(const std::string& ip, uint16_t port) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string key = make_peer_key(ip, port);
    return peer_map_.find(key) != peer_map_.end();
}

// 获取对端数量
size_t GlobalPeerManager::get_peer_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return peer_map_.size();
}

// 打印所有对端信息
void GlobalPeerManager::print_all_peers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::cout << "\n[PeerManager] All Peers (" << peer_map_.size() << "):" << std::endl;
    std::cout << "=========================================" << std::endl;
    
    for (const auto& pair : peer_map_) {
        const PeerInfo& peer = pair.second;
        const RDMA_Metadata& metadata = peer.metadata;
        
        std::cout << "  " << peer.ip << ":" << peer.port << std::endl;
        std::cout << "    QPN: " << metadata.qpn << std::endl;
        
        // 打印GID（十六进制格式）
        std::cout << "    GID: ";
        for (int i = 0; i < 16; i++) {
            std::cout << std::hex << std::setw(2) << std::setfill('0') 
                     << static_cast<int>(metadata.gid[i]);
            if (i % 2 == 1 && i != 15) std::cout << ":";
        }
        std::cout << std::dec << std::endl;
        
        std::cout << "    Port: " << static_cast<int>(metadata.port_num) << std::endl;
        std::cout << "    QKey: 0x" << std::hex << metadata.qkey << std::dec << std::endl;
        std::cout << "    AH: " << peer.ah << std::endl;
        std::cout << "    PD: " << peer.pd << std::endl;
        std::cout << "-----------------------------------------" << std::endl;
    }
}

// 清理指定PD创建的所有AH
void GlobalPeerManager::cleanup_pd_ahs(ibv_pd* pd) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (pd == nullptr) {
        return;
    }
    
    size_t destroyed_count = 0;
    
    for (auto& pair : peer_map_) {
        PeerInfo& peer = pair.second;
        
        // 如果这个对端的AH是由指定的PD创建的，销毁它
        if (peer.ah != nullptr && peer.pd == pd) {
            destroy_ah(peer.ah);
            peer.ah = nullptr;
            destroyed_count++;
        }
    }
    
    std::cout << "[PeerManager] Cleaned up " << destroyed_count 
              << " AHs for PD: " << pd << std::endl;
}

// 清理所有资源
void GlobalPeerManager::cleanup() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    size_t destroyed_count = 0;
    
    // 销毁所有地址句柄
    for (auto& pair : peer_map_) {
        PeerInfo& peer = pair.second;
        
        if (peer.ah != nullptr) {
            destroy_ah(peer.ah);
            peer.ah = nullptr;
            destroyed_count++;
        }
    }
    
    // 清空映射表
    peer_map_.clear();
    
    std::cout << "[PeerManager] Cleaned up " << destroyed_count 
              << " AHs and removed all peers" << std::endl;
}

// 生成对端键
std::string GlobalPeerManager::make_peer_key(const std::string& ip, uint16_t port) {
    return ip + ":" + std::to_string(port);
}

// 创建地址句柄（基于RoCE GID）
ibv_ah* GlobalPeerManager::create_ah(const PeerInfo& peer, ibv_pd* pd, uint8_t port_num) {
    if (pd == nullptr) {
        std::cerr << "[PeerManager] Cannot create AH: PD is null for peer " 
                  << peer.ip << ":" << peer.port << std::endl;
        return nullptr;
    }
    
    // 检查GID是否全为0
    bool gid_all_zero = true;
    for (int i = 0; i < 16; i++) {
        if (peer.metadata.gid[i] != 0) {
            gid_all_zero = false;
            break;
        }
    }
    
    if (gid_all_zero) {
        std::cerr << "[PeerManager] Cannot create AH: GID is all zeros for peer " 
                  << peer.ip << ":" << peer.port << std::endl;
        return nullptr;
    }
    
    // 准备AH属性
    ibv_ah_attr ah_attr;
    memset(&ah_attr, 0, sizeof(ah_attr));
    
    // 对于RoCE网络，需要设置全局路由
    ah_attr.is_global = 1;  // 启用全局路由
    ah_attr.grh.hop_limit = 1;  // TTL值
    ah_attr.grh.sgid_index = 4;  // 源GID索引
    
    // 设置目标GID
    memcpy(ah_attr.grh.dgid.raw, peer.metadata.gid, 16);
    
    // 设置其他属性
    ah_attr.dlid = 0;  // 对于RoCE v2，DLID为0
    ah_attr.sl = 0;    // 服务级别，默认为0
    ah_attr.src_path_bits = 0;
    ah_attr.port_num = port_num;
    ah_attr.grh.flow_label = 0;  // 流标签
    
    // 创建AH
    ibv_ah* ah = ibv_create_ah(pd, &ah_attr);
    
    if (ah == nullptr) {
        std::cerr << "[PeerManager] Failed to create AH for peer: " 
                  << peer.ip << ":" << peer.port 
                  << " (Port: " << static_cast<int>(port_num) 
                  << ")" << std::endl;
        // 打印GID以便调试
        std::cerr << "  GID: ";
        for (int i = 0; i < 16; i++) {
            std::cerr << std::hex << std::setw(2) << std::setfill('0') 
                     << static_cast<int>(peer.metadata.gid[i]);
            if (i % 2 == 1 && i != 15) std::cerr << ":";
        }
        std::cerr << std::dec << std::endl;
    } else {
        std::cout << "[PeerManager] Created AH for peer: " 
                  << peer.ip << ":" << peer.port << std::endl;
    }
    
    return ah;
}

// 销毁地址句柄
void GlobalPeerManager::destroy_ah(ibv_ah* ah) {
    if (ah != nullptr) {
        ibv_destroy_ah(ah);
    }
}

// zc add
extern My_fd_collection* my_g_p_fd_collection;
extern fd_collection* g_p_fd_collection;
extern SoRconn_collection* g_p_conn_collection;




