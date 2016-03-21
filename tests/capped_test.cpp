/*
 * 2008+ Copyright (c) Evgeniy Polyakov <zbr@ioremap.net>
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
#include <deque>

#define BOOST_TEST_NO_MAIN
#define BOOST_TEST_ALTERNATIVE_INIT_API
#include <boost/test/included/unit_test.hpp>

#include <boost/program_options.hpp>

using namespace ioremap::elliptics;
using namespace boost::unit_test;

namespace tests {

static nodes_data::ptr configure_test_setup(const std::vector<std::string> &remotes, const std::string &path)
{
#ifndef NO_SERVER
	if (remotes.empty()) {
		start_nodes_config start_config(results_reporter::get_stream(), std::vector<server_config>({
			server_config::default_value().apply_options(config_data()
				("indexes_shard_count", 1)
				("group", 5)
			)
		}), path);

		return start_nodes(start_config);

	} else
#endif // NO_SERVER
		return start_nodes(results_reporter::get_stream(), remotes, path);
}

static void test_capped_collection(session &sess, const std::string &collection_name)
{
	key collection = collection_name;
	sess.transform(collection);

	index_entry index(collection.raw_id(), data_pointer());

	std::deque<key> existing_objects;

	for (int i = 0; i < 10; ++i) {
		std::string object = "capped_obj_" + boost::lexical_cast<std::string>(i);
		std::string object_data = "capped_obj_data_" + boost::lexical_cast<std::string>(i);

		ELLIPTICS_REQUIRE(add_result, sess.add_to_capped_collection(object, index, 5, true));
		ELLIPTICS_REQUIRE(write_result, sess.write_data(object, object_data, 0));
		ELLIPTICS_REQUIRE(find_result, sess.find_any_indexes(std::vector<std::string>(1, collection_name)));
		ELLIPTICS_REQUIRE(test_read_result, sess.read_data(object, 0, 0));

		key id = object;
		sess.transform(id);
		existing_objects.push_back(id.id());

		sync_read_result test_read_result_sync = test_read_result;

		BOOST_REQUIRE_EQUAL(test_read_result_sync.size(), 1);
		BOOST_REQUIRE_EQUAL(object_data, test_read_result_sync[0].file().to_string());

		if (existing_objects.size() > 5) {
			ELLIPTICS_REQUIRE_ERROR(read_result, sess.read_data(existing_objects.front(), 0, 0), -ENOENT);

			existing_objects.pop_front();
		}

		sync_find_indexes_result results = find_result;
		BOOST_REQUIRE_EQUAL(existing_objects.size(), results.size());

		std::set<key> objects(existing_objects.begin(), existing_objects.end());

		for (size_t i = 0; i < results.size(); ++i) {
			const find_indexes_result_entry &entry = results[i];
			key id = entry.id;
			BOOST_REQUIRE(objects.find(id) != objects.end());
			objects.erase(id);
		}
	}
}

bool register_tests(const nodes_data *setup)
{
	auto n = setup->node->get_native();

	ELLIPTICS_TEST_CASE(test_capped_collection, use_session(n, {5}, 0, 0), "capped-collection");

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

#ifndef NO_SERVER
	if (vm.count("help")) {
#else
	if (vm.count("help") || remotes.empty()) {
#endif
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

