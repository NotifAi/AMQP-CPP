/**
 *  TcpResolver.h
 *
 *  Class that is used for the DNS lookup of the hostname of the RabbitMQ 
 *  server, and to make the initial connection
 *
 *  @author Emiel Bruijntjes <emiel.bruijntjes@copernica.com>
 *  @copyright 2015 - 2018 Copernica BV
 */

/**
 *  Include guard
 */
#pragma once

/**
 *  Dependencies
 */
#include <thread>
#include <iostream>
#include <mutex>
#include <atomic>
#include <csignal>
#include <sys/epoll.h>

#include "pipe.h"
#include "tcpstate.h"
#include "tcpclosed.h"
#include "tcpconnected.h"
#include "openssl.h"
#include "sslhandshake.h"

/**
 *  Set up namespace
 */
namespace AMQP {

/**
 *  Class definition
 */
class TcpResolver : public TcpExtState {
private:
	/**
	 *  The hostname that we're trying to resolve
	 *  @var std::string
	 */
	std::string _hostname;

	/**
	 *  Should we be using a secure connection?
	 *  @var bool
	 */
	bool _secure;

	/**
	 *  The portnumber to connect to
	 *  @var uint16_t
	 */
	uint16_t _port;

	/**
	 *  A pipe that is used to send back the socket that is connected to RabbitMQ
	 *  @var Pipe
	 */
	Pipe _pipe;

	/**
	 *  Possible error that occured
	 *  @var std::string
	 */
	std::string _error;

	/**
	 *  Data that was sent to the connection, while busy resolving the hostname
	 *  @var TcpBuffer
	 */
	TcpOutBuffer _buffer;

	/**
	 *  Thread in which the DNS lookup occurs
	 *  @var std::thread
	 */
	std::thread _thread;

	/**
	 * epoll file descriptor to monitor socket connection
	 * \var int
	 */
	int         _efd;

	/**
	 * pipe to interrupt while connecting
	 * \var Pipe
	 */
	Pipe        _signal_pipe;

	/**
	 *  Run the thread
	 */
	void run() {
		// prevent exceptions
		try {
			// check if we support openssl in the first place
			if (_secure && !OpenSSL::valid()) {
				throw std::runtime_error("Secure connection cannot be established: libssl.so cannot be loaded");
			}

			struct epoll_event signal_evt = {};

			signal_evt.data.fd = _signal_pipe.in();
			signal_evt.events  = EPOLLIN;

			auto rc = epoll_ctl(_efd, EPOLL_CTL_ADD, _signal_pipe.in(), &signal_evt);
			if (rc == -1) {
				throw std::runtime_error("cannot register shutdown signal");
			}

			// get address info
			AddressInfo addresses(_hostname.data(), _port);

			bool interrupted = false;

			// iterate over the addresses
			for (size_t i = 0; i < addresses.size(); ++i) {
				// create the socket
				_socket = socket(addresses[i]->ai_family, addresses[i]->ai_socktype, addresses[i]->ai_protocol);
				// move on on failure
				if (_socket < 0) {
					continue;
				}

				// turn socket into a non-blocking socket and set the close-on-exec bit
				rc = fcntl(_socket, F_GETFL, 0);
				if (rc == -1) {
					::close(_socket);
					_socket = -1;
					continue;
				}

				rc = fcntl(_socket, F_SETFL, (unsigned int) rc | O_NONBLOCK | O_CLOEXEC);
				if (rc == -1) {
					::close(_socket);
					_socket = -1;
					continue;
				}

				struct epoll_event conn_evt = {};

				conn_evt.data.fd = _socket;
				conn_evt.events  = EPOLLOUT | EPOLLIN | EPOLLERR;

				// connect to the socket
				rc = connect(_socket, addresses[i]->ai_addr, addresses[i]->ai_addrlen);
				if (rc == 0) {
					break;
				} else if (errno == EINPROGRESS) {
					rc = epoll_ctl(_efd, EPOLL_CTL_ADD, _socket, &conn_evt);
					if (rc == 0) {
						struct epoll_event processable_evt;

						int num_evts = epoll_wait(_efd, &processable_evt, 1, -1);
						epoll_ctl(_efd, EPOLL_CTL_DEL, _socket, nullptr);
						if (num_evts > 0) {
							// check for shutdown signal
							if (processable_evt.data.fd == _signal_pipe.in()) {
								interrupted = true;
								char ch;
								read(_signal_pipe.in(), &ch, 1);
								errno = ECONNABORTED;
								break;
							} else {
								int retVal = -1;
								socklen_t retValLen = sizeof(retVal);

								rc = getsockopt(_socket, SOL_SOCKET, SO_ERROR, &retVal, &retValLen);

								if (rc < 0 || retVal != 0) {
									// ERROR: connect did not "go through"
									errno = ENOTCONN;
								} else {
									// connection went OK
									break;
								}
							}
						}
					}
				}

				// log the error for the time being
				_error = strerror(errno);

				// close socket because connect failed

				::close(_socket);

				// socket no longer is valid
				_socket = -1;
			}

			epoll_ctl(_efd, EPOLL_CTL_DEL, _signal_pipe.in(), nullptr);

			// connection succeeded, mark socket as non-blocking
			if (!interrupted && _socket >= 0) {
				// we want to enable "no delay" on sockets (otherwise all send operations are s-l-o-w
				int optval = 1;

				// set the option
				setsockopt(_socket, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(int));

#ifdef AMQP_CPP_USE_SO_NOSIGPIPE
				set_sockopt_nosigpipe(_socket);
#endif
				// notify the master thread by sending a byte over the pipe
				if (!_pipe.notify()) {
					_error = strerror(errno);
				}
			}
		} catch (const std::runtime_error &error) {
			// address could not be resolved, we ignore this for now, but store the error
			_error = error.what();
		}
	}

public:
	/**
	 *  Constructor
	 *  @param  parent      Parent connection object
	 *  @param  hostname    The hostname for the lookup
	 *  @param  port        The port number for the lookup
	 *  @param  secure      Do we need a secure tls connection when ready?
	 */
	TcpResolver(TcpParent *parent, std::string hostname, uint16_t port, bool secure)
		: TcpExtState(parent)
		, _hostname(std::move(hostname))
		, _secure(secure)
		, _port(port)
	{
		_efd = epoll_create(1);
		if (_efd == -1) {
			throw std::runtime_error("cannot create epoll for dns resolver");
		}

		// make read-end non-blocking
		int flags = fcntl(_signal_pipe.in(), F_GETFL, 0);
		if (flags == - 1) {
			::close(_efd);
			throw std::runtime_error("cannot get pipe flags");
		}

		flags = fcntl(_signal_pipe.in(), F_SETFL, (unsigned int)flags | O_NONBLOCK);
		if (flags == - 1) {
			::close(_efd);
			throw std::runtime_error("cannot set pipe flags");
		}

		// tell the event loop to monitor the filedescriptor of the pipe
		parent->onIdle(this, _pipe.in(), readable);

		// we can now start the thread (must be started after filedescriptor is monitored!)
		std::thread thread(std::bind(&TcpResolver::run, this));

		// store thread in member
		_thread.swap(thread);
	}

	/**
	 *  Destructor
	 */
	virtual ~TcpResolver() noexcept {
		_signal_pipe.notify();

		// wait for the thread to be ready
		_thread.join();

		// stop monitoring the pipe filedescriptor
		_parent->onIdle(this, _pipe.in(), 0);

		::close(_efd);
	}

	/**
	 *  Number of bytes in the outgoing buffer
	 *  @return std::size_t
	 */
	 [[nodiscard]]
	std::size_t queued() const override { return _buffer.size(); }

	/**
	 *  Proceed to the next state
	 *  @return TcpState *
	 */
	TcpState *proceed(const Monitor &monitor) {
		// prevent exceptions
		try {
			// socket should be connected by now
			if (_socket < 0) {
				throw std::runtime_error(_error.data());
			}

			// report that the network-layer is connected
			_parent->onConnected(this);

			// handler callback might have destroyed connection
			if (!monitor.valid()) {
				return nullptr;
			}

			// if we need a secure connection, we move to the tls handshake (this could throw)
			if (_secure) {
				return new SslHandshake(this, std::move(_hostname), std::move(_buffer));
			} else { // otherwise we have a valid regular tcp connection
				return new TcpConnected(this, std::move(_buffer));
			}
		} catch (const std::runtime_error &error) {
			// report error
			_parent->onError(this, error.what(), false);

			// handler callback might have destroyed connection
			if (!monitor.valid()) {
				return nullptr;
			}

			// create dummy implementation
			return new TcpClosed(this);
		}
	}

	/**
	 *  Wait for the resolver to be ready
	 *  @param  monitor     Object to check if connection still exists
	 *  @param  fd          The filedescriptor that is active
	 *  @param  flags       Flags to indicate that fd is readable and/or writable
	 *  @return             New implementation object
	 */
	TcpState *process(const Monitor &monitor, int fd, int flags) override {
		// only works if the incoming pipe is readable
		if (fd != _pipe.in() || !(flags & readable)) {
			return this;
		}

		// proceed to the next state
		return proceed(monitor);
	}

	/**
	 *  Send data over the connection
	 *  @param  buffer      buffer to send
	 *  @param  size        size of the buffer
	 */
	void send(const char *buffer, size_t size) override {
		// add data to buffer
		_buffer.add(buffer, size);
	}
};

/**
 *  End of namespace
 */
}
