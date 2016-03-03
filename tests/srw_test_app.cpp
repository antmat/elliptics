#include <stdexcept>

//XXX: in order to give preference to the old blackhole (from foreign/blackhole)
// includes of elliptics must come before includes of cocaine

#include "elliptics/session.hpp"

#include <blackhole/v1/attribute.hpp>
#include <blackhole/message.hpp>
#include <blackhole/extensions/facade.hpp>

#include <cocaine/framework/manager.hpp>
#include <cocaine/framework/worker.hpp>
#include <cocaine/framework/service.hpp>
#include <cocaine/traits/tuple.hpp>
#include <cocaine/traits/enum.hpp>

#include "srw_test_base.hpp"

using namespace ioremap;
using namespace cocaine::framework;
namespace ph = std::placeholders;

namespace {

void keep_tx_live_till_done(elliptics::async_reply_result &async, worker::sender &tx)
{
	//XXX: what we want is to prolong life of tx sender until we done sending the reply;
	// * so we use the fact that std::bind ignores those actual call args which don't match
	//   inner function args (actual args object will be created but then discarded)
	// * but we can't capture tx object directly and instead forced to hold it by shared_ptr,
	//   that is because tx is movable-only, so binded functor is also movable-only
	//   but std::function which elliptics accepts as a callback requires functors
	//   to be copy constructable
	async.connect(std::bind(
		[](std::shared_ptr<worker::sender>) {},
		std::make_shared<worker::sender>(std::move(tx))
	));
}

}

struct app_context {
	typedef service<cocaine::io::log_tag> logging_service_type;

	std::string id_;
	std::shared_ptr<logging_service_type> log_;

	std::unique_ptr<elliptics::file_logger> logger;
	std::unique_ptr<elliptics::node> node;
	std::unique_ptr<elliptics::session> reply_client;

	app_context(const std::shared_ptr<logging_service_type> &log, const options_t &options)
	    : id_(options.name)
	    , log_(log)
	{}

	void log(int severity, const blackhole::v1::lazy_message_t &message, blackhole::v1::attribute_pack& pack);

	void init_elliptics_client(worker::sender tx, worker::receiver rx);
	void echo_via_elliptics(worker::sender tx, worker::receiver rx);
	void echo_via_cocaine(worker::sender tx, worker::receiver rx);
	void noreply(worker::sender tx, worker::receiver rx);
	void noreply_30seconds_wait(worker::sender tx, worker::receiver rx);

	void chain_via_elliptics(worker::sender tx, worker::receiver rx, const int step, const std::string next_event);
};

void app_context::log(int severity, const blackhole::v1::lazy_message_t &message, blackhole::v1::attribute_pack& pack)
{
	//XXX: can't really use attribute pack: blackhole v0/v1 clash on it here
	(void) pack;
	log_->invoke<cocaine::io::log::emit>(
		cocaine::logging::priorities(severity),
		id_,
		message.supplier().to_string()
	);
}

#undef LOG_DEBUG
#undef LOG_INFO
#undef LOG_ERROR
#define LOG_DEBUG(logger, ...) \
	blackhole::v1::logger_facade<app_context>(logger).log(cocaine::logging::debug, __VA_ARGS__)
#define LOG_INFO(logger, ...) \
	blackhole::v1::logger_facade<app_context>(logger).log(cocaine::logging::info, __VA_ARGS__)
#define LOG_ERROR(logger, ...) \
	blackhole::v1::logger_facade<app_context>(logger).log(cocaine::logging::error, __VA_ARGS__)

void app_context::init_elliptics_client(worker::sender tx, worker::receiver rx)
{
	LOG_DEBUG(*this, "{}: ENTER", __func__);

	const std::string input(std::move(rx.recv().get().get()));
	LOG_DEBUG(*this, "{}: input-size: {}", __func__, input.size());
	LOG_DEBUG(*this, "{}: event-size: {}, data-size: {}", __func__, ((const sph*)input.data())->event_size, ((const sph*)input.data())->data_size);
	auto context = elliptics::exec_context::from_raw(input.data(), input.size());

	tests::node_info info;
	info.unpack(context.data().to_string());

	const std::string log_path = info.path + "/" + "app-client.log";

	logger.reset(new elliptics::file_logger(log_path.c_str(), DNET_LOG_DEBUG));
	// differentiate this client from others in the log
	logger->add_attribute({"source", {"in-app-client"}});

	node.reset(new elliptics::node(elliptics::logger(*logger, blackhole::log::attributes_t())));

	for (auto it = info.remotes.begin(); it != info.remotes.end(); ++it) {
		node->add_remote(it->c_str());
	}

	reply_client.reset(new elliptics::session(*node));
	reply_client->set_groups(info.groups);

	auto async = reply_client->reply(context, std::string("inited"), elliptics::exec_context::final);
	keep_tx_live_till_done(async, tx);

	LOG_DEBUG(*this, "{}: EXIT", __func__);
}

/// Echo input data via elliptics channel
void app_context::echo_via_elliptics(worker::sender tx, worker::receiver rx)
{
	LOG_DEBUG(*this, "{}: ENTER", __func__);

	if (!reply_client) {
		tx.error(-EINVAL, "not initialized yet").get();
		return;
	}

	const std::string input(std::move(rx.recv().get().get()));
	auto context = elliptics::exec_context::from_raw(input.data(), input.size());

	LOG_INFO(*this, "{}: data '{}', size {}", __func__, context.data().to_string(), context.data().size());

	auto async = reply_client->reply(context, context.data(), elliptics::exec_context::final);
	keep_tx_live_till_done(async, tx);

	LOG_DEBUG(*this, "{}: EXIT", __func__);
}

/// Echo input data via cocaine response stream
void app_context::echo_via_cocaine(worker::sender tx, worker::receiver rx)
{
	LOG_DEBUG(*this, "{}: ENTER", __func__);

	const std::string input(std::move(rx.recv().get().get()));
	auto context = elliptics::exec_context::from_raw(input.data(), input.size());

	LOG_INFO(*this, "{}: data '{}', size {}", __func__, context.data().to_string(), context.data().size());

	tx.write(context.native_data().to_string()).get().close().get();

	LOG_DEBUG(*this, "{}: EXIT", __func__);
}

/// Make no reply at all
void app_context::noreply(worker::sender tx, worker::receiver rx)
{
	LOG_DEBUG(*this, "{}: ENTER", __func__);

	(void)tx;

	const std::string input(std::move(rx.recv().get().get()));
	auto context = elliptics::exec_context::from_raw(input.data(), input.size());

	LOG_INFO(*this, "{}: data '{}', size {}", __func__, context.data().to_string(), context.data().size());

	LOG_DEBUG(*this, "{}: EXIT", __func__);
}

/// Used for timeout test.
/// Make no reply but do not return immediately either (for the client timeout duration at least).
///
/// Return from event handler means 'close' event in the channel to the srw and subsequently
/// 'ack' event back to the elliptics client, which is exactly what we don't want in this test
/// -- we what elliptics client's transaction to timeout.
///
/// Be careful, this function doesn't check whether application was initialized or not
/// (compare to @echo_via_elliptics function).
/// It is possible that new cocaine workers will be spawned only to handle this event type,
/// and client will not send 'init' event to them.
void app_context::noreply_30seconds_wait(worker::sender tx, worker::receiver rx)
{
	LOG_DEBUG(*this, "{}: ENTER", __func__);

	(void)tx;

	const std::string input = rx.recv().get().get();
	auto context = elliptics::exec_context::from_raw(input.data(), input.size());

	LOG_INFO(*this, "{}: data '{}', size {}", __func__, context.data().to_string(), context.data().size());

	sleep(30);

	LOG_DEBUG(*this, "{}: EXIT", __func__);
}

/// Pass input message to the next step in chain with `push` command
void app_context::chain_via_elliptics(worker::sender tx, worker::receiver rx, const int step, const std::string next_event)
{
	LOG_DEBUG(*this, "{}: ENTER ({})", __func__, step);

	if (!reply_client) {
		tx.error(-EINVAL, "not initialized yet").get();
		return;
	}

	const std::string input(std::move(rx.recv().get().get()));
	auto context = elliptics::exec_context::from_raw(input.data(), input.size());

	LOG_INFO(*this, "{}: data '{}', size {}", __func__, context.data().to_string(), context.data().size());

	auto client = reply_client->clone();
	client.set_trace_id(step);

	dnet_id next_id = {{0}, 0, 0};
	client.transform(next_event, next_id);
	auto async = client.push(&next_id, context, next_event, context.data());

	tx.write("").get();
	keep_tx_live_till_done(async, tx); // invalidates `tx`

	LOG_DEBUG(*this, "{}: EXIT ({})", __func__, step);
}

int main(int argc, char **argv)
{
	worker_t worker(options_t(argc, argv));
	app_context context(worker.manager().logger(), worker.options());

	// manual connect required to (somewhat) ensure order in log message stream
	// (cocaine's client-service protocol does not guarantee order for the messages
	// happening during (or close to) connection/re-connection phase)
	worker.manager().logger()->connect().get();

	LOG_INFO(context, "{}, registering event handler(s)", context.id_.c_str());

	worker.on("init", std::bind(&app_context::init_elliptics_client, &context, ph::_1, ph::_2));

	worker.on("echo-via-elliptics", std::bind(&app_context::echo_via_elliptics, &context, ph::_1, ph::_2));
	worker.on("echo-via-cocaine", std::bind(&app_context::echo_via_cocaine, &context, ph::_1, ph::_2));
	worker.on("noreply", std::bind(&app_context::noreply, &context, ph::_1, ph::_2));
	worker.on("noreply-30seconds-wait", std::bind(&app_context::noreply_30seconds_wait, &context, ph::_1, ph::_2));

	// test exec+push chains

	// 2 step chain
	worker.on("2-step-chain-via-elliptics", std::bind(&app_context::chain_via_elliptics, &context,
		ph::_1, ph::_2,
		1, context.id_ + "@echo-via-elliptics"
	));

	// 3 step chain
	worker.on("3-step-chain-via-elliptics", std::bind(&app_context::chain_via_elliptics, &context,
		ph::_1, ph::_2,
		1, context.id_ + "@3-step-chain-via-elliptics-2"
	));
	worker.on("3-step-chain-via-elliptics-2", std::bind(&app_context::chain_via_elliptics, &context,
		ph::_1, ph::_2,
		2, context.id_ + "@echo-via-elliptics"
	));

	// 4 step chain
	worker.on("4-step-chain-via-elliptics", std::bind(&app_context::chain_via_elliptics, &context,
		ph::_1, ph::_2,
		1, context.id_ + "@4-step-chain-via-elliptics-2"
	));
	worker.on("4-step-chain-via-elliptics-2", std::bind(&app_context::chain_via_elliptics, &context,
		ph::_1, ph::_2,
		2, context.id_ + "@4-step-chain-via-elliptics-3"
	));
	worker.on("4-step-chain-via-elliptics-3", std::bind(&app_context::chain_via_elliptics, &context,
		ph::_1, ph::_2,
		3, context.id_ + "@echo-via-elliptics"
	));

	//TODO: add chaining via cocaine
	//TODO: add chaining and reply via localnode

	LOG_INFO(context, "{}, application started", context.id_.c_str());

	return worker.run();
}


