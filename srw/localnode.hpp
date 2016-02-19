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

#ifndef LOCALNODE_SERVICE_HPP
#define LOCALNODE_SERVICE_HPP

#include <cocaine/api/service.hpp>
#include <cocaine/rpc/dispatch.hpp>

#include "elliptics/session.hpp"
#include "cocaine/idl/localnode.hpp"

namespace ioremap { namespace elliptics {

using cocaine::deferred;

//
// Service implementation object.
// This is the actual service working inside cocaine runtime.
//
class localnode : public cocaine::api::service_t, public cocaine::dispatch<io::localnode_tag>
{
private:
	session session_proto_;
	std::shared_ptr<cocaine::logging::logger_t> log_;

public:
	localnode(cocaine::context_t &context, asio::io_service &reactor, const std::string &name, const cocaine::dynamic_t &args, dnet_node *node);

	// service_t interface
	auto
	prototype() const -> const cocaine::io::basic_dispatch_t& {
		return *this;
	}

private:
	deferred<data_pointer> read(const dnet_raw_id &key, const std::vector<int> &groups, uint64_t offset, uint64_t size);
	deferred<dnet_async_service_result> lookup(const dnet_raw_id &key, const std::vector<int> &groups);
	deferred<dnet_async_service_result> write(const dnet_raw_id &key, const std::vector<int> &groups, const std::string &bytes, uint64_t offset);

	void on_read_completed(deferred<data_pointer> promise,
		const std::vector<ioremap::elliptics::read_result_entry> &result,
		const ioremap::elliptics::error_info &error
	);
	void on_write_completed(deferred<dnet_async_service_result> promise,
		const std::vector<ioremap::elliptics::lookup_result_entry> &result,
		const ioremap::elliptics::error_info &error
	);
};

}} // namespace ioremap::elliptics

#endif // LOCALNODE_SERVICE_HPP
