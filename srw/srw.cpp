/*
 * 2011+ Copyright (c) Evgeniy Polyakov <zbr@ioremap.net>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 */

#ifdef HAVE_COCAINE_SUPPORT

#include <map>
#include <vector>
#include <sstream>
#include <functional>
#include <msgpack.hpp>

#include <boost/thread/tss.hpp>

#include <elliptics/interface.h>
#include <elliptics/utils.hpp>

#include "elliptics.h"

#include <cocaine/context.hpp>
#include <cocaine/api/stream.hpp>
#include <cocaine/rpc/actor.hpp>
#include <cocaine/service/node.hpp>
#include <cocaine/service/node/overseer.hpp>

#include <blackhole/v1/attribute.hpp>
#include <blackhole/v1/logger.hpp>
#include <blackhole/v1/record.hpp>
#include <blackhole/v1/formatter.hpp>
#include <blackhole/v1/formatter/string.hpp>
#include <blackhole/scope/watcher.hpp>
#include <blackhole/scope/manager.hpp>
#include <blackhole/extensions/writer.hpp>

#include "localnode.hpp"
#include "cocaine/traits/localnode.hpp"

#include "../bindings/cpp/exec_context_data_p.hpp"
#include "elliptics/srw.h"

#define SRW_LOG(__log__, __level__, __app__, ...) \
	BH_LOG((__log__), (__level__), __VA_ARGS__) \
		("app", (__app__)) \
		("source", "srw")


namespace ioremap { namespace elliptics {

inline
blackhole::v1::severity_t convert_severity(dnet_log_level level)
{
	switch (level) {
		case DNET_LOG_DEBUG:
			return cocaine::logging::debug;
		case DNET_LOG_NOTICE:
		case DNET_LOG_INFO:
			return cocaine::logging::info;
		case DNET_LOG_WARNING:
			return cocaine::logging::warning;
		case DNET_LOG_ERROR:
		default:
			return cocaine::logging::error;
	};
}

inline
dnet_log_level convert_severity(blackhole::v1::severity_t severity)
{
	switch (severity) {
		case cocaine::logging::debug:
			return DNET_LOG_DEBUG;
		case cocaine::logging::info:
			return DNET_LOG_INFO;
		case cocaine::logging::warning:
			return DNET_LOG_WARNING;
		case cocaine::logging::error:
		default:
			return DNET_LOG_ERROR;
	}
}

/// Elliptics to cocaine logger adapter.
///
/// Current elliptics logger is actually a wrapper over on blackhole v0.2 logger,
/// Current cocaine logger is generalized logger interface from blackhole v1.0 (which was rewritten from scratch).
///
/// This logger_adapter wraps v0.2 logger into v1.0 logger interface.
///

/// Scope manager.
//
//XXX: pristine copy from blackhole/root.cpp,
// We're forced to use it because blackhole requires manager_t to remember
// sequence of watcher_t object, dummy implementation without memory
// will break assertion in ~watcher_t()
//
class thread_manager_t : public blackhole::v1::scope::manager_t {
	boost::thread_specific_ptr<blackhole::v1::scope::watcher_t> inner;

public:
	thread_manager_t() : inner([](blackhole::v1::scope::watcher_t*) {}) {}

	auto get() const -> blackhole::v1::scope::watcher_t* {
		return inner.get();
	}

	auto reset(blackhole::v1::scope::watcher_t* value) -> void {
		inner.reset(value);
	}
};

/// Logger interface implementation.
///
class logger_adapter : public cocaine::logging::logger_t
{
private:
	dnet_logger *elliptics_logger;
	thread_manager_t scope_manager;

public:
	logger_adapter(dnet_node *n) : elliptics_logger(dnet_node_get_logger(n)) {}

	// logging::logger_t interface

	virtual ~logger_adapter() = default;

	/// Logs the given message with the specified severity level.
	virtual auto log(blackhole::v1::severity_t severity, const blackhole::v1::message_t& message) -> void
	{
		blackhole::v1::attribute_pack pack;
		log(severity, message, pack);
	}

	/// Logs the given message with the specified severity level and attributes pack attached.
	virtual auto log(blackhole::v1::severity_t severity, const blackhole::v1::message_t& message, blackhole::v1::attribute_pack& pack) -> void
	{
		if (scope_manager.get()) {
			scope_manager.get()->collect(pack);
		}
		blackhole::v1::writer_t writer;
		blackhole::v1::record_t record(severity, message, pack);
		blackhole::v1::formatter::string_t formatter("{message}, attrs: [{...}]");
		formatter.format(record, writer);
		dnet_log_only_log(elliptics_logger, convert_severity(severity), "%s", writer.result().to_string().c_str());
	}

	/// Logs a message which is only to be constructed if the result record passes filtering with
	/// the specified severity and including the attributes pack provided.
	virtual auto log(blackhole::v1::severity_t severity, const blackhole::v1::lazy_message_t& message, blackhole::v1::attribute_pack& pack) -> void
	{
		//TODO: properly support message laziness
		log(severity, message.supplier(), pack);
	}

	/// Returns a scoped attributes manager reference.
	///
	/// Returned manager allows the external tools to attach scoped attributes to the current logger
	/// instance, making every further log event to contain them until the registered scoped guard
	/// keeped alive.
	///
	/// \returns a scoped attributes manager.
	virtual auto manager() -> blackhole::v1::scope::manager_t&
	{
		return scope_manager;
	}
};

//
// `client_session` represents open communication with the srw's client
//  over elliptics channel.
//
struct client_session
{
	dnet_net_state *state_;
	const dnet_cmd cmd_copy_;
	exec_context exec_copy_;
	const std::string app_;
	const std::string signature_;

	client_session(dnet_net_state *state, dnet_cmd *cmd, const std::string &app, const std::string &signature, const exec_context &exec_copy)
		: state_(state)
		, cmd_copy_(*cmd)
		, exec_copy_(exec_copy)
		, app_(app)
		, signature_(signature)
	{
		dnet_state_get(state_);
		SRW_LOG(*dnet_node_get_logger(state_->n), DNET_LOG_DEBUG, app_, "%s: client session open", signature_);

		// set DNET_SPH_FLAGS_REPLY flag, drop all others
		exec_copy_.set_flags(DNET_SPH_FLAGS_REPLY);
	}

	~client_session() {
		SRW_LOG(*dnet_node_get_logger(state_->n), DNET_LOG_DEBUG, app_, "%s: client session close", signature_);
		dnet_state_put(state_);
	}

	void send_chunk(const argument_data &data) {
		// chunks of size 0 are just redundant, its safe to ignore them;
		// also as ack is a reply with zero payload too, its better not
		// to create a mess
		if (data.size() == 0) {
			return;
		}

		SRW_LOG(*dnet_node_get_logger(state_->n), DNET_LOG_DEBUG, app_, "%s: client session sends data", signature_);

		//FIXME: `data` payload gets copied here 2 times more than necessary:
		//  1. in exec_context_data::copy, to prefix it with sph
		//  2. in dnet_send, to prefix [sph,data] with dnet_cmd
		// Those could be eliminated, for this case specifically.
		// (Third copy of entire [cmd,sph,data] is performed in dnet_io_req_queue,
		// and thats too deep, -- fixing that would mean changing a lot of elliptics
		// entrails.)
		auto reply = exec_context_data::copy(exec_copy_, exec_copy_.event(), data);
		auto srw_packet = reply.native_data();
		dnet_send_reply(state_, const_cast<dnet_cmd*>(&cmd_copy_), srw_packet.data(), srw_packet.size(), 1);
	}

	void finish(int error_code = 0) {
		// exec commands have NEED_ACK flag set unconditionally
		// (same as all other nonsystem commands).
		// If cmd_copy_ will have it unset then it will mean that client logic
		// was changed incompatibly with server logic.

		SRW_LOG(*dnet_node_get_logger(state_->n), DNET_LOG_DEBUG, app_, "%s: client session sends ack", signature_);
		dnet_send_ack(state_, const_cast<dnet_cmd*>(&cmd_copy_), error_code, 0);
	}
};


//
// `exec_back_stream` is a stream receiving responses from an app
// (in cocaine terms response stream called upstream).
//
// It transfers replies back to the original elliptics client which
// started EXEC transaction, it keeps client session used for that transfer
// and it notifies srw when stream job is done.
//
// Method write() accepts strings chunks from worker.
// It allows to write interactive worker application in a straightforward way:
//  - read SPH + data from request stream
//  - do some useful job
//  - send reply via response stream
// Chunked replies are allowed by elliptics protocol.
//
class exec_back_stream : public cocaine::api::stream_t
{
	dnet_logger *logger_;
	std::weak_ptr<client_session> client_;
	const std::string app_;
	const std::string signature_;
	std::function<void()> notify_completion_;

public:
	exec_back_stream(dnet_logger *logger, const std::shared_ptr<client_session> &client, std::function<void()> notify_completion
		)
		: logger_(logger)
		, client_(client)
		, app_(client->app_)
		, signature_(client->signature_)
		, notify_completion_(notify_completion)
	{}

	// stream_t interface

	virtual ~exec_back_stream() = default;

	// write() is called when worker sends chunk to the response stream.
	// write() performs transfer of data back to elliptics client.
	virtual stream_t& write(const std::string& chunk) {
		if (chunk.empty()) {
			SRW_LOG(*logger_, DNET_LOG_DEBUG, app_, "%s: stream: got chunk from app, size 0 -- 'drop me' signal", signature_);
			// empty chunk is a signal of chaining: the worker will not provide
			// the final result of event processing immediately but will instead
			// pass processing further down the chain -- result will be provided
			// eventually by a different channel and not through this stream
			client_.reset();

		} else {
			SRW_LOG(*logger_, DNET_LOG_DEBUG, app_, "%s: stream: got chunk from app, size %ld", signature_, chunk.size());
			if (auto client = client_.lock()) {
				client->send_chunk(chunk);

			} else {
				SRW_LOG(*logger_, DNET_LOG_ERROR, app_, "%s: stream: client session already closed", signature_);
			}
		}
		return *this;
	}

	virtual void error(const std::error_code& code, const std::string& reason) {
		SRW_LOG(*logger_, DNET_LOG_ERROR, app_, "%s: stream: got error from app, %s: (%d) %s", signature_, reason, code.value(), code.message());
		if (auto client = client_.lock()) {
			client->finish(code.value());

			// notify that we are done and this cocaine session closed
			//TODO: move to client_session
			notify_completion_();

			// stream object could be held live long after its close() or error() was called,
			// so explicitly dropping hold on client session helps
			client_.reset();

		} else {
			SRW_LOG(*logger_, DNET_LOG_DEBUG, app_, "%s: stream: client session already closed", signature_);
		}
	}

	virtual void close() {
		SRW_LOG(*logger_, DNET_LOG_DEBUG, app_, "%s: stream: got close", signature_);
		if (auto client = client_.lock()) {
			client->finish();

			// notify that we are done and this cocaine session closed
			//TODO: move to client_session?
			notify_completion_();

			// stream object could be held live long after its close() or error() was called,
			// so explicitly dropping hold on client session helps
			client_.reset();

		} else {
			SRW_LOG(*logger_, DNET_LOG_DEBUG, app_, "%s: stream: client session already closed", signature_);
		}
	}
};


//
// `push_back_stream` is a stub for the reply stream (upstream in cocaine terms).
//
// In some cases srw expects the other end (the app) to send no reply,
// so getting anything back indicate error in app behaviour.
//
class push_back_stream : public cocaine::api::stream_t
{
	dnet_logger *logger_;
	std::shared_ptr<client_session> client_;
	const std::string app_;
	const std::string signature_;

public:
	push_back_stream(dnet_logger *logger, const std::shared_ptr<client_session> &client)
		: logger_(logger)
		, client_(client)
		, app_(client->app_)
		, signature_(client->signature_)
	{}

	// stream_t interface

	// virtual ~push_back_stream() = default;
	virtual ~push_back_stream() {
		SRW_LOG(*logger_, DNET_LOG_DEBUG, app_, "%s: stream: close", signature_);
	}

	virtual stream_t& write(const std::string& chunk) {
		if (chunk.empty()) {
			SRW_LOG(*logger_, DNET_LOG_DEBUG, app_, "%s: stream: got chunk from app, size 0 -- 'drop me' signal", signature_);
			client_->finish();

			// stream object could be held live long after its close() or error() was called,
			// so explicitly dropping hold on client session helps
			client_.reset();

		} else {
			SRW_LOG(*logger_, DNET_LOG_ERROR, app_, "%s: stream: got chunk from app in no-reply-expected mode", signature_);
		}
		return *this;
	}

	virtual void error(const std::error_code& code, const std::string& reason) {
		SRW_LOG(*logger_, DNET_LOG_ERROR, app_, "%s: stream: got error from app in no-reply-expected mode, %s: (%d) %s", signature_, reason, code.value(), code.message());
		if (client_) {
			client_->finish(code.value());

			// stream object could be held live long after its close() or error() was called,
			// so explicitly dropping hold on client session helps
			client_.reset();
		}
	}

	virtual void close() {
		SRW_LOG(*logger_, DNET_LOG_DEBUG, app_, "%s: stream: got close", signature_);
		if (client_) {
			client_->finish();

			// stream object could be held live long after its close() or error() was called,
			// so explicitly dropping hold on client session helps
			client_.reset();
		}
	}
};

class srw
{
	struct exec_session
	{
		std::shared_ptr<client_session> client_session_;
		std::shared_ptr<cocaine::api::stream_t> back_stream_;
	};

	struct dnet_node   *m_node;

	// main cocaine core object -- context
	cocaine::context_t  m_ctx;

	// exec session map
	typedef std::map<uint64_t, std::shared_ptr<exec_session>> jobs_map_t;
	jobs_map_t          m_jobs;
	atomic_t            m_job_id_counter;

	// lock to serialize access to m_jobs
	std::mutex          m_lock;

	void register_job(int job_id, const std::shared_ptr<exec_session> &exec_session)
	{
		std::lock_guard<std::mutex> guard(m_lock);
		m_jobs.insert(std::make_pair(job_id, exec_session));
	}

	bool unregister_job(const std::string &signature, int job_id)
	{
		std::lock_guard<std::mutex> guard(m_lock);
		return unregister_job_nolock(signature, job_id);
	}

	bool unregister_job_nolock(const std::string &signature, int job_id)
	{
		dnet_log(m_node, DNET_LOG_DEBUG, "%s: srw: request to remove job %d", signature.c_str(), job_id);
		jobs_map_t::iterator found = m_jobs.find(job_id);
		if (found != m_jobs.end()) {
			m_jobs.erase(found);
			dnet_log(m_node, DNET_LOG_DEBUG, "%s: srw: job %d found and removed", signature.c_str(), job_id);
			return true;
		} else {
			//FIXME: fix log message text
			dnet_log(m_node, DNET_LOG_ERROR, "%s: srw: B, no job %d to complete", signature.c_str(), job_id);
			return false;
		}
	}

	// std::shared_ptr<exec_session> pop_job(const std::string &signature, int job_id)
	// {
	// 	{
	// 		std::lock_guard<std::mutex> guard(m_lock);
	// 		jobs_map_t::iterator found = m_jobs.find(job_id);
	// 		if (found != m_jobs.end()) {
	// 			std::shared_ptr<exec_stream> value = found->second;
	// 			m_jobs.erase(found);
	// 			return value;
	// 		}
	// 	}
	// 	//FIXME: fix log message text
	// 	dnet_log(m_node, DNET_LOG_ERROR, "%s: no job: %d to complete", signature.c_str(), job_id);
	// 	return std::shared_ptr<exec_session>();
	// }

	static
	boost::optional<const cocaine::service::node_t&> lookup_node_service(const cocaine::context_t &ctx)
	{
		//XXX: can we detect node service name automatically?
		if (const auto actor = ctx.locate("node")) {
			const auto &prototype = actor.get().prototype();
			const auto &object = dynamic_cast<const cocaine::service::node_t&>(prototype);
			return boost::optional<const cocaine::service::node_t&>(object);
		}

		return boost::none;
	}

	static std::string make_log_signature(struct dnet_net_state *st, struct dnet_cmd *cmd, const exec_context &exec)
	{
		//XXX: use more vivid formatting (cppformat?)
		std::ostringstream buf;

		buf << dnet_state_dump_addr(st) << ": trans: " << cmd->trans << ": " << dnet_dump_id(&cmd->id) << ", ";

		// In `exec` commands src_id (or origin id) is equal to dnet_cmd::id.
		// Same for simple cases of `reply` commands.
		// Origin id could differ from dnet_cmd::id in `push` commands and in those
		// `reply` commands that come from the end of `exec`/`push` chains.

		// add origin id to signature only if it differs -- no need to clutter log
		if (dnet_id_cmp_str(cmd->id.id, exec.src_id()->id) != 0) {
			// here and manually -- because family of dnet_id formatting functions
			// lack a variant that can accept dnet_raw_id (instead of dnet_id)
			char origin_id_str[DNET_DUMP_NUM * 2 + 1];
			dnet_dump_id_len_raw(exec.src_id()->id, DNET_DUMP_NUM, origin_id_str);
			buf << "(for origin id " << origin_id_str << "), ";
		}

		// exec_context's event have format {app}@{event}
		buf << exec.event();

		return buf.str();
	}

	static std::tuple<std::string, std::string> parse_srw_event(const std::string &s)
	{
		auto found = s.find("@");
		if (found != std::string::npos) {
			return std::make_tuple(s.substr(0, found), s.substr(found + 1));
		}
		return std::make_tuple(s, std::string());
	};


public:
	srw(struct dnet_node *n, const std::string &config)
		: m_node(n)
		//NOTE: context_t ctor throws an exception on config parse error
		, m_ctx(config, std::make_unique<logger_adapter>(m_node))
	{
		atomic_set(&m_job_id_counter, 0);

		// register `localnode` service
		//
		// the hard way, with asio::io_service exposed and other internal details visible
		// also note explicit upcast from localnode to service_t base
		// also note that there are two use sites for asio::io_service, and while its
		// customary to use the same object at both sites, but that's not strictly required
		//
		// this is the only possible way now
		{
			auto reactor = std::make_shared<asio::io_service>();
			std::unique_ptr<cocaine::api::service_t> service(
				new ioremap::elliptics::localnode(
					m_ctx, *reactor, "localnode", cocaine::dynamic_t(), m_node
				)
			);
			m_ctx.insert("localnode", std::make_unique<cocaine::actor_t>(
				m_ctx,
				reactor,
				std::move(service)
			));
		}

		// simpler way, for the time when cocaine core will develop proper api
		//
		// std::unique_ptr<cocaine::api::service_t> service(
		// 	   new ioremap::elliptics::localnode(
		// 	       m_ctx, "localnode", cocaine::dynamic_t(), m_node
		//     )
		// );
		// m_ctx.insert_service("localnode", std::move(service));
	}

	~srw()
	{
		// manually inserted services require manual removal
		m_ctx.remove("localnode");
	}

	int process(struct dnet_net_state *st, struct dnet_cmd *cmd, const void *data)
	{
		exec_context exec;
		try {
			exec = exec_context::from_raw(data, cmd->size);

		} catch(const error &e) {
			//TODO: add logging attribute 'source: srw'
			dnet_log(m_node, DNET_LOG_ERROR, "%s: srw: invalid exec_context: %s", dnet_dump_id(&cmd->id), e.what());
			return e.error_code();
		}

		// srw event should look like "application@method"
		const std::string srw_event = exec.event();
		std::string app;
		std::string event;
		{
			std::tie(app, event) = parse_srw_event(srw_event);
			if (app.empty() || event.empty()) {
				//TODO: add logging attribute 'source: srw'
				dnet_log(m_node, DNET_LOG_ERROR, "%s: srw: invalid event (should be {app}@{event}): %s", dnet_dump_id(&cmd->id), srw_event.c_str());
				return -EINVAL;
			}
		}

		const std::string signature = make_log_signature(st, cmd, exec);

		dnet_log(m_node, DNET_LOG_DEBUG, "%s: srw: start processing, payload size: %ld", signature.c_str(), exec.data().size());

		/**if ((event == "start-task") || (event == "start-multiple-task")) {
			std::unique_lock<std::mutex> guard(m_lock);
			eng_map_t::iterator it = m_map.find(app);
			if (it == m_map.end()) {
				auto eng = std::make_shared<dnet_app_t>(m_ctx, app, app);
				eng->start();

				if (event == "start-multiple-task") {
					auto storage = cocaine::api::storage(m_ctx, "core");
					Json::Value profile = storage->get<Json::Value>("profiles", app);

					int idle = profile["idle-timeout"].asInt();
					int pool_limit = profile["pool-limit"].asInt();
					const int idle_min = 60 * 60 * 24 * 30;

					dnet_log(m_node, DNET_LOG_INFO, "%s: multiple start: idle: %d/%d, workers: %d", signature.c_str(), idle, idle_min, pool_limit);

					if (idle && idle < idle_min) {
						dnet_log(m_node, DNET_LOG_ERROR, "%s: multiple start: "
							"idle must be big enough, we check it to be larger than 30 days (%d seconds), "
							"current profile value is %d",
							signature.c_str(), idle_min, idle);
						return -EINVAL;
					}

					eng->set_pool_size(pool_limit);

					if (sph->data_size) {
						std::string task_id(data + sph->event_size, sph->data_size);

						eng->set_task_id(task_id);
					}
				}

				m_map.insert(std::make_pair(app, eng));

				dnet_log(m_node, DNET_LOG_INFO, "%s: started", signature.c_str());

			} else {
				dnet_log(m_node, DNET_LOG_INFO, "%s: was already started", signature.c_str());
			}

		} else if (event == "stop-task") {
			std::unique_lock<std::mutex> guard(m_lock);
			eng_map_t::iterator it = m_map.find(app);
			// destructor stops engine
			if (it != m_map.end())
				m_map.erase(it);
			guard.unlock();

			dnet_log(m_node, DNET_LOG_INFO, "%s: stopped", signature.c_str());

		}
		*/

		// Handle control-over-elliptics-channel commands.

		if (event == "start-task"
				|| event == "start-multiple-task"
				|| event == "stop-task"
				|| event == "info") {

			dnet_log(m_node, DNET_LOG_INFO, "%s: srw: app control", signature.c_str());

			//XXX: how we can differentiate services from apps?

			//XXX: can we detect node service name automatically?
			if (auto node = lookup_node_service(m_ctx)) {
				try {
					if (event == "info") {
						auto doc = node.get().info(app,
							cocaine::io::node::info::flags_t(
								cocaine::io::node::info::overseer_report
								| cocaine::io::node::info::expand_manifest
								| cocaine::io::node::info::expand_profile
							)
						);
						auto text = boost::lexical_cast<std::string>(doc);

						client_session client(st, cmd, app, signature, exec);
						client.send_chunk(text);
						client.finish();

						// we've just sent ack with client.finish(),
						// clearing NEED_ACK flag will turn off ack sending
						// done in dnet_process_cmd_raw
						cmd->flags &= ~DNET_FLAGS_NEED_ACK;

					} else {
						//TODO: reimplement support for other commands:
						// their working over elliptics channel is not required right now,
						// but still convenient and has value
						return -ENOTSUP;
					}

				} catch (const std::exception &e) {
					// exception text must be "app '{name}' is not running"
					dnet_log(m_node, DNET_LOG_ERROR, "%s: srw: %s", signature.c_str(), e.what());
					return -ENOENT;
				}

			} else {
				dnet_log(m_node, DNET_LOG_ERROR, "%s: 'node' ('node::v2') service not found, but its required to be able to run user apps; check cocaine config", signature.c_str());
				return -ENOENT;
			}

			return 0;
		}

		// So, here some non-control EXEC command came to us via elliptics channel.

		try {

			if (exec.is_reply()) {
				// This is a reply in a sequence of replies to some previous exec.

				dnet_log(m_node, DNET_LOG_INFO, "%s: srw: reply pass", signature.c_str());

				// This segment could be marked as the final segment in a sequence,
				// which means exec session should be finalized and closed.
				//
				const int job_id = exec.src_key();

				std::unique_lock<std::mutex> guard(m_lock);

				jobs_map_t::iterator found = m_jobs.find(job_id);
				if (found == m_jobs.end()) {
					//FIXME: fix log message text
					dnet_log(m_node, DNET_LOG_ERROR, "%s: srw: A, no job %d to complete", signature.c_str(), job_id);
					return -ENOENT;
				}

				// take our chance to release job map as soon as possible
				std::shared_ptr<exec_session> exec_session = found->second;
				if (exec.is_final()) {
					m_jobs.erase(found);
					// exec_session object should be released and actually destroyed
					// as soon as `exec_session` ptr goes out of scope
				}
				guard.unlock();

				//XXX: move addr fixup here?

				// and only then do the real work with reply
				exec_session->client_session_->send_chunk(exec.data());
				if (exec.is_final()) {
					exec_session->client_session_->finish();
				}

				//XXX: why that addr fixup? why it made after reply?
				// memcpy(&sph->addr, &st->n->addrs[0], sizeof(struct dnet_addr));

				//NOTE: transient client that sent this `reply` is not the same
				// as the original client that had sent original `exec` --
				// -- original client was just answered through client_session

				// Ack to the transient client will be sent by dnet_process_cmd_raw(),
				// it does ack auto sending for all io commands --
				// -- only if NEED_ACK flag is not specifically cleared,
				// and we are not clearing NEED_ACK flag.

			} else {
				// This is an original exec.
				//
				dnet_log(m_node, DNET_LOG_INFO, "%s: srw: forward pass", signature.c_str());

				auto node = lookup_node_service(m_ctx);
				if (!node) {
					dnet_log(m_node, DNET_LOG_ERROR, "%s: 'node' ('node::v2') service not found, but its required to be able to run user apps; check cocaine config", signature.c_str());
					return -ENOENT;
				}

				std::shared_ptr<cocaine::overseer_t> app_overseer;
				try {
					app_overseer = node.get().overseer(app);

				} catch (const cocaine::error_t &e) {
					// exception text must be "app '{name}' is not running"
					dnet_log(m_node, DNET_LOG_ERROR, "%s: srw: %s", signature.c_str(), e.what());
					return -ENOENT;
				}

				// src_key in exec can be used to map processing to a specific worker,
				// src_key in reply will be used as job_id to find job in job map.
				// Original src_key gets saved into exec_session and substituted by the job_id
				// generated by the srw.
				const int src_key = exec.src_key();
				const bool reply_expected = !!(exec.flags() & DNET_SPH_FLAGS_SRC_BLOCK);
				int job_id = 0;

				if (reply_expected) {
					job_id = atomic_inc(&m_job_id_counter);
					exec.set_src_key(job_id);
					memcpy(exec.src_id(), cmd->id.id, sizeof(exec.src_id()->id));
				}

				auto clean_copy = [] (const exec_context &other) -> exec_context {
					return exec_context_data::copy(other, other.event(), data_pointer());
				};

				auto session = std::make_shared<client_session>(
					st, cmd, app, signature, clean_copy(exec)
				);
				dnet_log(m_node, DNET_LOG_DEBUG, "%s: srw: exec_context original, size: total %ld, event %ld(%d), payload %ld", signature.c_str(),
					exec.native_data().size(), exec.event().size(), exec.native_data().data<sph>()->event_size, exec.data().size()
				);
				dnet_log(m_node, DNET_LOG_DEBUG, "%s: srw: exec_context session copy, size: total %ld, event %ld(%d), payload %ld", signature.c_str(),
					session->exec_copy_.native_data().size(), session->exec_copy_.event().size(), session->exec_copy_.native_data().data<sph>()->event_size, session->exec_copy_.data().size()
				);

				// optional tag to stick processing to a certain worker
				//FIXME: what about tagging of `push` requests?
				std::string tag;

				std::shared_ptr<cocaine::api::stream_t> back_stream;
				if (reply_expected) {
					// for `exec`
					dnet_log(m_node, DNET_LOG_INFO, "%s: srw: mode exec (src_key %d replaced with job %d)", signature.c_str(), src_key, job_id);

					back_stream = std::make_shared<exec_back_stream>(m_node->log, session,
						std::bind(&srw::unregister_job, this, signature, job_id)
					);

					if (src_key >= 0) {
						const int index = (src_key % app_overseer->profile().pool_limit);
						//TODO: think out tag format
						tag = /* {unique app instance id} + */ app + ".worker-" + std::to_string(index);
					}

				} else {
					// for `push`
					dnet_log(m_node, DNET_LOG_INFO, "%s: srw: mode push", signature.c_str());

					back_stream = std::make_shared<push_back_stream>(m_node->log, session);
				}

				try {
					//FIXME: get rid of this copy from data_pointer to a std::string
					const std::string chunk = exec.native_data().to_string();

					dnet_log(m_node, DNET_LOG_DEBUG, "%s: srw: enqueueing, tag '%s', event '%s', chunk size %lu", signature.c_str(), tag.c_str(), event.c_str(), chunk.size());

					auto send_stream = app_overseer->enqueue(
						back_stream,
						cocaine::app::event_t(event),
						cocaine::service::node::slave::id_t(tag)
					);

					send_stream->write(chunk);

					// Request stream should be closed after all data was sent to prevent resource leakage.
					send_stream->close();

					dnet_log(m_node, DNET_LOG_INFO, "%s: srw: enqueued, src_key %d, job %d, payload size %zd, block %d",
						signature.c_str(),
						src_key, job_id, exec.data().size(), reply_expected
					);

				} catch (const std::exception &e) {
					dnet_log(m_node, DNET_LOG_ERROR, "%s: srw: enqueue error, src_key %d, job %d, payload size %zd, block %d: %s",
						signature.c_str(),
						src_key, job_id, exec.data().size(), reply_expected,
						e.what()
					);
					return -EXFULL;
				}

				if (reply_expected) {
					// register exec session in job map
					register_job(job_id, std::make_shared<exec_session>(exec_session{session, back_stream}));
				}

				// clearing NEED_ACK flag turns off ack auto sending
				// (dnet_process_cmd_raw() does that) -- client should receive ack
				// only after it'll get result of exec processing
				cmd->flags &= ~DNET_FLAGS_NEED_ACK;
			}

			return 0;

		} catch(const std::exception &e) {
			dnet_log(m_node, DNET_LOG_ERROR, "%s: processing failed: %s", signature.c_str(), e.what());
		}

		return -EINVAL;
	}
};

}} // namespace ioremap::elliptics


int dnet_srw_init(struct dnet_node *n, struct dnet_config *cfg)
{
	try {
		dnet_log(n, DNET_LOG_INFO, "srw: init, config: %s", cfg->srw.config);
		n->srw = new ioremap::elliptics::srw(n, cfg->srw.config);
		dnet_log(n, DNET_LOG_INFO, "srw: init done, config: %s", cfg->srw.config);
		return 0;

	} catch (const cocaine::error_t &e) {
		dnet_log(n, DNET_LOG_ERROR, "srw: init failed, config: %s, config error: %s", cfg->srw.config, e.what());
		return -EINVAL;

	} catch (const std::system_error &e) {
		dnet_log(n, DNET_LOG_ERROR, "srw: init failed, config: %s, exception: %s", cfg->srw.config, e.what());
		return -ENOMEM;
	}
}

void dnet_srw_cleanup(struct dnet_node *n)
{
	if (n->srw) {
		try {
			dnet_log(n, DNET_LOG_INFO, "srw: fini");
			auto *srw = static_cast<ioremap::elliptics::srw*>(n->srw);
			delete srw;
			dnet_log(n, DNET_LOG_INFO, "srw: fini done");

		} catch(const std::exception &e) {
			dnet_log(n, DNET_LOG_ERROR, "srw: fini failed: %s", e.what());

		} catch (...) {
			dnet_log(n, DNET_LOG_ERROR, "srw: fini failed by unknown reason");
		}

		n->srw = NULL;
	}
}

int dnet_cmd_exec(struct dnet_net_state *st, struct dnet_cmd *cmd, const void *payload)
{
	struct dnet_node *n = st->n;
	auto *srw = static_cast<ioremap::elliptics::srw*>(n->srw);

	if (!srw)
		return -ENOTSUP;

	try {
		return srw->process(st, cmd, payload);

	} catch (...) {
		dnet_log(n, DNET_LOG_ERROR, "srw: processing failed by unknown reason");
		return -EINVAL;
	}
}

int dnet_srw_update(struct dnet_node *, int)
{
	return 0;
}

#else // HAVE_COCAINE_SUPPORT = 0

// cocaine support disabled

#include <errno.h>

#include "elliptics/srw.h"

int dnet_srw_init(struct dnet_node *, struct dnet_config *)
{
	return -ENOTSUP;
}

void dnet_srw_cleanup(struct dnet_node *)
{
}

int dnet_cmd_exec(struct dnet_net_state *, struct dnet_cmd *, const void *)
{
	return -ENOTSUP;
}

int dnet_srw_update(struct dnet_node *, int)
{
	return 0;
}

#endif // HAVE_COCAINE_SUPPORT
