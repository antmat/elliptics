/*
 * 2013+ Copyright (c) Ruslan Nigmatullin <euroelessar@yandex.ru>
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

#include <algorithm>

#include <boost/program_options.hpp>
#define BOOST_TEST_NO_MAIN
#include <boost/test/included/unit_test.hpp>

#include "elliptics/interface.h"
#include "exec_context_data_p.hpp"

#include <cocaine/framework/manager.hpp>
#include <cocaine/framework/service.hpp>

#include <cocaine/traits/tuple.hpp>
#include "cocaine/idl/localnode.hpp"
#include "cocaine/traits/localnode.hpp"

#include "srw_test_base.hpp"

namespace {

std::string to_hex_string(const unsigned char * data, const size_t size)
{
	static const char hexchars[] = "0123456789abcdef";
	std::string result;
	result.reserve(size * 2);
	for (size_t i = 0; i < size; ++i) {
		const unsigned char byte = data[i];
		result.push_back(hexchars[(byte >> 4) & 0x0f]);
		result.push_back(hexchars[byte & 0x0f]);
	}
	return result;
}

std::string gen_random(const int len) {
	static const char alphanum[] =
		"0123456789"
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		"abcdefghijklmnopqrstuvwxyz";

	std::string result;
	result.reserve(len);
	for (int i = 0; i < len; ++i) {
		result += alphanum[rand() % (sizeof(alphanum) - 1)];
	}
	return result;
}

}

// required for localnode_test
//
namespace ioremap { namespace elliptics {

inline bool operator==(const dnet_async_service_result &a, const dnet_async_service_result &b)
{
	return ((0 == dnet_addr_cmp(&a.addr, &b.addr))
		&& (0 == memcmp(&a.file_info, &b.file_info, sizeof(a.file_info)))
		&& a.file_path == b.file_path
	);
}

inline std::ostream& operator<<(std::ostream& ostr, const dnet_async_service_result &v)
{
	ostr << "{\n addr=" << dnet_addr_string(&v.addr)
		<< ",\n " << "record_flags=" << v.file_info.record_flags
		<< ",\n " << "size=" << v.file_info.size
		<< ",\n " << "time=" << dnet_print_time(&v.file_info.mtime)
		<< ",\n " << "file=" << v.file_path
		<< ",\n " << "checksum=" << to_hex_string(v.file_info.checksum, sizeof(v.file_info.checksum))
		<< "\n}"
		;
	return ostr;
}

}}

namespace tests {

using namespace ioremap::elliptics;
using namespace boost::unit_test;

static std::shared_ptr<nodes_data> global_data;

bool register_tests(test_suite *suite, node n);

static void configure_nodes(const std::vector<std::string> &remotes, const std::string &path)
{
	if (remotes.empty()) {
		start_nodes_config start_config(results_reporter::get_stream(), std::vector<server_config>({
			server_config::default_srw_value().apply_options(config_data()
				("group", 1)
			)
		}), path);

		global_data = start_nodes(start_config);
	} else {
		global_data = start_nodes(results_reporter::get_stream(), remotes, path);
	}
}

static void destroy_global_data()
{
	global_data.reset();
}

static void init_application(session &sess, const std::string &app_name)
{
	init_application_impl(sess, app_name, *global_data);
}

boost::unit_test::test_suite *create_testsuite(int argc, char *argv[])
{
	namespace bpo = boost::program_options;

	bpo::variables_map vm;
	bpo::options_description generic("Test options");

	std::vector<std::string> remotes;
	std::string path;

	generic.add_options()
			("help", "This help message")
			("remote", bpo::value(&remotes), "Remote elliptics server address")
			("path", bpo::value(&path), "Path where to store everything")
			;

	bpo::store(bpo::parse_command_line(argc, argv, generic), vm);
	bpo::notify(vm);

	if (vm.count("help")) {
		std::cerr << generic;
		return NULL;
	}

	test_suite *suite = new test_suite("Local Test Suite");

	configure_nodes(remotes, path);

	register_tests(suite, *global_data->node);

	return suite;
}

///
/// Checks worker response via worker's own elliptics client.
/// (For some time it was the only working way to respond.)
/// Original client is able to receive reply.
///
static void test_echo_via_elliptics(session &sess, const std::string &app_name, const std::string &data)
{
	key key_id(gen_random(8));
	key_id.transform(sess);
	dnet_id id = key_id.id();

	ELLIPTICS_REQUIRE(exec_result, sess.exec(&id, app_name + "@echo-via-elliptics", data));

	sync_exec_result result = exec_result;
	BOOST_REQUIRE_EQUAL(result.size(), 1);
	BOOST_REQUIRE_EQUAL(result[0].context().data().to_string(), data);
}

///
/// Checks worker response via cocaine response stream.
/// From original client's point of view there should be no difference.
/// Original client is able to receive reply.
///
static void test_echo_via_cocaine(session &sess, const std::string &app_name, const std::string &data)
{
	key key_id(gen_random(8));
	key_id.transform(sess);
	dnet_id id = key_id.id();

	ELLIPTICS_REQUIRE(exec_result, sess.exec(&id, app_name + "@echo-via-cocaine", data));

	sync_exec_result result = exec_result;
	BOOST_REQUIRE_EQUAL(result.size(), 1);
	BOOST_REQUIRE_EQUAL(result[0].context().data().to_string(), data);
}

///
/// Checks `push` processing expectations.
/// Original client receives nothing even from handlers that send replies.
///
static void test_push(session &client, const std::string &app_name, const std::string &data)
{
	auto origin = exec_context_data::create("dummy-event", "dummy-data");

	{
		key key(std::string(__func__) + "noreply");
		key.transform(client);
		dnet_id id = key.id();
		ELLIPTICS_REQUIRE(async, client.push(&id, origin, app_name + "@noreply", data));
		BOOST_REQUIRE_EQUAL(async.get().size(), 0);
	}
	{
		key key(std::string(__func__) + "echo-via-elliptics");
		key.transform(client);
		dnet_id id = key.id();
		ELLIPTICS_REQUIRE(async, client.push(&id, origin, app_name + "@echo-via-elliptics", data));
		BOOST_REQUIRE_EQUAL(async.get().size(), 0);
	}
	{
		key key(std::string(__func__) + "echo-via-cocaine");
		key.transform(client);
		dnet_id id = key.id();
		ELLIPTICS_REQUIRE(async, client.push(&id, origin, app_name + "@echo-via-cocaine", data));
		BOOST_REQUIRE_EQUAL(async.get().size(), 0);
	}
}

///
/// Checks `exec`->`push`(->`push`...) chaining.
/// Original client is able to receive reply from the end of the chain
/// (from it's point of view there should be no difference with test_echo_*).
///
static void test_chain_via_elliptics(session &client, const std::string &app_name, const std::string &start_event, const std::string &data)
{
	key key_id(__func__ + start_event);
	key_id.transform(client);
	dnet_id id = key_id.id();

	ELLIPTICS_REQUIRE(async, client.exec(&id, app_name + "@" + start_event, data));
	BOOST_REQUIRE_EQUAL(async.get().size(), 1);
	BOOST_REQUIRE_EQUAL(async.get()[0].context().data().to_string(), data);
}

///
/// This funky thread is needed to periodically 'ping' network connection to given node,
/// since otherwise 3 timed out transaction in a row will force elliptics client to kill
/// this connection and all subsequent transactions will be timed out prematurely.
///
/// State is only marked as 'stall' (and eventually will be killed) when it faces timeout transactions.
/// Read transactions will be quickly completed (even with ENOENT error), which resets stall
/// counter of the selected network connection.
///
class thread_watchdog {
	public:
		session sess;

		thread_watchdog(const session &sess) : sess(sess), need_exit(false) {
			tid = std::thread(std::bind(&thread_watchdog::ping, this));
		}

		~thread_watchdog() {
			need_exit = true;
			tid.join();
		}

	private:
		std::thread tid;
		bool need_exit;

		void ping() {
			while (!need_exit) {
				sess.read_data(std::string("test-key"), 0, 0).wait();
				sleep(1);
			}
		}
};

///
/// Checks timeout mechanics on `exec` commands.
///
/// Runs @num exec transactions with random timeouts.
///
/// Timeouts must be set to less than 30 seconds, since 30 seconds is a magic number:
/// first, because that's the number of seconds cocaine application sleeps in 'noreply' event,
/// second, because cocaine sends heartbeats every 30 seconds (or at least complains that it didn't
/// receive heartbeat after 30 seconds) and kills application.
///
/// Trying to set 'heartbeat-timeout' in profile to 60 seconds didn't help.
/// See tests/srw_test.hpp file where we actually set 3 different timeouts to 60 seconds,
/// but yet application is killed in 30 seconds.
///TODO: check if this is still true after moving to cocaine v12.
///
/// Basic idea behind this test is following: we run multiple exec transactions with random timeouts,
/// and all transactions must be timed out at most in 2 seconds after timeout expired. These 2 seconds
/// happen because of checker thread which checks timer tree every second and check time (in seconds)
/// must be greater than so called 'death' time.
///
/// For more details see dnet_trans_iterate_move_transaction() function.
///
static void test_timeout(session &sess, const std::string &app_name)
{
	key key_id = app_name;
	key_id.transform(sess);
	dnet_id id = key_id.id();

	// just a number of test transactions
	int num = 50;

	std::vector<std::pair<int, async_exec_result>> results;
	results.reserve(num);

	sess.set_exceptions_policy(session::no_exceptions);

	thread_watchdog ping(sess);

	for (int i = 0; i < num; ++i) {
		int timeout = rand() % 20 + 1;
		sess.set_timeout(timeout);

		results.emplace_back(std::make_pair(timeout, sess.exec(&id, app_name + "@noreply-30seconds-wait", "some data")));
	}


	for (auto it = results.begin(); it != results.end(); ++it) {
		auto & res = it->second;

		res.wait();

		auto elapsed = res.elapsed_time();
		auto diff = elapsed.tsec - it->first;

		// 2 is a magic number of seconds, I tried to highlight it in the test description
		unsigned long max_diff = 2;

		if (diff >= max_diff) {
			printf("elapsed: %lld.%lld, timeout: %d, diff: %ld, must be less than %ld, error: %s [%d]\n",
					(unsigned long long)elapsed.tsec, (unsigned long long)elapsed.tnsec, it->first,
					diff, max_diff,
					res.error().message().c_str(), res.error().code());

			BOOST_REQUIRE_LE(elapsed.tsec - it->first, max_diff);
		}
	}
}

///
/// Checks serializability of data structures used in `localnode` service interface.
///
template<class T>
T pack_unpack(const T &v)
{
	msgpack::sbuffer buf;
	msgpack::pack(buf, v);

	msgpack::unpacked msg;
	msgpack::unpack(&msg, buf.data(), buf.size());

	return msg.get().as<T>();
}

static void test_localnode_data_serialization()
{
	{
		dnet_raw_id a = { "0123455678" };
		dnet_raw_id b = pack_unpack(a);
		BOOST_REQUIRE_EQUAL(std::string((const char *)a.id), std::string((const char *)b.id));
	}
	{
		dnet_addr a = { "abc", 5, 8 };
		dnet_addr b = pack_unpack(a);
		BOOST_REQUIRE_EQUAL(a.addr_len, b.addr_len);
		BOOST_REQUIRE_EQUAL(std::string((const char *)a.addr, a.addr_len), std::string((const char *)b.addr, b.addr_len));
		BOOST_REQUIRE_EQUAL(a.family, b.family);
	}
	{
		dnet_time a = { 5, 8 };
		dnet_time b = pack_unpack(a);
		BOOST_REQUIRE_EQUAL(a.tsec, b.tsec);
		BOOST_REQUIRE_EQUAL(a.tnsec, b.tnsec);
	}
	{
		dnet_file_info a = { 3, "abc", 5, 8, 10, { 2, 3} };
		dnet_file_info b = pack_unpack(a);
#define CMP(x) BOOST_REQUIRE_EQUAL(a.x, b.x)
		CMP(flen);
		CMP(record_flags);
		CMP(size);
		CMP(offset);
		CMP(mtime.tsec);
		CMP(mtime.tnsec);
#undef CMP
		BOOST_REQUIRE_EQUAL(
			std::string((const char *)a.checksum, sizeof(a.checksum)),
			std::string((const char *)b.checksum, sizeof(b.checksum))
		);
	}
	{
		dnet_async_service_result a = {
			{ "abc", 5, 8 },
			{ 3, "abc", 5, 8, 10, { 2, 3} },
			"file path"
		};
		dnet_async_service_result b = pack_unpack(a);
		BOOST_REQUIRE_EQUAL(a, b);
	}
}

///
/// Checks if `localnode` service methods are really working.
///
static void test_localnode(session &sess, const std::vector<int> &groups)
{
	using cocaine::framework::service_manager_t;

	service_manager_t::endpoint_type endpoint(boost::asio::ip::address_v4::loopback(), global_data->locator_port);
	service_manager_t manager({endpoint}, 1);

	auto localnode = manager.create<io::localnode_tag>("localnode");

	key key(gen_random(8));
	key.transform(sess);

	auto value = gen_random(15);

	dnet_async_service_result write_result;
	dnet_async_service_result lookup_result;
	{
		auto &result = write_result;
		auto future = localnode.invoke<io::localnode::write>(key.raw_id(), groups, value, 0);
		BOOST_REQUIRE_EQUAL(future.valid(), true);
		BOOST_REQUIRE_NO_THROW(result = future.get());
		BOOST_CHECK_GT(result.file_info.size, 0);
		BOOST_CHECK_GT(result.file_path.size(), 0);
	}
	{
		auto &result = lookup_result;
		auto future = localnode.invoke<io::localnode::lookup>(key.raw_id(), groups);
		BOOST_REQUIRE_EQUAL(future.valid(), true);
		BOOST_REQUIRE_NO_THROW(result = future.get());
		BOOST_CHECK_GT(result.file_info.size, 0);
		BOOST_CHECK_GT(result.file_path.size(), 0);
	}

	BOOST_CHECK_EQUAL(write_result, lookup_result);

	ioremap::elliptics::data_pointer read_result;
	{
		auto &result = read_result;
		auto future = localnode.invoke<io::localnode::read>(key.raw_id(), groups, 0, 0);
		BOOST_REQUIRE_EQUAL(future.valid(), true);
		BOOST_REQUIRE_NO_THROW(result = future.get());
	}

	BOOST_CHECK_EQUAL(read_result.to_string(), value);
}

///
/// Place to list available tests.
///
bool register_tests(test_suite *suite, node n)
{
	// IMPORTANT: Any testcase that uses session object should be registered
	// with ELLIPTICS_TEST_CASE macro using session object as a second argument --
	// -- there is special variant of tests::make() in test_object.hpp,
	// which is specifically tailored for test cases with session.

	const std::string app = application_name();

	/// prerequisite: launch and init test application
	ELLIPTICS_TEST_CASE(upload_application, global_data->locator_port, app, global_data->directory.path());
	ELLIPTICS_TEST_CASE(start_application, global_data->locator_port, app);
	ELLIPTICS_TEST_CASE(init_application, create_session(n, { 1 }, 0, 0), app);

	/// various ways to send a reply to an `exec` command
	ELLIPTICS_TEST_CASE(test_echo_via_elliptics, create_session(n, { 1 }, 0, 0), app, "");
	ELLIPTICS_TEST_CASE(test_echo_via_elliptics, create_session(n, { 1 }, 0, 0), app, "some-data");
	ELLIPTICS_TEST_CASE(test_echo_via_elliptics, create_session(n, { 1 }, 0, 0), app, "some-data and long-data.. like this");
	ELLIPTICS_TEST_CASE(test_echo_via_cocaine, create_session(n, { 1 }, 0, 0), app, "");
	ELLIPTICS_TEST_CASE(test_echo_via_cocaine, create_session(n, { 1 }, 0, 0), app, "some-data");
	ELLIPTICS_TEST_CASE(test_echo_via_cocaine, create_session(n, { 1 }, 0, 0), app, "some-data and long-data.. like this");

	/// single `push` command does not expect reply at all
	ELLIPTICS_TEST_CASE(test_push, create_session(n, { 1 }, 0, 0), app, "");
	ELLIPTICS_TEST_CASE(test_push, create_session(n, { 1 }, 0, 0), app, "some-data");

	/// `exec`/`push` chains
	ELLIPTICS_TEST_CASE(test_chain_via_elliptics, create_session(n, { 1 }, 0, 0), app, "2-step-chain-via-elliptics", "some-data");
	ELLIPTICS_TEST_CASE(test_chain_via_elliptics, create_session(n, { 1 }, 0, 0), app, "3-step-chain-via-elliptics", "some-data");
	ELLIPTICS_TEST_CASE(test_chain_via_elliptics, create_session(n, { 1 }, 0, 0), app, "4-step-chain-via-elliptics", "some-data");

	/// timeout mechanics on `exec` commands (long test -- at least 30 seconds long)
	ELLIPTICS_TEST_CASE(test_timeout, create_session(n, { 1 }, 0, 0), app);

	/// continuous load handles properly
	//TODO: micro stress test similar to timeout test:
	// - send wast stream of commands, big enough to affect concurrency rate
	//   and number of spawned workers;
	// - wait for completion;
	// - check app info if load stat return to zero

	/// localnode service
	// * data structures
	ELLIPTICS_TEST_CASE_NOARGS(test_localnode_data_serialization);
	// * methods (first using matching group_id, then empty group list)
	ELLIPTICS_TEST_CASE(test_localnode, create_session(n, {1}, 0, 0), std::vector<int>{1});
	ELLIPTICS_TEST_CASE(test_localnode, create_session(n, {1}, 0, 0), std::vector<int>{});

	return true;
}

} // namespace tests

int main(int argc, char *argv[])
{
	atexit(tests::destroy_global_data);

	srand(time(0));
	return unit_test_main(tests::create_testsuite, argc, argv);
}
