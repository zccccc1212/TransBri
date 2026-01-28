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


#ifndef SOCK_REDIRECT_H
#define SOCK_REDIRECT_H


//if you need select with more than 1024 sockets - enable this
#ifndef SELECT_BIG_SETSIZE
#define SELECT_BIG_SETSIZE 0
#endif

#if SELECT_BIG_SETSIZE
#include <features.h>
#if  (__GLIBC__ > 2) || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 2)
#include <bits/types.h>
#undef __FD_SETSIZE
#define __FD_SETSIZE 32768
#endif
#endif //SELECT_BIG_SETSIZE

#include <stdint.h>
#include <fcntl.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <features.h>
#include <signal.h>
#include <dlfcn.h>
#include <netinet/in.h>
#include <linux/net.h>
#include <linux/socket.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <time.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <resolv.h>
#include <mutex>
#include <unordered_map>

#include <iomanip>
#include <iostream>
#include <string>


#include <vma/util/vtypes.h>
#include <vma/util/vma_stats.h>
#include <vma/util/sys_vars.h>
#include <vma/util/utils.h>


#include <vlogger/vlogger.h>

// RDMA元数据结构（使用gid不需要lid）
struct RDMA_Metadata {
    uint32_t qpn;                   // QP号
    uint8_t gid[16];                // GID（RoCE网络使用）
    uint8_t port_num;               // 端口号
    uint32_t qkey;                  // QKEY
    
    RDMA_Metadata() 
        : qpn(0), port_num(1), qkey(0x111111) {
        memset(gid, 0, sizeof(gid));  // 初始化GID为0
    }
    
    // 序列化（用于网络传输）
    void serialize(uint8_t* buffer) const {
        // 序列化qpn
        *reinterpret_cast<uint32_t*>(buffer) = qpn;
        
        // 序列化gid（16字节）
        memcpy(buffer + 4, gid, 16);
        
        // 序列化port_num
        *(buffer + 20) = port_num;
        
        // 序列化qkey
        *reinterpret_cast<uint32_t*>(buffer + 21) = qkey;
    }
    
    // 反序列化（用于从网络接收）
    void deserialize(const uint8_t* buffer) {
        // 反序列化qpn
        qpn = *reinterpret_cast<const uint32_t*>(buffer);
        
        // 反序列化gid
        memcpy(gid, buffer + 4, 16);
        
        // 反序列化port_num
        port_num = *(buffer + 20);
        
        // 反序列化qkey
        qkey = *reinterpret_cast<const uint32_t*>(buffer + 21);
    }
    
    // 获取序列化后的字节数
    static constexpr size_t serialized_size() {
        return sizeof(uint32_t) +  // qpn
               16 +                // gid (16 bytes)
               1 +                 // port_num (uint8_t)
               sizeof(uint32_t);   // qkey
    }
    
    // 打印元数据信息
    void print() const {
        std::cout << "RDMA Metadata (RoCE):" << std::endl;
        std::cout << "  QPN: " << qpn << std::endl;
        
        // 打印GID（十六进制格式）
        std::cout << "  GID: ";
        for (int i = 0; i < 16; i++) {
            std::cout << std::hex << std::setw(2) << std::setfill('0') 
                     << static_cast<int>(gid[i]);
            if (i % 2 == 1 && i != 15) std::cout << ":";
        }
        std::cout << std::dec << std::endl;
        
        std::cout << "  Port: " << static_cast<int>(port_num) << std::endl;
        std::cout << "  QKey: 0x" << std::hex << qkey << std::dec << std::endl;
    }
};

struct ibv_pd;
struct ibv_ah;
class UDRdmaManager;


struct PeerInfo {
    std::string ip;              // 对端IP地址
    uint16_t port;               // 对端端口号
    RDMA_Metadata metadata;      // RDMA元数据
    ibv_pd* pd;                  // 保护域指针
    ibv_ah* ah;                  // 地址句柄指针
    
    PeerInfo() 
        : ip(""), port(0), pd(nullptr), ah(nullptr) {
    }
    
    PeerInfo(const std::string& ip_addr, uint16_t port_num, 
             const RDMA_Metadata& meta = RDMA_Metadata(), 
             ibv_pd* protection_domain = nullptr, 
             ibv_ah* address_handle = nullptr)
        : ip(ip_addr), port(port_num), metadata(meta), 
          pd(protection_domain), ah(address_handle) {
    }
};

struct mmsghdr;

// Format a fd_set into a string for logging
// Check nfd to know how many 32 bits hexs do we want to sprintf into user buffer
const char* sprintf_fdset(char* buf, int buflen, int __nfds, fd_set *__fds);


// zc add
class Socket_transbridge{
public:
	// Socket类型枚举
    enum SocketType {
        SOCKET_TYPE_TCP = 0,
        SOCKET_TYPE_UDP = 1
    };


	Socket_transbridge(int fd);
    virtual ~Socket_transbridge();

    // 通用接口
    
    virtual int close(){
        return 0;
    }
    

    virtual    int socket(){
        return 0;
    }
    virtual    int listen(int backlog){
        if(backlog)
        return 0;
    }
    virtual    int accept(){
        return 0;
    }
    virtual    int connect(){
        return 0;
    }
    virtual    int bind(){
        return 0;
    }


            // 数据收发
    virtual    ssize_t send(__const void *__buf, size_t __nbytes, int __flags){
        if(__buf){
            if(__nbytes){
                if(__flags){

                }
            }
        }
        return 0;
    }
    virtual    ssize_t recv(void *buf, size_t nbytes, int flags = 0){
        if(buf){
            if(nbytes){
                if(flags){
                    
                }
            }
        }
        return 0;
    }
        
            // 便捷读写接口
    virtual    ssize_t write(__const void *__buf, size_t __nbytes){
        if(__buf){
            if(__nbytes){
            
            }
        }
        return 0;
    }
    virtual    ssize_t read(void *buf, size_t nbytes){
        if(buf){
            if(nbytes){
                
            }
        }
        return 0;
    }

        // UDP数据收发
    ssize_t sendto(__const void *__buf, size_t __nbytes, int __flags,
	       const struct sockaddr *__to, socklen_t __tolen){
        if(__buf){
            if(__nbytes){
                if(__flags){
                    if(__to || __tolen){
                        
                    }
                }
            }
        }

        return 0;
    }
    //note !! sendto sendmsg类似的中，都默认了bind的操作，如果这个套接字没有和某个ip地址和端口绑定的话，此时会默认绑定
    
    ssize_t recvfrom(void *buf, size_t nbytes, int flags,
                     sockaddr *srcAddr, socklen_t *addrlen){
        if(buf){
            if(nbytes){
                if(flags){
                    if(srcAddr || addrlen){

                    }
                }
            }
        }
        return 0;
    }

    ssize_t sendmsg(const struct msghdr *msg, int flags){
        if(msg){
            if(flags){

            }
        }
        return 0;
    }
    
    ssize_t recvmsg(struct msghdr *__msg, int __flags){
        if(__msg){
            if(__flags){

            }
        }
        return 0;
    }

    // 地址绑定
    void extractAddressFromSockaddr(const sockaddr_in *addr) {
        if (!addr || addr->sin_family != AF_INET) {
            m_bind_ip.clear();
            m_bind_port = 0;
            m_isBound = false;
            return;
        }

        char ip_str[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &addr->sin_addr, ip_str, sizeof(ip_str))) {
            m_bind_ip = ip_str;
            m_bind_port = ntohs(addr->sin_port);
            m_isBound = true;
        } else {
            m_bind_ip.clear();
            m_bind_port = 0;
            m_isBound = false;
        }
    }

    virtual int getsockopt(int __level, int __optname, void *__optval,
                       socklen_t *__optlen) {
        // 最基本的参数检查
        if (!__optval || !__optlen) {
            return -1;  // 设置errno
        }
        // 避免未使用参数警告
        (void)__level;
        (void)__optname;
        (void)__optval;
        (void)__optlen;
        return 0;  // 总是成功
    }

    virtual int ioctl(unsigned long int __request, unsigned long int __arg) {
        // 最简单的实现
        (void)__request;  // 避免未使用参数警告
        (void)__arg;      // 避免未使用参数警告
        return 0;         // 总是成功
    }

    virtual int setsockopt(int __level, int __optname,
                           const void *__optval, socklen_t __optlen) {
        // 最基本的参数检查
        if (!__optval && __optlen > 0) {
            return -1;  // 设置errno
        }

        // 避免未使用参数警告
        (void)__level;
        (void)__optname;
        (void)__optval;
        (void)__optlen;

        return 0;  // 总是成功
    }


    bool isBound() const { return m_isBound; }

    // 获取socket描述符
    int getFd() const { return m_fd; }
    
    // 设置非阻塞模式
    virtual int setNonBlocking(bool nonblocking){
        if(nonblocking)
        {
            
        }
        return 0;
    }

	// 获取socket类型（返回枚举值）
    SocketType getType() const { return m_type; }


protected:
    int m_fd;
	SocketType m_type;  // socket类型

    bool islistenserver;

    // 绑定的IP地址和端口号
    std::string m_bind_ip;   // 绑定的IP地址
    uint16_t m_bind_port;    // 绑定的端口号
    bool m_isBound;          // 是否已绑定地址

	void setType(SocketType type) { m_type = type; }  // 保护方法，子类可调用

};

class Socket_tb_tcp : public Socket_transbridge{
public:
	
	Socket_tb_tcp(int fd);
	virtual ~Socket_tb_tcp();

	// TCP特定操作
    int socket();
    int listen(int backlog);
    int accept();
    int connect();
    int bind();


    // 数据收发
    ssize_t send(__const void *__buf, size_t __nbytes, int __flags);
    ssize_t recv(void *buf, size_t nbytes, int flags = 0);
    
    // 便捷读写接口
    ssize_t write(__const void *__buf, size_t __nbytes);
    ssize_t read(void *buf, size_t nbytes);
    
    // 带缓冲的收发
    ssize_t rx(void *buf, size_t nbytes, int flags = 0);
    ssize_t tx(__const void *__buf, size_t __nbytes, int __flags);
    
    // TCP特有选项设置
    virtual int setKeepAlive(bool enable, int idle = 60, int interval = 10, int count = 5) = 0;
    virtual int setNoDelay(bool enable) = 0;  // 禁用Nagle算法

    // 判断是否已绑定地址
    bool isBound() const { return !m_bind_ip.empty() && m_bind_port != 0; }

    // 状态查询
    bool isListening() const { return m_isListening; }
    bool isConnected() const { return m_isConnected; }

private:
    bool m_isListening;
    bool m_isConnected;
	int send_buffer_current;
	int	recv_buffer_current;
    

    // 连接信息缓存
    sockaddr_in m_localAddr;
    sockaddr_in m_remoteAddr;
    bool m_addrCached;

};



class Socket_tb_udp : public Socket_transbridge{
public:

	Socket_tb_udp(int fd);
	virtual ~Socket_tb_udp();
 // UDP特定操作
    int socket();
    
    int bind();

    // UDP连接（伪连接，设置默认目标地址）
    int connect(const sockaddr *addr, socklen_t addrlen){
        if(addr || addrlen){
            
        }
        return 0;
    };
    int disconnect(){
        return 0;
    }  // 断开伪连接
    
    // UDP数据收发
    ssize_t sendto(__const void *__buf, size_t __nbytes, int __flags,
	       const struct sockaddr *__to, socklen_t __tolen);
    //note !! sendto sendmsg类似的中，都默认了bind的操作，如果这个套接字没有和某个ip地址和端口绑定的话，此时会默认绑定
    
    ssize_t recvfrom(void *buf, size_t nbytes, int flags,
                     sockaddr *srcAddr, socklen_t *addrlen);

    ssize_t sendmsg(const struct msghdr *msg, int flags);
                     
    ssize_t fallback_to_normal_sendmsg(const struct msghdr *msg, int flags, 
                                                  const struct sockaddr_in* to_addr);

    bool establish_rdma_connection(const char* remote_ip, int remote_port,
                                 const struct sockaddr* to_addr);
                                    
    void updateRdmaLocalAddress(uint32_t new_ip, uint16_t new_port);

    bool send_metadata(int sockfd, const sockaddr* to, socklen_t tolen);
    bool receive_metadata(int sockfd, sockaddr* from, socklen_t* fromlen, RDMA_Metadata& metadata);

    bool post_send_to_peer(const PeerInfo* peer, size_t data_len);

    ssize_t handle_udp_metadata(char* temp_buf, ssize_t recv_len,
                                sockaddr_in& src_addr, socklen_t src_len);

    ssize_t recvmsg(struct msghdr *__msg, int __flags);

    int getsockopt(int __level, int __optname, void *__optval, socklen_t *__optlen);
    
    int ioctl(unsigned long int __request, unsigned long int __arg);

    int setsockopt(int __level, int __optname,  __const void *__optval, socklen_t __optlen);

    int handle_ip_options(int optname, void *optval, socklen_t *optlen);
    int handle_socket_options(int optname, void *optval, socklen_t *optlen);



    // 连接模式下的收发（使用connect设置默认地址后）
    virtual ssize_t send(const void *buf, size_t nbytes, int flags = 0) = 0;
    virtual ssize_t recv(void *buf, size_t nbytes, int flags = 0) = 0;
    
    // 广播和多播支持
    virtual int setBroadcast(bool enable) = 0;
    virtual int joinMulticastGroup(const char* multicastAddr, const char* localAddr = nullptr) = 0;
    virtual int leaveMulticastGroup(const char* multicastAddr) = 0;
    
    // UDP特有选项
    virtual int setReuseAddr(bool enable) = 0;
    virtual int setMulticastTTL(int ttl) = 0;
    virtual int setMulticastLoop(bool enable) = 0;
    
    // 接收超时设置
    virtual int setRecvTimeout(int seconds, int microseconds = 0) = 0;

    // RDMA管理器 - 每个socket管理自己的RDMA资源
    UDRdmaManager* m_rdma_manager;

private:
    bool m_isConnected;  // 是否设置了默认目标地址
    bool m_broadcastEnabled;
    
    // 默认目标地址（用于connect模式）
    sockaddr_in m_defaultDestAddr;

    // 初始化RDMA管理器
    bool m_rdma_initialized;
    bool initRdmaManager(uint32_t local_ip, uint16_t local_port, int sockfd);

    bool isbound;

    ibv_pd* m_pd;  // 保护域指针

};


// 全局对端管理器 - 单例模式
class GlobalPeerManager {
public:
    // 删除拷贝构造函数和赋值操作符
    GlobalPeerManager(const GlobalPeerManager&) = delete;
    GlobalPeerManager& operator=(const GlobalPeerManager&) = delete;
    
    // 获取单例实例
    static GlobalPeerManager& instance();
    
    // ============ 对端管理 ============
    // 添加对端信息
    bool add_peer(const std::string& ip, uint16_t port, 
                  const RDMA_Metadata& metadata, ibv_pd* pd = nullptr);
    
    // 更新对端信息
    bool update_peer(const std::string& ip, uint16_t port, 
                     const RDMA_Metadata& metadata);
    
    // 移除对端
    bool remove_peer(const std::string& ip, uint16_t port);
    
    // 获取对端信息
    const PeerInfo* get_peer(const std::string& ip, uint16_t port) const;
    
    // 获取对端的地址句柄（如果不存在则创建）- 注意：现在基于GID创建AH
    ibv_ah* get_or_create_ah(const std::string& ip, uint16_t port, 
                             ibv_pd* pd, uint8_t port_num = 1);
    
    // 检查对端是否存在
    bool has_peer(const std::string& ip, uint16_t port) const;
    
    // 获取对端数量
    size_t get_peer_count() const;
    
    // 打印所有对端信息
    void print_all_peers() const;
    
    // 清理指定PD创建的所有AH
    void cleanup_pd_ahs(ibv_pd* pd);
    
    // 清理所有资源
    void cleanup();
    
    // ============ 工具函数 ============
    // 生成对端键
    static std::string make_peer_key(const std::string& ip, uint16_t port);
    
private:
    // 私有构造函数和析构函数
    GlobalPeerManager();
    ~GlobalPeerManager();
    
    // 创建地址句柄（基于RoCE GID）
    ibv_ah* create_ah(const PeerInfo& peer, ibv_pd* pd, uint8_t port_num);
    
    // 销毁地址句柄
    void destroy_ah(ibv_ah* ah);
    
private:
    // 对端信息映射表
    std::unordered_map<std::string, PeerInfo> peer_map_;
    
    // 互斥锁，保护对端映射表
    mutable std::mutex mutex_;
};


/* *
 *-----------------------------------------------------------------------------
 *  variables to hold the function-pointers to original functions
 *-----------------------------------------------------------------------------
 */

struct os_api {
	int (*creat) (const char *__pathname, mode_t __mode);
	int (*open) (__const char *__file, int __oflag, ...);
	int (*dup) (int fildes);
	int (*dup2) (int fildes, int fildes2);
	int (*pipe) (int __filedes[2]);
	int (*socket) (int __domain, int __type, int __protocol);
	int (*socketpair) (int __domain, int __type, int __protocol, int __sv[2]);

	int (*close) (int __fd);
	int (*__res_iclose) (res_state statp, bool free_addr);
	int (*shutdown) (int __fd, int __how);

	int (*accept) (int __fd, struct sockaddr *__addr, socklen_t *__addrlen);
	int (*accept4) (int __fd, struct sockaddr *__addr, socklen_t *__addrlen, int __flags);
	int (*bind) (int __fd, const struct sockaddr *__addr, socklen_t __addrlen);
	int (*connect) (int __fd, const struct sockaddr *__to, socklen_t __tolen);
	int (*listen) (int __fd, int __backlog);

	int (*setsockopt) (int __fd, int __level, int __optname, __const void *__optval, socklen_t __optlen);
	int (*getsockopt) (int __fd, int __level, int __optname, void *__optval, socklen_t *__optlen);
	int (*fcntl) (int __fd, int __cmd, ...);
	int (*fcntl64) (int __fd, int __cmd, ...);
	int (*ioctl) (int __fd, unsigned long int __request, ...);
	int (*getsockname) (int __fd, struct sockaddr *__name,socklen_t *__namelen);
	int (*getpeername) (int __fd, struct sockaddr *__name,socklen_t *__namelen);

	ssize_t (*read) (int __fd, void *__buf, size_t __nbytes);
#if defined HAVE___READ_CHK
	ssize_t (*__read_chk) (int __fd, void *__buf, size_t __nbytes, size_t __buflen);
#endif
	ssize_t (*readv) (int __fd, const struct iovec *iov, int iovcnt);
	ssize_t (*recv) (int __fd, void *__buf, size_t __n, int __flags);
#if defined HAVE___RECV_CHK
	ssize_t (*__recv_chk) (int __fd, void *__buf, size_t __n,  size_t __buflen, int __flags);
#endif
	ssize_t (*recvmsg) (int __fd, struct msghdr *__message, int __flags);
	int (*recvmmsg) (int __fd, struct mmsghdr *__mmsghdr, unsigned int __vlen, int __flags, const struct timespec *__timeout);

	ssize_t (*recvfrom) (int __fd, void *__restrict __buf, size_t __n, int __flags, struct sockaddr *__from, socklen_t *__fromlen);
#if defined HAVE___RECVFROM_CHK
	ssize_t (*__recvfrom_chk) (int __fd, void *__restrict __buf, size_t __n, size_t __buflen, int __flags, struct sockaddr *__from, socklen_t *__fromlen);
#endif

	ssize_t (*write) (int __fd, __const void *__buf, size_t __n);
	ssize_t (*writev) (int __fd, const struct iovec *iov, int iovcnt);
	ssize_t (*send) (int __fd, __const void *__buf, size_t __n, int __flags);
	ssize_t (*sendmsg) (int __fd, __const struct msghdr *__message, int __flags);
	ssize_t (*sendmmsg) (int __fd, struct mmsghdr *__mmsghdr, unsigned int __vlen, int __flags);
	ssize_t (*sendto) (int __fd, __const void *__buf, size_t __n,int __flags, const struct sockaddr *__to, socklen_t __tolen);
	ssize_t (*sendfile) (int out_fd, int in_fd, off_t *offset, size_t count);
	ssize_t (*sendfile64) (int out_fd, int in_fd, __off64_t *offset, size_t count);

	int (*select) (int __nfds, fd_set *__readfds, fd_set *__writefds, fd_set *__exceptfds, struct timeval *__timeout);
	int (*pselect) (int __nfds, fd_set *__readfds, fd_set *__writefds, fd_set *__errorfds, const struct timespec *__timeout, const sigset_t *__sigmask);

	int (*poll) (struct pollfd *__fds, nfds_t __nfds, int __timeout);
#if defined HAVE___POLL_CHK
	int (*__poll_chk) (struct pollfd *__fds, nfds_t __nfds, int __timeout, size_t __fdslen);
#endif
	int (*ppoll) (struct pollfd *__fds, nfds_t __nfds, const struct timespec *__timeout, const sigset_t *__sigmask);
#if defined HAVE___PPOLL_CHK
	int (*__ppoll_chk) (struct pollfd *__fds, nfds_t __nfds, const struct timespec *__timeout, const sigset_t *__sigmask, size_t __fdslen);
#endif
	int (*epoll_create) (int __size);
	int (*epoll_create1) (int __flags);
	int (*epoll_ctl) (int __epfd, int __op, int __fd, struct epoll_event *__event);
	int (*epoll_wait) (int __epfd, struct epoll_event *__events, int __maxevents, int __timeout);
	int (*epoll_pwait) (int __epfd, struct epoll_event *__events, int __maxevents, int __timeout, const sigset_t *sigmask);

	int (*clone) (int (*__fn)(void *), void *__child_stack, int __flags, void *__arg);
	pid_t (*fork) (void);
	pid_t (*vfork) (void);
	int (*daemon) (int __nochdir, int __noclose);

	int (*sigaction) (int signum, const struct sigaction *act, struct sigaction *oldact);
	sighandler_t (*signal) (int signum, sighandler_t handler);
};

/**
 *-----------------------------------------------------------------------------
 *  variables to hold the function-pointers to original functions
 *-----------------------------------------------------------------------------
 */
extern os_api orig_os_api;

extern void get_orig_funcs();

extern iomux_stats_t* g_p_select_stats;
extern iomux_stats_t* g_p_poll_stats;
extern iomux_stats_t* g_p_epoll_stats;

int do_global_ctors();
void reset_globals();
void handle_close(int fd, bool cleanup = false, bool passthrough = false);

// allow calling our socket(...) implementation safely from within libvma.so
// this is critical in case VMA was loaded using dlopen and not using LD_PRELOAD
// TODO: look for additional such functions/calls
int socket_internal(int __domain, int __type, int __protocol, bool check_offload = false);

#endif  //SOCK_REDIRECT_H


