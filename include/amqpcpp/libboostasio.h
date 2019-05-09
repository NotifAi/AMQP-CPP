/**
 *  LibBoostAsio.h
 *
 *  Implementation for the AMQP::TcpHandler for boost::asio. You can use this class 
 *  instead of a AMQP::TcpHandler class, just pass the boost asio service to the 
 *  constructor and you're all set.  See tests/libboostasio.cpp for example.
 *
 *  Watch out: this class was not implemented or reviewed by the original author of 
 *  AMQP-CPP. However, we do get a lot of questions and issues from users of this class,
 *  so we cannot guarantee its quality. If you run into such issues too, it might be
 *  better to implement your own handler that interact with boost.
 *
 *
 *  @author Gavin Smith <gavin.smith@coralbay.tv>
 */


/**
 *  Include guard
 */
#pragma once

/**
 *  Dependencies
 */
#include <memory>
#include <mutex>

#include <boost/asio/io_service.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/deadline_timer.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/bind.hpp>
#include <boost/function.hpp>
#include <boost/noncopyable.hpp>

#include "amqpcpp/linux_tcp.h"

// C++17 has 'weak_from_this()' support.
#if __cplusplus >= 201701L
#   define PTR_FROM_THIS(T) weak_from_this()
#else
#   define PTR_FROM_THIS(T) std::weak_ptr<T>(shared_from_this())
#endif

/**
 *  Set up namespace
 */
namespace AMQP {

/**
 *  Class definition
 *  @note Because of a limitation on Windows, this will only work on POSIX based systems - see https://github.com/chriskohlhoff/asio/issues/70
 */
class LibBoostAsioHandler : public virtual TcpHandler, private boost::noncopyable {
protected:
	using strand_sp = std::shared_ptr<boost::asio::io_service::strand>;

	/**
	 *  Helper class that wraps a boost io_service socket monitor.
	 */
	class Watcher : public virtual std::enable_shared_from_this<Watcher>, private boost::noncopyable {
	private:
		using strand_wp = std::weak_ptr<boost::asio::io_service::strand>;
		using handler_cb = std::function<void(boost::system::error_code, std::size_t)>;

	public:
		/**
		 *  Constructor- initialises the watcher and assigns the filedescriptor to
		 *  a boost socket for monitoring.
		 *  @param  io_service      The boost io_service
		 *  @param  wpstrand        A weak pointer to a io_service::strand instance.
		 *  @param  fd              The filedescriptor being watched
		 */
		Watcher(boost::asio::io_service &io_service, strand_wp strand, int fd);

		/**
		 *  Destructor
		 */
		~Watcher() {
			_read = false;
			_write = false;
			_socket.release();
		}

	public:
		/**
		 *  Change the events for which the filedescriptor is monitored
		 *  @param  events
		 */
		void events(TcpConnection *connection, int fd, int events);

	private:
		/**
		 * Binds and returns a read handler for the io operation.
		 * @param  connection   The connection being watched.
		 * @param  fd           The file descriptor being watched.
		 * @return handler callback
		 */
		handler_cb get_read_handler(TcpConnection *const connection, const int fd);

		/**
		 * Binds and returns a read handler for the io operation.
		 * @param  connection   The connection being watched.
		 * @param  fd           The file descripter being watched.
		 * @return handler callback
		 */
		handler_cb get_write_handler(TcpConnection *const connection, const int fd);

		/**
		 *  Handler method that is called by boost's io_service when the socket pumps a read event.
		 *  @param  ec          The status of the callback.
		 *  @param  bytes_transferred The number of bytes transferred.
		 *  @param  awpWatcher  A weak pointer to this object.
		 *  @param  connection  The connection being watched.
		 *  @param  fd          The file descriptor being watched.
		 *  @note   The handler will get called if a read is cancelled.
		 */
		void read_handler(const boost::system::error_code &ec, std::size_t bytes_transferred
		                  , std::weak_ptr<Watcher> awpWatcher, TcpConnection *const connection
		                  , int fd);

		/**
		 *  Handler method that is called by boost's io_service when the socket pumps a write event.
		 *  @param  ec          The status of the callback.
		 *  @param  bytes_transferred The number of bytes transferred.
		 *  @param  awpWatcher  A weak pointer to this object.
		 *  @param  connection  The connection being watched.
		 *  @param  fd          The file descriptor being watched.
		 *  @note   The handler will get called if a write is cancelled.
		 */
		void write_handler(const boost::system::error_code &ec, std::size_t bytes_transferred
		                   , std::weak_ptr<Watcher> awpWatcher, TcpConnection *const connection
		                   , int fd);

	private:
		boost::asio::io_service &_io;
		strand_wp _wpstrand;
		/**
		 *  The boost tcp socket.
		 *  @var class boost::asio::ip::tcp::socket
		 *  @note https://stackoverflow.com/questions/38906711/destroying-boost-asio-socket-without-closing-native-handler
		 */
		boost::asio::posix::stream_descriptor _socket;
		bool _read;
		std::atomic<bool> _read_pending;
		bool _write;
		std::atomic<bool> _write_pending;
	};

	/**
	 *  Timer class to periodically fire a heartbeat
	 */
	class Timer : public std::enable_shared_from_this<Timer>, private boost::noncopyable {
	private:
		using handler_fn = std::function<void(const boost::system::error_code &)>;
		using strand_weak_ptr = std::weak_ptr<boost::asio::io_service::strand>;

	public:
		/**
		 *  Constructor
		 *  @param  io_service The boost asio io_service.
		 *  @param  wpstrand   A weak pointer to a io_service::strand instance.
		 */
		Timer(boost::asio::io_service &io_service, strand_weak_ptr wpstrand);

		/**
		 *  Destructor
		 */
		~Timer() {
			// stop the timer
			stop();
		}

	public:
		/**
		 *  Change the expire time
		 *  @param  connection
		 *  @param  timeout
		 */
		void set(TcpConnection *connection, uint16_t timeout);

	private:
		/**
		 * Binds and returns a lambda function handler for the io operation.
		 * @param  connection   The connection being watched.
		 * @param  timeout      The file descriptor being watched.
		 * @return handler callback
		 */
		handler_fn get_handler(TcpConnection *connection, uint16_t timeout);

		/**
		 *  Callback method that is called by libev when the timer expires
		 *  @param  ec          error code returned from loop
		 *  @param  loop        The loop in which the event was triggered
		 *  @param  connection
		 *  @param  timeout
		 */
		void timeout(const boost::system::error_code &ec, std::weak_ptr<Timer> awpThis, TcpConnection *connection
		             , uint16_t timeout);

		/**
		 *  Stop the timer
		 */
		void stop();

	private:
		boost::asio::io_service &_ioservice;
		strand_weak_ptr _wpstrand;
		boost::asio::steady_timer _timer;
	};

public:
	/**
	 *  Constructor
	 *  @param  io_service    The boost io_service to wrap
	 */
	explicit LibBoostAsioHandler(boost::asio::io_service &io);

	/**
	 *  Destructor
	 */
	~LibBoostAsioHandler() override = default;

public:
	/**
	 *  Returns a reference to the boost io_service object that is being used.
	 *  @return The boost io_service object.
	 */
	boost::asio::io_service &service();

	/**
	 * Make sure to stop the heartbeat timer after the connection is closed.
	 * Otherwise it will keep the service running forever.
	 */
	void onClosed(TcpConnection *connection) override;

protected:
	/**
	 *  Method that is called when the heartbeat frequency is negotiated between the server and the client.
	 *  @param  connection      The connection that suggested a heartbeat interval
	 *  @param  interval        The suggested interval from the server
	 *  @return uint16_t        The interval to use
	 */
	uint16_t onNegotiate(TcpConnection *connection, uint16_t interval) override;

private:
	/**
	 *  Method that is called by AMQP-CPP to register a filedescriptor for readability or writability
	 *  @param  connection  The TCP connection object that is reporting
	 *  @param  fd          The filedescriptor to be monitored
	 *  @param  flags       Should the object be monitored for readability or writability?
	 */
	void monitor(TcpConnection *connection, int fd, int flags) override;

private:
	/**
	 *  The boost asio io_service.
	 *  @var class boost::asio::io_service&
	 */
	boost::asio::io_service &_ioservice;
	strand_sp _strand;
	std::mutex _lock;
	std::map<int, std::shared_ptr<Watcher>> _watchers;
	std::shared_ptr<Timer> _timer;
};


/**
 *  End of namespace
 */
}
