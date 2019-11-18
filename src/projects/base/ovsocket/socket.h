//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include "socket_address.h"

#include <sys/socket.h>
#if defined(__APPLE__)
#include <sys/event.h>

typedef union epoll_data {
	void    *ptr;
	int      fd;
	uint32_t u32;
	uint64_t u64;
} epoll_data_t;

struct epoll_event {
	uint32_t		events;
	epoll_data_t	data;
};

static inline int epoll_create1(int)
{
	return kqueue();
}

// epoll_ctl flags
constexpr int EPOLL_CTL_ADD = 1;
constexpr int EPOLL_CTL_DEL = 2;

// epoll_event event values
constexpr int EPOLLIN  		= 0x0001;
constexpr int EPOLLOUT		= 0x0002;
constexpr int EPOLLHUP 		= 0x0004;
constexpr int EPOLLERR 		= 0x0008;
constexpr int EPOLLRDHUP 	= 0x0010;
constexpr int EPOLLPRI		= 0x0020;
constexpr int EPOLLRDNORM	= 0x0040;
constexpr int EPOLLRDBAND	= 0x0080;
constexpr int EPOLLWRNORM	= 0x0100;
constexpr int EPOLLWRBAND	= 0x0200;
constexpr int EPOLLMSG		= 0x0400;
constexpr int EPOLLWAKEUP	= 0x0800;
constexpr int EPOLLONESHOT	= 0x1000;
constexpr int EPOLLET		= 0x2000;

static inline int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
	int flags = 0, fflags = 0;
	struct kevent ke {};
	switch (op)
	{
	case EPOLL_CTL_ADD:
		flags = EV_ADD;
		if (event->events & EPOLLIN)
		{
			fflags |= EVFILT_READ;
		}
		if (event->events & EPOLLOUT)
		{
			fflags |= EVFILT_WRITE;
		}
		break;
	case EPOLL_CTL_DEL:
		flags = EV_DELETE;
		break;
	}
	EV_SET(&ke, fd, flags, fflags, 0, 0, event->data.ptr);
	return kevent(epfd, &ke, 1, nullptr, 0, nullptr);
}

static inline int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout)
{
	struct kevent ke[maxevents];
	const timespec t 
	{
		.tv_sec = timeout / 1000,
		.tv_nsec = (timeout % 1000) * 1000 * 1000
	};
	return kevent(epfd, nullptr, 0, ke, maxevents, timeout == -1 ? nullptr : & t);
}

// macOS does not have a MSG_NOSIGNAL, has a SO_NOSIGPIPE, need to test to understand equality
#define MSG_NOSIGNAL 0x2000
#else
#include <sys/epoll.h>
#endif
#include <netinet/in.h>

#include <utility>
#include <memory>
#include <map>
#include <functional>

// for SRT
#include <srt/srt.h>

#include <base/ovlibrary/ovlibrary.h>

namespace ov
{
	// socket type
	typedef int socket_t;

	// socket()에서 실패했을때의 값
	const int InvalidSocket = -1;

	constexpr const int EpollMaxEvents = 1024;

	constexpr const int MaxSrtPacketSize = 1316;

	enum class SocketType : char
	{
		Unknown,
		Udp,
		Tcp,
		Srt
	};

	enum class SocketState : char
	{
		Closed,
		Created,
		Bound,
		Listening,
		Connected,
		Error,
	};

	// socket_t와 SRTSOCKET의 타입이 int로 같으므로, 이를 추상화
	struct SocketWrapper
	{
	public:
		SocketWrapper() = default;

		explicit SocketWrapper(SocketType type, int sock)
		{
			SetSocket(type, sock);
		}

		bool operator ==(const int &sock) const
		{
			switch(_type)
			{
				case SocketType::Tcp:
				case SocketType::Udp:
					return _socket.socket == sock;

				case SocketType::Srt:
					return _socket.srt_socket == sock;

				default:
					return (sock == InvalidSocket);
			}
		}

		bool operator !=(const int &sock) const
		{
			return !operator ==(sock);
		}

		int GetSocket() const
		{
			switch(_type)
			{
				case SocketType::Tcp:
				case SocketType::Udp:
					return _socket.socket;

				case SocketType::Srt:
					return _socket.srt_socket;

				default:
					return InvalidSocket;
			}
		}

		void SetSocket(SocketType type, int sock)
		{
			switch(type)
			{
				case SocketType::Tcp:
				case SocketType::Udp:
					_socket.socket = sock;

					if(sock != InvalidSocket)
					{
						_type = type;
						_valid = true;
					}
					break;

				case SocketType::Srt:
					_socket.srt_socket = sock;

					if(sock != SRT_INVALID_SOCK)
					{
						_type = type;
						_valid = true;
					}
					break;

				default:
					OV_ASSERT2(type == SocketType::Unknown);
					OV_ASSERT2(sock == InvalidSocket);

					_type = SocketType::Unknown;
					_socket.socket = InvalidSocket;
					break;
			}
		}

		void SetValid(bool valid)
		{
			_valid = valid;
		}

		const bool IsValid() const noexcept
		{
			return _valid;
		}

		SocketType GetType() const
		{
			return _type;
		}

		SocketWrapper &operator =(const SocketWrapper &wrapper) = default;

	protected:
		SocketType _type = SocketType::Unknown;
		bool _valid = false;

		union
		{
			socket_t socket;
			SRTSOCKET srt_socket;
		} _socket { InvalidSocket };
	};

	class Socket : public EnableSharedFromThis<Socket>
	{
	public:
		Socket();
		Socket(SocketWrapper socket, const SocketAddress &remote_address);
		// socket은 복사 불가능 (descriptor 까지 복사되는 것 방지)
		Socket(const Socket &socket) = delete;
		Socket(Socket &&socket) noexcept;
		virtual ~Socket();

		bool Create(SocketType type);
		bool MakeNonBlocking();

		bool Bind(const SocketAddress &address);

		bool Listen(int backlog = SOMAXCONN);

		template<typename T>
		std::shared_ptr<T> AcceptClient()
		{
			SocketAddress client;
			SocketWrapper client_socket = AcceptClientInternal(&client);

			if(client_socket.IsValid())
			{
				// Accept()는 TCP에서만 일어남
				return std::make_shared<T>(client_socket, client);
			}

			return nullptr;
		}

		std::shared_ptr<ov::Error> Connect(const SocketAddress &endpoint, int timeout = Infinite);

		bool PrepareEpoll();
		bool AddToEpoll(Socket *socket, void *parameter);
		int EpollWait(int timeout = Infinite);
		const epoll_event *EpollEvents(int index);
		bool RemoveFromEpoll(Socket *socket);

		std::shared_ptr<SocketAddress> GetLocalAddress() const;
		std::shared_ptr<SocketAddress> GetRemoteAddress() const;

		// for normal socket
		template<class T>
		bool SetSockOpt(int option, const T &value)
		{
			return SetSockOpt(option, &value, (socklen_t)sizeof(T));
		}

		bool SetSockOpt(int option, const void *value, socklen_t value_length);

		// for SRT
		template<class T>
		bool SetSockOpt(SRT_SOCKOPT option, const T &value)
		{
			return SetSockOpt(option, &value, static_cast<int>(sizeof(T)));
		}

		bool SetSockOpt(SRT_SOCKOPT option, const void *value, int value_length);

		// 현재 소켓의 접속 상태
		SocketState GetState() const;

		void SetState(SocketState state);

		SocketWrapper GetSocket() const
		{
			return _socket;
		}

		// 소켓 타입
		SocketType GetType() const;

		// 데이터 송신
		virtual ssize_t Send(const void *data, size_t length);
		virtual ssize_t Send(const std::shared_ptr<const Data> &data);
        virtual ssize_t Send(const void *data, size_t length, bool &is_retry);

		virtual ssize_t SendTo(const ov::SocketAddress &address, const void *data, size_t length);
		virtual ssize_t SendTo(const ov::SocketAddress &address, const std::shared_ptr<const Data> &data);

		virtual // 데이터 수신
		// 최대 ByteData의 capacity만큼 데이터를 기록
		// false가 반환되면 error를 체크해야 함
		std::shared_ptr<ov::Error> Recv(std::shared_ptr<Data> &data);

		virtual // 최대 ByteData의 capacity만큼 데이터를 기록
		// nullptr이 반환되면 errno를 체크해야 함
		std::shared_ptr<ov::Error> RecvFrom(std::shared_ptr<Data> &data, std::shared_ptr<ov::SocketAddress> *address);

		// 소켓을 닫음
		virtual bool Close();

		virtual String ToString() const;

	protected:
		SocketWrapper AcceptClientInternal(SocketAddress *client);

		// utility method
		static String StringFromEpollEvent(const epoll_event *event);
		static String StringFromEpollEvent(const epoll_event &event);

		virtual String ToString(const char *class_name) const;

	protected:
		SocketWrapper _socket;

		SocketState _state = SocketState::Closed;

		std::shared_ptr<ov::SocketAddress> _local_address = nullptr;
		std::shared_ptr<ov::SocketAddress> _remote_address = nullptr;

		bool _is_nonblock = false;

		// Related to epoll
		// for normal socket
		socket_t _epoll = InvalidSocket;
		// for srt socket
		std::map<SRTSOCKET, void *> _srt_parameter_map;
		int _srt_epoll = SRT_INVALID_SOCK;
		epoll_event *_epoll_events = nullptr;
		int _last_epoll_event_count = 0;
	};
}
