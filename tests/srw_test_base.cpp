/*
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

//XXX: in order to give preference to the old foreign/blackhole logger
// elliptics' includes must before cocaine's includes

#include "library/elliptics.h"
#include "elliptics/session.hpp"

#include <cocaine/framework/manager.hpp>
#include <cocaine/framework/service.hpp>
#include <cocaine/idl/storage.hpp>
#include <cocaine/idl/node.hpp>

#include "srw_test_base.hpp"

namespace {

tests::node_info node_info_create(tests::nodes_data &data, const std::vector<int> &groups)
{
    tests::node_info info;

    info.groups = groups;
    info.path = data.directory.path();
    for (auto it = data.nodes.begin(); it != data.nodes.end(); ++it) {
        info.remotes.push_back(it->remote().to_string_with_family());
    }

    return info;
}

}

namespace tests {

std::string application_name()
{
    return "dnet_cpp_srw_test_app";
}

void start_application(int locator_port, const std::string &app_name)
{
    using namespace cocaine::framework;

    // Here we use naming conventions:
    //  1. that `node::v2` is available under the name `node`
    //  2. app's profile is stored under name of the app
    service_manager_t::endpoint_type endpoint(boost::asio::ip::address_v4::loopback(), locator_port);
    service_manager_t manager({endpoint}, 1);

    try {
        auto node = manager.create<cocaine::io::node_tag>("node");
        node.invoke<cocaine::io::node::start_app>(app_name, app_name).get();
    } catch(const std::exception &e) {
        throw std::runtime_error(std::string("Failed to start application: ") + e.what());
    }
}

void upload_application(int locator_port, const std::string &app_name, const std::string &path)
{
    using namespace cocaine::framework;

    service_manager_t::endpoint_type endpoint(boost::asio::ip::address_v4::loopback(), locator_port);
    service_manager_t manager({endpoint}, 1);
    auto storage = manager.create<cocaine::io::storage_tag>("storage");

    const std::vector<std::string> app_tags = {
        "apps"
    };
    const std::vector<std::string> profile_tags = {
        "profiles"
    };

    msgpack::sbuffer buffer;
    {
        msgpack::packer<msgpack::sbuffer> packer(buffer);
        packer.pack_map(2);
        packer << std::string("log-output");
        packer << true;
        packer << std::string("isolate");
        packer.pack_map(2);
        packer << std::string("type");
        packer << std::string("process");
        packer << std::string("args");
        packer.pack_map(4);
        packer << std::string("spool");
        packer << path;
        // increase termination timeout to stop cocaine engine
        // from killing our long-standing transactions, which are
        // used for timeout test
        //
        // timeout test starts several exec transactions with random timeouts
        // which end up in the noreply@ callback which just sleeps for 60 seconds
        // this forces elliptics client-side to timeout, which must be correlated
        // with timeouts (+2 seconds max) set for each transactions, i.e.
        // transactions with 7 seconds timeout must be timed out at most in 7+2 seconds
        packer << std::string("termination-timeout");
        packer << 60;
        packer << std::string("heartbeat-timeout");
        packer << 60;
        packer << std::string("startup-timeout");
        packer << 60;
    }
    std::string profile(buffer.data(), buffer.size());

    {
        buffer.clear();
        msgpack::packer<msgpack::sbuffer> packer(buffer);
        packer.pack_map(2);
        packer << std::string("type");
        packer << std::string("binary");
        packer << std::string("slave");
        packer << app_name;
    }
    std::string manifest(buffer.data(), buffer.size());
    {
        buffer.clear();
        msgpack::packer<msgpack::sbuffer> packer(buffer);
        const char *cocaine_app = getenv("TEST_COCAINE_APP");
        if (!cocaine_app)
            throw std::runtime_error("TEST_COCAINE_APP environment variable is no set");
        packer << read_file(cocaine_app);
    }
    std::string app(buffer.data(), buffer.size());

    auto results = when_all(
        storage.invoke<cocaine::io::storage::write>("manifests", app_name, manifest, app_tags),
        storage.invoke<cocaine::io::storage::write>("profiles", app_name, profile, profile_tags),
        storage.invoke<cocaine::io::storage::write>("apps", app_name, app, profile_tags)
    ).get();

    try {
        std::get<0>(results).get();
        std::get<1>(results).get();
        std::get<2>(results).get();
    } catch(const std::exception &e) {
        throw std::runtime_error(std::string("Failed to upload application: ") + e.what());
    }
}

void init_application_impl(session &sess, const std::string &app_name, nodes_data &data)
{
    sess.set_timeout(600);
    dnet_log_only_log(&sess.get_logger(), DNET_LOG_INFO, "Sending @init");

    node_info info = node_info_create(data, sess.get_groups());

    ELLIPTICS_REQUIRE(exec_result, sess.exec(NULL, app_name + "@init", info.pack()));

    sync_exec_result result = exec_result;
    BOOST_REQUIRE_EQUAL(result.size(), data.nodes.size());
    for (auto it = result.begin(); it != result.end(); ++it)
        BOOST_REQUIRE_EQUAL(it->context().data().to_string(), "inited");
}

} // namespace tests
