#include <err.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <libgdb/parser2.h>
#include <libgdb/response.h>

#include <systemc>

#include "gdb_server.h"

/* TODO: move this header to common? */
#include <platform/hifive/async_event.h>

extern std::map<std::string, GDBServer::packet_handler> handlers;

GDBServer::GDBServer(sc_core::sc_module_name name,
                     std::vector<debug_target*> dharts,
                     DebugMemoryInterface *mm,
                     uint16_t port) {
	if (dharts.size() <= 0)
		throw std::invalid_argument("no harts specified");

	for (debug_target *h : dharts) {
		events[h] = std::make_tuple(new sc_core::sc_event, (sc_core::sc_event *)NULL);

		/* Don't block on WFI, otherwise the run_threads method
		 * does not work correctly. Allowing blocking WFI would
		 * likely increase the performance */
		h->block_on_wfi(false);
	}

	memory = mm;
	arch = dharts.at(0)->get_architecture(); // assuming all harts use the same
	harts = dharts;
	prevpkt = NULL;
	create_sock(port);

	SC_THREAD(run);

	thr = std::thread(&GDBServer::serve, this);
	thr.detach();
}

sc_core::sc_event *GDBServer::get_stop_event(debug_target *hart) {
	return std::get<0>(events.at(hart));
}

void GDBServer::set_run_event(debug_target *hart, sc_core::sc_event *event) {
	std::get<1>(events.at(hart)) = event;
}

void GDBServer::create_sock(uint16_t port) {
	struct sockaddr_in6 addr;
	int reuse, ip6only;

	sockfd = socket(AF_INET6, SOCK_STREAM, 0);
	if (sockfd == -1)
		throw std::system_error(errno, std::generic_category());

	ip6only = 0;
	if (setsockopt(sockfd, IPPROTO_IPV6, IPV6_V6ONLY, &ip6only, sizeof(ip6only)) == -1)
		goto err;

	reuse = 1;
	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) == -1)
		goto err;

	memset(&addr, 0, sizeof(addr));
	addr.sin6_family = AF_INET6;
	addr.sin6_addr = in6addr_loopback;
	addr.sin6_port = htons(port);

	if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
		goto err;
	if (listen(sockfd, 0) == -1)
		goto err;

	return;
err:
	close(sockfd);
	throw std::system_error(errno, std::generic_category());
}

std::vector<debug_target *> GDBServer::get_threads(int id) {
	if (id == GDB_THREAD_ANY)
		id = 1; /* pick the first thread */

	if (id == GDB_THREAD_ALL)
		return harts;

	if (id < 1) /* thread ids should start at index 1 */
		throw std::out_of_range("Thread id is too small");
	else
		id--;

	std::vector<debug_target *> v;
	v.push_back(harts.at(id));
	return v;
}

void GDBServer::exec_thread(thread_func fn, char op) {
	int thread;
	std::vector<debug_target *> threads;

	try {
		thread = thread_ops.at(op);
	} catch (const std::out_of_range&) {
		thread = GDB_THREAD_ANY;
	}

	threads = get_threads(thread);
	for (debug_target *thread : threads)
		fn(thread);
}

std::vector<debug_target *> GDBServer::run_threads(int id, bool single) {
	this->single_run = single;
	std::vector<debug_target *> hartsrun = get_threads(id);

	/* invoke all selected harts */
	sc_core::sc_event_or_list allharts;
	for (debug_target *hart : hartsrun) {
		sc_core::sc_event *stop_event, *run_event;
		std::tie (stop_event, run_event) = this->events.at(hart);

		run_event->notify();
		allharts |= *stop_event;
	}

	/* wait until the first hart finishes execution */
	sc_core::wait(allharts);

	/* ensure that all running harts are stopped */
	std::vector<CoreExecStatus> orig_status;
	for (debug_target *hart : hartsrun) {
		CoreExecStatus status = hart->get_status();
		orig_status.push_back(status);

		if (status != CoreExecStatus::Runnable)
			continue;
		hart->set_status(CoreExecStatus::HitBreakpoint);

		sc_core::sc_event &stopev = *get_stop_event(hart);
		sc_core::wait(stopev);
	}

	/* restore original hart status */
	assert(orig_status.size() == hartsrun.size());
	for (size_t i = 0; i < hartsrun.size(); i++)
		hartsrun[i]->set_status(orig_status[i]);

	return hartsrun;
}

void GDBServer::writeall(int fd, char *data, size_t len) {
	ssize_t ret, w;

	w = 0;
	do {
		assert(len >= (size_t)w);
		ret = write(fd, &data[w], len - (size_t)w);
		if (ret < 0)
			throw std::system_error(errno, std::generic_category());

		w += ret;
	} while ((size_t)w < len);
}

void GDBServer::send_packet(int conn, const char *data, gdb_kind_t kind) {
	char *serialized;

	serialized = gdb_serialize(kind, data);

	try {
		writeall(conn, serialized, strlen(serialized));
	} catch (const std::system_error& e) {
		warnx("writeall failed: %s", e.what());
		goto ret;
	}

	/* acknowledgment are only used for GDB packets */
	if (kind != GDB_KIND_PACKET)
		goto ret;

	free(prevpkt);
	if (!(prevpkt = strdup(serialized))) {
		prevpkt = NULL;
		free(serialized);
		throw std::system_error(errno, std::generic_category());
	}

ret:
	free(serialized);
}

void GDBServer::retransmit(int conn) {
	if (!prevpkt)
		return;

	try {
		writeall(conn, prevpkt, strlen(prevpkt));
	} catch (const std::system_error& e) {
		warnx("writeall failed: %s", e.what());
	}

	/* memory for prevpkt is freed on next successfull
	 * packet transmit in the send_packet function */
}

void GDBServer::dispatch(int conn) {
	FILE *stream;
	gdb_packet_t *pkt;

	if (!(stream = fdopen(conn, "r")))
		throw std::system_error(errno, std::generic_category());

	for (;;) {
		mtx.lock();
		if (!(pkt = gdb_parse_pkt(stream))) {
			mtx.unlock();
			break;
		}

		pktq.push(std::make_tuple(conn, pkt));
		asyncEvent.notify();

		/* further processing is performed in the SystemC run()
		 * thread which interacts with the ISS objects and
		 * unlocks the mutex after finishing all operations. */
	}

	fclose(stream);
}

void GDBServer::serve(void) {
	int conn;

	for (;;) {
		if ((conn = accept(sockfd, NULL, NULL)) == -1) {
			warn("accept failed");
			continue;
		}

		dispatch(conn);
		close(conn);

		free(prevpkt);
		prevpkt = NULL;
	}
}

void GDBServer::run(void) {
	int conn;
	gdb_command_t *cmd;
	gdb_packet_t *pkt;
	packet_handler handler;

	for (;;) {
		sc_core::wait(asyncEvent);

		auto ctx = pktq.front();
		std::tie (conn, pkt) = ctx;

		switch (pkt->kind) {
		case GDB_KIND_NACK:
			retransmit(conn);
			/* fall through */
		case GDB_KIND_ACK:
			goto next1;
		default:
			break;
		}

		if (!(cmd = gdb_parse_cmd(pkt))) {
			send_packet(conn, NULL, GDB_KIND_NACK);
			goto next1;
		}

		send_packet(conn, NULL, GDB_KIND_ACK);
		try {
			handler = handlers.at(cmd->name);
		} catch (const std::out_of_range&) {
			// For any command not supported by the stub, an
			// empty response (‘$#00’) should be returned.
			send_packet(conn, "");
			goto next2;
		}

		(this->*handler)(conn, cmd);

next2:
		gdb_free_cmd(cmd);
next1:
		gdb_free_packet(pkt);
		pktq.pop();
		mtx.unlock();
	}
}
