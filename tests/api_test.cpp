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

#include "test_base.hpp"
#include <algorithm>

#define BOOST_TEST_NO_MAIN
#define BOOST_TEST_ALTERNATIVE_INIT_API
#include <boost/test/included/unit_test.hpp>

#include <boost/program_options.hpp>

using namespace ioremap::elliptics;
using namespace boost::unit_test;

namespace tests {

static nodes_data::ptr configure_test_setup(const std::vector<std::string> &remotes, const std::string &path)
{
	if (remotes.empty()) {
		start_nodes_config start_config(results_reporter::get_stream(), std::vector<server_config>({
			server_config::default_value().apply_options(config_data()
				("group", 2)
			)
		}), path);

		return start_nodes(start_config);

	} else {
		return start_nodes(results_reporter::get_stream(), remotes, path);
	}
}

static bool test_error_check_exception(const not_found_error &e)
{
	return e.error_message()
			.find("entry::io_attribute(): data.size is too small, expected: 208, actual: 0, status: -2")
			!= std::string::npos;
}

static void test_error_message(session &s, const std::string &id, int err)
{
	s.set_filter(filters::all);

	ELLIPTICS_REQUIRE_ERROR(read_result, s.read_data(id, 0, 0), err);

	sync_read_result sync_result = read_result.get();

	BOOST_REQUIRE(sync_result.size() > 0);

	read_result_entry entry = sync_result[0];

	BOOST_REQUIRE_EXCEPTION(entry.io_attribute(), not_found_error, test_error_check_exception);
}

static bool test_error_null_message_check_exception_1(const not_found_error &e)
{
	return e.error_message()
			.find("entry::command(): entry is null")
			!= std::string::npos;
}

static bool test_error_null_message_check_exception_2(const not_found_error &e)
{
	return e.error_message()
			.find("entry::io_attribute(): entry is null")
			!= std::string::npos;
}

static void test_error_null_message()
{
	read_result_entry entry;
	BOOST_REQUIRE_EXCEPTION(entry.command(), not_found_error, test_error_null_message_check_exception_1);
	BOOST_REQUIRE_EXCEPTION(entry.io_attribute(), not_found_error, test_error_null_message_check_exception_2);
}

static void test_data_buffer()
{
	const std::string str = "some long or not very long string";

	data_buffer buffer;
	buffer.write(str.c_str(), str.size());

	BOOST_REQUIRE_EQUAL(buffer.size(), str.size());

	data_pointer data = std::move(buffer);
	BOOST_REQUIRE_EQUAL(data.size(), str.size());
	BOOST_REQUIRE_EQUAL(data.to_string(), str);


	buffer.write(str.c_str(), str.size());
	BOOST_REQUIRE_EQUAL(buffer.size(), str.size());
	buffer.write(str.c_str(), str.size());
	BOOST_REQUIRE_EQUAL(buffer.size(), 2 * str.size());

	data_pointer data2 = std::move(buffer);
	BOOST_REQUIRE_EQUAL(data2.size(), 2 * str.size());
	BOOST_REQUIRE_EQUAL(data2.to_string(), str + str);
}

bool register_tests(const nodes_data *setup)
{
	auto n = setup->node->get_native();

	ELLIPTICS_TEST_CASE(test_error_message, use_session(n, {2}, 0, 0), "non-existent-key", -ENOENT);
	ELLIPTICS_TEST_CASE_NOARGS(test_error_null_message);
	ELLIPTICS_TEST_CASE_NOARGS(test_data_buffer);

	return true;
}

nodes_data::ptr configure_test_setup_from_args(int argc, char *argv[])
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

	return configure_test_setup(remotes, path);
}

}

//
// Common test initialization routine.
//
using namespace tests;
using namespace boost::unit_test;

//FIXME: forced to use global variable and plain function wrapper
// because of the way how init_test_main works in boost.test,
// introducing a global fixture would be a proper way to handle
// global test setup
namespace {

std::shared_ptr<nodes_data> setup;

bool init_func()
{
	return register_tests(setup.get());
}

}

int main(int argc, char *argv[])
{
	srand(time(nullptr));

	// we own our test setup
	setup = configure_test_setup_from_args(argc, argv);

	int result = unit_test_main(init_func, argc, argv);

	// disassemble setup explicitly, to be sure about where its lifetime ends
	setup.reset();

	return result;
}
