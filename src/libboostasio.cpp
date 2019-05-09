//
// Created by Artur Troian on 2019-05-08
//

#include <amqpcpp.h>
#include <amqpcpp/libboostasio.h>

namespace AMQP {

LibBoostAsioHandler::Watcher::Watcher(boost::asio::io_service &io_service, strand_wp strand, int fd)
	: _io(io_service)
	, _wpstrand(strand)
	, _socket(_io)
	, _read(false)
	, _read_pending(false)
	, _write(false)
	, _write_pending(false) {
	_socket.assign(fd);
	_socket.non_blocking(true);
}

LibBoostAsioHandler::Watcher::handler_cb
LibBoostAsioHandler::Watcher::get_read_handler(TcpConnection *const connection, int fd) {
	const std::weak_ptr<Watcher> awpWatcher = PTR_FROM_THIS(Watcher);

	return [connection, fd, awpWatcher](const boost::system::error_code &ec, std::size_t bytes_transferred) {
		auto aWatcher = awpWatcher.lock();
		if (!aWatcher) {
			return;
		}

		auto strand = aWatcher->_wpstrand.lock();
		if (!strand) {
			aWatcher->read_handler(boost::asio::error::make_error_code(boost::asio::error::operation_aborted), 0
			                       , awpWatcher, connection, fd);
			return;
		}

		strand->dispatch([ec, bytes_transferred, awpWatcher, connection, fd]() {
			auto awtch = awpWatcher.lock();
			if (awtch) {
				awtch->read_handler(ec, bytes_transferred, awpWatcher, connection, fd);
			}
		});
	};
}

LibBoostAsioHandler::Watcher::handler_cb
LibBoostAsioHandler::Watcher::get_write_handler(TcpConnection *const connection, int fd) {
	auto awpWatcher = PTR_FROM_THIS(Watcher);

	return [connection, fd, awpWatcher](const boost::system::error_code &ec, const std::size_t bytes_transferred) {
		auto aWatcher = awpWatcher.lock();
		if (!aWatcher) {
			return;
		}

		auto strand = aWatcher->_wpstrand.lock();
		if (!strand) {
			aWatcher->write_handler(boost::asio::error::make_error_code(boost::asio::error::operation_aborted), 0
			                        , awpWatcher, connection, fd);
			return;
		}

		strand->dispatch([ec, bytes_transferred, awpWatcher, connection, fd]() {
			auto awtch = awpWatcher.lock();
			if (awtch) {
				awtch->write_handler(ec, bytes_transferred, awpWatcher, connection, fd);
			}
		});
	};
}

void LibBoostAsioHandler::Watcher::read_handler(
	const boost::system::error_code &ec
	, std::size_t bytes_transferred
	, std::weak_ptr<Watcher> awpWatcher
	, TcpConnection *const connection
	, int fd) {
	// Resolve any potential problems with dangling pointers
	// (remember we are using async).
	const std::shared_ptr<Watcher> apWatcher = awpWatcher.lock();
	if (!apWatcher) {
		return;
	}

	_read_pending = false;

	if (_read && (!ec || ec == boost::asio::error::would_block)) {
		connection->process(fd, AMQP::readable);

		bool expected = false;
		if (_read_pending.compare_exchange_strong(expected, true)) {
			_socket.async_read_some(boost::asio::null_buffers(), get_read_handler(connection, fd));
		}
	}
}

void LibBoostAsioHandler::Watcher::write_handler(const boost::system::error_code &ec, std::size_t bytes_transferred
                                                 , std::weak_ptr<Watcher> awpWatcher
                                                 , TcpConnection *const connection
                                                 , int fd) {
	// Resolve any potential problems with dangling pointers
	// (remember we are using async).
	const std::shared_ptr<Watcher> apWatcher = awpWatcher.lock();
	if (!apWatcher) {
		return;
	}

	_write_pending = false;

	if (_write && (!ec || ec == boost::asio::error::would_block)) {
		connection->process(fd, AMQP::writable);

		bool expected = false;
		if (_write_pending.compare_exchange_strong(expected, true)) {
			_socket.async_write_some(boost::asio::null_buffers(), get_write_handler(connection, fd));
		}
	}
}

void LibBoostAsioHandler::Watcher::events(TcpConnection *connection, int fd, int events) {
	// 1. Handle reads?
	_read = ((events & AMQP::readable) != 0);

	// Read requested but no read pending?
	bool expected = false;

	if (_read) {
		if (_read_pending.compare_exchange_strong(expected, true)) {
			_socket.async_read_some(boost::asio::null_buffers(), get_read_handler(connection, fd));
		}
	}

	// 2. Handle writes?
	_write = ((events & AMQP::writable) != 0);

	// Write requested but no write pending?
	expected = false;
	if (_write) {
		if (_write_pending.compare_exchange_strong(expected, true)) {
			_socket.async_write_some(boost::asio::null_buffers(), get_write_handler(connection, fd));
		}
	}
}

LibBoostAsioHandler::Timer::Timer(boost::asio::io_service &io_service, strand_weak_ptr wpstrand)
	: _ioservice(io_service)
	, _wpstrand(wpstrand)
	, _timer(_ioservice) {}

void LibBoostAsioHandler::Timer::set(TcpConnection *connection, uint16_t timeout) {
	// stop timer in case it was already set
	stop();

	// Reschedule the timer for the future:
	_timer.expires_from_now(std::chrono::seconds(timeout));

	// Posts the timer event
	_timer.async_wait(get_handler(connection, timeout));
}

LibBoostAsioHandler::Timer::handler_fn
LibBoostAsioHandler::Timer::get_handler(TcpConnection *const connection, uint16_t timeout) {
	const std::weak_ptr<Timer> awpTimer = PTR_FROM_THIS(Timer);

	return [connection, timeout, awpTimer](const boost::system::error_code &ec) {
		const std::shared_ptr<Timer> aTimer = awpTimer.lock();
		if (!aTimer) {
			return;
		}

//		auto strand = aTimer->_wpstrand.lock();
//		if (!strand) {
		aTimer->timeout(
			boost::asio::error::make_error_code(boost::asio::error::operation_aborted), awpTimer
			, connection, timeout
		);
//			return;
//		}
//		strand->dispatch([&ec, awpTimer, connection, timeout]() {
//			const std::shared_ptr<Timer> atimer = awpTimer.lock();
//			if (atimer) {
//				atimer->timeout(ec, awpTimer, connection, timeout);
//			}
//		});
	};
}

void LibBoostAsioHandler::Timer::timeout(
	const boost::system::error_code &ec
	, std::weak_ptr<Timer> awpThis
	, TcpConnection *const connection
	, uint16_t timeout) {
	// Resolve any potential problems with dangling pointers
	// (remember we are using async).
	const std::shared_ptr<Timer> apTimer = awpThis.lock();
	if (!apTimer) {
		return;
	}

	if (!ec) {
		if (connection) {
			// send the heartbeat
			connection->heartbeat();
		}

		// Reschedule the timer for the future:
		_timer.expires_at(_timer.expires_at() + std::chrono::seconds(timeout));

		// Posts the timer event
		_timer.async_wait(get_handler(connection, timeout));
	}
}

void LibBoostAsioHandler::Timer::stop() {
	// do nothing if it was never set
	_timer.cancel();
}

LibBoostAsioHandler::LibBoostAsioHandler(boost::asio::io_service &io)
	: _ioservice(io)
	, _strand(std::make_shared<boost::asio::io_service::strand>(io))
	, _timer(std::make_shared<Timer>(_ioservice, _strand)) {}

boost::asio::io_service &LibBoostAsioHandler::service() {
	return _ioservice;
}

void LibBoostAsioHandler::onClosed(TcpConnection *connection) {
	(void)connection;
	_timer.reset();
}

void LibBoostAsioHandler::monitor(TcpConnection *const connection, int fd, int flags) {
	std::lock_guard<std::mutex> lk(_lock);

	// do we already have this file descriptor
	auto iter = _watchers.find(fd);

	// was it found?
	if (iter == _watchers.end()) {
		// we did not yet have this watcher - but that is ok if no file descriptor was registered
		if (flags == 0) {
			return;
		}

		auto apWatcher = std::make_shared<Watcher>(_ioservice, _strand, fd);

		_watchers[fd] = apWatcher;

		// explicitly set the events to monitor
		apWatcher->events(connection, fd, flags);
	} else if (flags == 0) {
		// the watcher does already exist, but we no longer have to watch this watcher
		_watchers.erase(iter);
	} else {
		// Change the events on which to act.
		iter->second->events(connection, fd, flags);
	}
}

uint16_t LibBoostAsioHandler::onNegotiate(TcpConnection *connection, uint16_t interval) {
	// skip if no heartbeats are needed
	if (interval == 0) {
		return 0;
	}

	// set the timer
	_timer->set(connection, interval);

	// we agree with the interval
	return interval;
}

} // namespace AMQP
