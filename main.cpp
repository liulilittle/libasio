#include <stdio.h>
#include <signal.h>
#include <limits.h>
#ifdef _WIN32
#include <WinSock2.h>
#include <Windows.h>

#pragma comment(lib, "Ws2_32.lib")
#else
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/file.h>
#endif

#include <unordered_map>
#include <functional>
#include <thread>
#include <boost/asio.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#ifdef _WIN32
namespace boost { // boost::asio::posix::stream_descriptor
    namespace asio {
        namespace posix {
            typedef boost::asio::windows::stream_handle stream_descriptor;
        }
    }
}
#include <WinSock2.h>
#else
namespace boost {
    namespace asio {
        typedef io_service io_context;
    }
}
#endif

#ifndef LIBASIO_API
#ifdef __cplusplus 
#ifdef _WIN32
#define LIBASIO_API extern "C" __declspec(dllexport)
#else
#define LIBASIO_API extern "C" __attribute__((visibility("default")))
#endif
#else
#define LIBASIO_API
#endif
#endif

#pragma pack(push, 1)
typedef struct {
    uint32_t            v4_or_v6_;
    union {
        struct {
            uint32_t    address_;
            uint32_t    port_;
        } in4_;
        struct {
            char        address_[16];
            uint32_t    port_;
        } in6_;
        char            data_[20];
    };
} libasio_endpoint;
#pragma pack(pop)

typedef void(*libasio_delay_callback)(void* context_, uint64_t key_, int err_);
typedef void(*libasio_post_callback)(void* context_, uint64_t key_);
typedef void(*libasio_sendto_callback)(void* socket_, uint64_t key_, int length_);
typedef void(*libasio_recvfrom_callback)(void* socket_, uint64_t key_, int length_, libasio_endpoint* remoteEP_);

typedef std::shared_ptr<boost::asio::ip::udp::socket>                       libasio_socket;
typedef std::shared_ptr<boost::asio::io_context>                            libasio_context;
typedef std::shared_ptr<boost::asio::deadline_timer>                        libasio_timer;

typedef std::unordered_map<boost::asio::io_context*, libasio_context>       libasio_context_hashtable;
typedef std::unordered_map<boost::asio::ip::udp::socket*, libasio_socket>   libasio_socket_hashtable;
typedef std::unordered_map<boost::asio::ip::udp::socket*, libasio_context>  libasio_so2ctx;
typedef std::unordered_map<uint64_t, libasio_timer>                         libasio_timer_hashtable;

static libasio_timer_hashtable                                              _ltimers_;
static libasio_so2ctx                                                       _so2ctxs_;
static libasio_socket_hashtable                                             _sockets_;
static libasio_context_hashtable                                            _contexts_;
static std::mutex                                                           _syncobj_;

#define __lock__(LOCKOBJ) \
do {\
    std::lock_guard<std::mutex> __scoped__(LOCKOBJ);
#define __unlock__ \
} while(0);

inline static libasio_context libasio_getcontext(boost::asio::io_context* context_) noexcept {
    libasio_context_hashtable::iterator tail = _contexts_.find(context_);
    libasio_context_hashtable::iterator endl = _contexts_.end();
    if (tail == endl) {
        return NULL;
    }
    return tail->second;
}

inline static libasio_context libasio_getcontext(boost::asio::ip::udp::socket* socket_) noexcept {
    libasio_so2ctx::iterator tail = _so2ctxs_.find(socket_);
    libasio_so2ctx::iterator endl = _so2ctxs_.end();
    if (tail == endl) {
        return NULL;
    }
    return tail->second;
}

inline static libasio_socket libasio_getsocket(boost::asio::ip::udp::socket* socket_) noexcept {
    libasio_socket_hashtable::iterator tail = _sockets_.find(socket_);
    libasio_socket_hashtable::iterator endl = _sockets_.end();
    if (tail == endl) {
        return NULL;
    }
    return tail->second;
}

LIBASIO_API
boost::asio::io_context* libasio_newcontext() noexcept {
    std::shared_ptr<boost::asio::io_context> context_ = std::make_shared<boost::asio::io_context>();
    std::thread([context_] {
#ifdef _WIN32
        SetThreadPriority(GetCurrentProcess(), THREAD_PRIORITY_HIGHEST);
#else
        /* ps -eo state,uid,pid,ppid,rtprio,time,comm */
        struct sched_param param_;
        param_.sched_priority = sched_get_priority_max(SCHED_FIFO); // SCHED_RR
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &param_);
#endif

        boost::asio::io_context::work work_(*context_);
        boost::system::error_code ec_;
        context_->run(ec_);

        __lock__(_syncobj_) {
            std::shared_ptr<boost::asio::io_context> p = libasio_getcontext(context_.get());
            if (p) {
                _contexts_.erase(context_.get());
            }
        } __unlock__;
    }).detach();
    boost::asio::io_context* p = context_.get();
    __lock__(_syncobj_) {
        _contexts_.insert(std::make_pair(p, std::move(context_)));
    } __unlock__;
    return p;
}

LIBASIO_API
void libasio_closecontext(boost::asio::io_context* context_) noexcept {
    if (!context_) {
        return;
    }
    __lock__(_syncobj_) {
        std::shared_ptr<boost::asio::io_context> p_ = libasio_getcontext(context_);
        if (p_) {
            boost::asio::post(*context_, [p_] {
                p_->stop();
            });
            _contexts_.erase(context_);
        }
    } __unlock__;
}

LIBASIO_API
bool libasio_postcontext(boost::asio::io_context* context_, uint64_t key_, libasio_post_callback callback_) noexcept {
    if (!context_ || !callback_) {
        return false;
    }
    __lock__(_syncobj_) {
        std::shared_ptr<boost::asio::io_context> io_context_ = libasio_getcontext(context_);
        if (!io_context_) {
            return false;
        }
        context_->post([io_context_, context_, key_, callback_] {
            callback_(context_, key_);
        });
    } __unlock__;
    return true;
}

LIBASIO_API
boost::asio::ip::udp::socket* libasio_createsocket(boost::asio::io_context* context_, int sockfd_, bool v4_or_v6_) noexcept {
    if (!context_ || sockfd_ == -1) {
        return NULL;
    }
    boost::system::error_code ec_;
    __lock__(_syncobj_) {
        libasio_context context = libasio_getcontext(context_);
        if (!context) {
            return NULL;
        }
        libasio_socket socket_ = std::make_shared<boost::asio::ip::udp::socket>(*context_);
        if (v4_or_v6_) {
            socket_->assign(boost::asio::ip::udp::v4(), sockfd_, ec_);
        }
        else {
            socket_->assign(boost::asio::ip::udp::v6(), sockfd_, ec_);
        }
        if (ec_) {
            return NULL;
        }
        boost::asio::ip::udp::socket* r_ = socket_.get();
        _sockets_.insert(std::make_pair(r_, std::move(socket_)));
        _so2ctxs_.insert(std::make_pair(r_, std::move(context)));
        return r_;
    } __unlock__;
}

LIBASIO_API
void libasio_closesocket(boost::asio::ip::udp::socket* socket_) noexcept {
    if (!socket_) {
        return;
    }
    libasio_context context;
    libasio_socket socket;
    __lock__(_syncobj_) {
        
        libasio_socket_hashtable::iterator tail = _sockets_.find(socket_);
        libasio_socket_hashtable::iterator endl = _sockets_.end();
        if (tail != endl) {
            socket = std::move(tail->second);
            _sockets_.erase(tail);
        }
        libasio_so2ctx::iterator tail2 = _so2ctxs_.find(socket_);
        libasio_so2ctx::iterator endl2 = _so2ctxs_.end();
        if (tail2 != endl2) {
            context = std::move(tail2->second);
            _so2ctxs_.erase(tail2);
        }
        if (socket) {
            boost::asio::post(socket->get_executor(), [context, socket] {
                if (socket->is_open()) {
                    boost::system::error_code ec;
                    try {
                        socket->close(ec);
                    }
                    catch (std::exception&) {}
                }
            });
        }
    } __unlock__;
}

LIBASIO_API
bool libasio_recvfrom(boost::asio::ip::udp::socket* socket_, uint64_t key_, char* buf_, int size_, libasio_recvfrom_callback callback_) noexcept {
    if (!socket_ || !buf_ || size_ < 1 || !callback_) {
        return false;
    }
    std::shared_ptr<boost::asio::ip::udp::endpoint> endpoint_ = std::make_shared<boost::asio::ip::udp::endpoint>();
    __lock__(_syncobj_) {
        libasio_context context_ = libasio_getcontext(socket_);
        if (!context_) {
            return false;
        }
        libasio_socket socket = libasio_getsocket(socket_);
        if (!socket || !socket->is_open()) {
            return false;
        }
        socket->async_receive_from(boost::asio::buffer(buf_, size_), *endpoint_,
            [context_, socket, key_, callback_, endpoint_](const boost::system::error_code& ec, uint32_t sz) noexcept {
                int by = std::max<int>(-1, ec ? -1 : sz);
                libasio_endpoint stack_;
                if (endpoint_->protocol() == boost::asio::ip::udp::v4()) {
                    stack_.v4_or_v6_ = 1;
                    stack_.in4_.address_ = htonl(endpoint_->address().to_v4().to_uint());
                    stack_.in4_.port_ = endpoint_->port();

                    callback_(socket.get(), key_, by, &stack_);
                }
                else if (endpoint_->protocol() == boost::asio::ip::udp::v6()) {
                    stack_.v4_or_v6_ = 0;
                    stack_.in6_.port_ = endpoint_->port();

                    boost::asio::ip::address_v6::bytes_type addr_bytes_ = endpoint_->address().to_v6().to_bytes();
                    memcpy(stack_.in6_.address_, addr_bytes_.data(), addr_bytes_.size());

                    callback_(socket.get(), key_, by, &stack_);
                }
                else {
                    callback_(socket.get(), key_, by, NULL);
                }
            });
    } __unlock__;
    return true;
}

LIBASIO_API
bool libasio_sendto(boost::asio::ip::udp::socket* socket_, uint64_t key_, char* buf_, int size_, libasio_endpoint* endpoint_, libasio_sendto_callback callback_) noexcept {
    if (!socket_ || !buf_ || size_ < 1 || !endpoint_) {
        return false;
    }
    boost::asio::ip::udp::endpoint sendtoEP_;
    if (endpoint_->v4_or_v6_) {
        sendtoEP_ = boost::asio::ip::udp::endpoint(boost::asio::ip::address_v4(ntohl(endpoint_->in4_.address_)), endpoint_->in4_.port_);
    }
    else {
        boost::asio::ip::address_v6::bytes_type addr_bytes_;
        memcpy(addr_bytes_.data(), endpoint_->in6_.address_, addr_bytes_.size());
        sendtoEP_ = boost::asio::ip::udp::endpoint(boost::asio::ip::address_v6(addr_bytes_), endpoint_->in6_.port_);
    }
    __lock__(_syncobj_) {
        libasio_context context_ = libasio_getcontext(socket_);
        if (!context_) {
            return false;
        }
        libasio_socket socket = libasio_getsocket(socket_);
        if (!socket || !socket->is_open()) {
            return false;
        }
        socket->async_send_to(boost::asio::buffer(buf_, size_), sendtoEP_,
            [context_, socket, key_, callback_](const boost::system::error_code& ec, uint32_t sz) noexcept {
                if (callback_) {
                    int by = std::max<int>(-1, ec ? -1 : sz);
                    callback_(socket.get(), key_, by);
                }
            });
    } __unlock__;
    return true;
}

LIBASIO_API
bool libasio_stopdelay(uint64_t key_) noexcept {
    boost::system::error_code ec_;
    __lock__(_syncobj_) {
        libasio_timer_hashtable::iterator tail_ = _ltimers_.find(key_);
        libasio_timer_hashtable::iterator endl_ = _ltimers_.end();
        if (tail_ == endl_) {
            return false;
        }
        try {
            tail_->second->cancel(ec_);
        }
        catch (std::exception&) {}
        _ltimers_.erase(tail_);
        return true;
    } __unlock__;
}

LIBASIO_API
bool libasio_opendelay(boost::asio::io_context* context_, uint64_t key_, int timeout_, libasio_delay_callback callback_) noexcept {
    if (!context_ || !callback_) {
        return false;
    }
    boost::system::error_code ec_;
    __lock__(_syncobj_) {
        std::shared_ptr<boost::asio::io_context> io_context_ = libasio_getcontext(context_);
        if (!io_context_) {
            return false;
        }
        else {
            libasio_timer_hashtable::iterator tail_ = _ltimers_.find(key_);
            libasio_timer_hashtable::iterator endl_ = _ltimers_.end();
            if (tail_ != endl_) {
                return false;
            }
        }
        std::shared_ptr<boost::asio::deadline_timer> io_timer_ = std::make_shared<boost::asio::deadline_timer>(*io_context_);
        if (!io_timer_) {
            return false;
        }
        io_timer_->expires_from_now(boost::posix_time::milliseconds(timeout_), ec_);
        if (ec_) {
            return false;
        }
        _ltimers_.insert(std::make_pair(key_, io_timer_));
        io_timer_->async_wait([io_context_, io_timer_, key_, callback_](const boost::system::error_code& ec_) noexcept {
            libasio_stopdelay(key_);
            if (callback_) {
                callback_(io_context_.get(), key_, ec_.value());
            }
        });
    } __unlock__;
    return true;
}

LIBASIO_API
int libasio_sendto2(boost::asio::ip::udp::socket* socket_, char* buf_, int size_, libasio_endpoint* endpoint_) noexcept {
    if (!socket_ || !buf_ || size_ < 1) {
        return -1;
    }

    libasio_socket socket;
    libasio_context context_;
    do {
        __lock__(_syncobj_) {
            context_ = libasio_getcontext(socket_);
            if (!context_) {
                return -1;
            }

            socket = libasio_getsocket(socket_);
            if (!socket || !socket->is_open()) {
                return -1;
            }
        } __unlock__;
    } while (0);

    boost::asio::ip::udp::endpoint sendtoEP_;
    if (endpoint_->v4_or_v6_) {
        sendtoEP_ = boost::asio::ip::udp::endpoint(boost::asio::ip::address_v4(ntohl(endpoint_->in4_.address_)), endpoint_->in4_.port_);
    }
    else {
        boost::asio::ip::address_v6::bytes_type addr_bytes_;
        memcpy(addr_bytes_.data(), endpoint_->in6_.address_, addr_bytes_.size());
        sendtoEP_ = boost::asio::ip::udp::endpoint(boost::asio::ip::address_v6(addr_bytes_), endpoint_->in6_.port_);
    }

    size_t by_;
    try {
        boost::system::error_code ec_;
        by_ = socket->send_to(boost::asio::buffer(buf_, size_), sendtoEP_, 0, ec_);
        if (ec_) {
            return -1;
        }
    }
    catch (std::exception&) {
        return -1;
    }
    return std::max<int>(0, by_);
}

LIBASIO_API
int libasio_recvfrom2(boost::asio::ip::udp::socket* socket_, char* buf_, int size_, libasio_endpoint* endpoint_) noexcept {
    if (!socket_ || !buf_ || size_ < 1) {
        return -1;
    }

    libasio_socket socket;
    libasio_context context_;
    do {
        __lock__(_syncobj_) {
            context_ = libasio_getcontext(socket_);
            if (!context_) {
                return -1;
            }

            socket = libasio_getsocket(socket_);
            if (!socket || !socket->is_open()) {
                return -1;
            }
        } __unlock__;
    } while (0);

    boost::asio::ip::udp::endpoint ep_;
    size_t by_;
    try {
        boost::system::error_code ec_;
        by_ = socket->receive_from(boost::asio::buffer(buf_, size_), ep_, 0, ec_);
        if (ec_) {
            return -1;
        }
    }
    catch (std::exception&) {
        return -1;
    }

    if (endpoint_) {
        libasio_endpoint& stack_ = *endpoint_;
        if (ep_.protocol() == boost::asio::ip::udp::v4()) {
            stack_.v4_or_v6_ = 1;
            stack_.in4_.address_ = htonl(ep_.address().to_v4().to_uint());
            stack_.in4_.port_ = ep_.port();

        }
        else if (ep_.protocol() == boost::asio::ip::udp::v6()) {
            stack_.v4_or_v6_ = 0;
            stack_.in6_.port_ = ep_.port();

            boost::asio::ip::address_v6::bytes_type addr_bytes_ = ep_.address().to_v6().to_bytes();
            memcpy(stack_.in6_.address_, addr_bytes_.data(), addr_bytes_.size());
        }
    }
    return std::max<int>(0, by_);
}