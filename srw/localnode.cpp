/*
 * 2015+ Copyright (c) Ivan Chelyubeev <ivan.chelubeev@gmail.com>
 * 2014 Copyright (c) Asier Gutierrez <asierguti@gmail.com>
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

#include <sstream>

#include "elliptics/interface.h"
#include "elliptics.h"

#include <blackhole/v1/logger.hpp>
#include <blackhole/v1/attribute.hpp>
#include <cocaine/context.hpp>
#include <cocaine/rpc/actor.hpp> // for factory
#include <cocaine/logging.hpp>

#include "cocaine/idl/localnode.hpp"
#include "cocaine/traits/localnode.hpp"
#include "localnode.hpp"


namespace {

std::string to_string(const std::vector<int> &v)
{
    std::ostringstream ss;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0) {
            ss << ", ";
        }
        ss << v[i];
    }
    return ss.str();
}

}


namespace ioremap { namespace elliptics {

namespace ph = std::placeholders;

std::vector<int> find_local_groups(dnet_node *node)
{
	std::vector<int> result;

	for (rb_node *i = rb_first(&node->group_root); i != nullptr; i = rb_next(i)) {
		const dnet_group *group = rb_entry(i, dnet_group, group_entry);
		// take local groups only
		if (group->ids[0].idc->st == node->st) {
			result.push_back(group->group_id);
		}
	}

	return result;
}

localnode::localnode(
		cocaine::context_t& context,
		asio::io_service& reactor,
		const std::string& name,
		const cocaine::dynamic_t &args,
		dnet_node* node
)
	: cocaine::api::service_t(context, reactor, name, args)
	, cocaine::dispatch<io::localnode_tag>(name)
	, session_proto_(node)
	, log_(context.log(name))
{
	COCAINE_LOG_DEBUG(log_, "{}: ENTER", __func__);

	on<ioremap::elliptics::io::localnode::read>(std::bind(&localnode::read, this, ph::_1, ph::_2, ph::_3, ph::_4));
	on<io::localnode::write>(std::bind(&localnode::write, this, ph::_1, ph::_2, ph::_3, ph::_4));
	on<io::localnode::lookup>(std::bind(&localnode::lookup, this, ph::_1, ph::_2));

	// In the simplest case when node serves exactly one group, we want to free
	// client from the bother of providing group number: client will be allowed
	// to use an empty group list.
	{
		// We are forced to find all local groups anyway because there is no other
		// way to get the total number of the groups this node serves.
		const auto local_groups = find_local_groups(node);
		COCAINE_LOG_INFO(log_, "{}: found local groups: [{}]", __func__, to_string(local_groups).c_str());
		if (local_groups.size() == 1) {
			session_proto_.set_groups(local_groups);
		}
	}

	COCAINE_LOG_INFO(log_, "{}: service initialized", __func__);

	COCAINE_LOG_DEBUG(log_, "{}: EXIT", __func__);
}

inline void override_groups(session &s, const std::vector<int> &groups)
{
	// Empty group list is only meaningful if node serve a single group.
	// In that case, empty groups are just a way to say: "please execute
	// my command against whatever group you are serving".
	if (!groups.empty())  {
		s.set_groups(groups);
	}
}

deferred<data_pointer> localnode::read(const dnet_raw_id &key, const std::vector<int> &groups, uint64_t offset, uint64_t size)
{
	COCAINE_LOG_DEBUG(log_, "{}: ENTER", __func__);

	auto s = session_proto_.clone();
	s.set_exceptions_policy(session::no_exceptions);
	override_groups(s, groups);

	//XXX: NOLOCK flag should not be set here unconditionally,
	// as such it breaks generality of localnode interface;
	// localnode interface must evolve further to allow that kind of configurability;
	// but right now we badly need NOLOCK for reads (we know for sure
	// that in our usecase there are no updates to the existing resources
	// and its safe to perform a read without locking on a key)
	s.set_cflags(DNET_FLAGS_NOLOCK);

	deferred<data_pointer> promise;

	s.read_data(elliptics::key(key), offset, size).connect(
		std::bind(&localnode::on_read_completed, this, promise, ph::_1, ph::_2)
	);

	COCAINE_LOG_DEBUG(log_, "{}: EXIT", __func__);

	return promise;
}

deferred<dnet_async_service_result> localnode::write(const dnet_raw_id &key, const std::vector<int> &groups, const std::string &bytes, uint64_t offset)
{
	COCAINE_LOG_DEBUG(log_, "{}: ENTER", __func__);

	auto s = session_proto_.clone();
	s.set_exceptions_policy(session::no_exceptions);
	override_groups(s, groups);

	deferred<dnet_async_service_result> promise;

	s.write_data(elliptics::key(key), bytes, offset).connect(
		std::bind(&localnode::on_write_completed, this, promise, ph::_1, ph::_2)
	);

	COCAINE_LOG_DEBUG(log_, "{}: EXIT", __func__);

	return promise;
}

deferred<dnet_async_service_result> localnode::lookup(const dnet_raw_id &key, const std::vector<int> &groups)
{
	auto s = session_proto_.clone();
	s.set_exceptions_policy(session::no_exceptions);
	override_groups(s, groups);

	deferred<dnet_async_service_result> promise;

	s.lookup(elliptics::key(key)).connect(
		std::bind(&localnode::on_write_completed, this, promise, ph::_1, ph::_2)
	);

	return promise;
}

void localnode::on_read_completed(deferred<data_pointer> promise,
		const std::vector<read_result_entry> &result,
		const error_info &error)
{
	COCAINE_LOG_DEBUG(log_, "{}: ENTER", __func__);

	if (error) {
		COCAINE_LOG_ERROR(log_, "{}: return error {}, {}", __func__, error.code(), error.message());
		promise.abort(std::error_code(-error.code(), std::generic_category()), error.message());

	} else {
		COCAINE_LOG_DEBUG(log_, "{}: return success", __func__);
		promise.write(result[0].file());
	}

	COCAINE_LOG_DEBUG(log_, "{}: EXIT", __func__);
}

void localnode::on_write_completed(deferred<dnet_async_service_result> promise,
		const std::vector<lookup_result_entry> &results,
		const error_info &error)
{
	COCAINE_LOG_DEBUG(log_, "{}: ENTER", __func__);

	if (error) {
		COCAINE_LOG_ERROR(log_, "{}: return error {}, {}", __func__, error.code(), error.message());
		promise.abort(std::error_code(-error.code(), std::generic_category()), error.message());

	} else {
		dnet_async_service_result r;
		const auto &single_result = results[0];
		r.addr = *single_result.storage_address();
		r.file_info = *single_result.file_info();
		r.file_path = single_result.file_path();
		COCAINE_LOG_DEBUG(log_, "{}: return success", __func__);
		promise.write(r);
	}

	COCAINE_LOG_DEBUG(log_, "{}: EXIT", __func__);
}

}} // namespace ioremap::elliptics
