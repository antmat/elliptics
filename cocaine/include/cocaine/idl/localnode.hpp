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

#ifndef LOCALNODE_SERVICE_INTERFACE_HPP
#define LOCALNODE_SERVICE_INTERFACE_HPP

#include <cocaine/rpc/protocol.hpp>
#include <elliptics/packet.h>
#include <elliptics/utils.hpp>

namespace ioremap { namespace elliptics {

struct dnet_async_service_result {
	dnet_addr addr;
	dnet_file_info file_info;
	std::string file_path;
};

namespace io {

struct localnode_tag;

struct localnode {

	struct read {
		typedef localnode_tag tag;

		static const char* alias() { return "read"; }

		typedef boost::mpl::list<
			/// Elliptics id to read from
			dnet_raw_id,
			/// Reading group list
			std::vector<int>,
			/// Offset
			uint64_t,
			/// Size
			uint64_t
		> argument_type;

		typedef cocaine::io::option_of<
			/// Raw bytes of read result
			data_pointer
		>::tag upstream_type;
	};

	struct write {
		typedef localnode_tag tag;

		static const char* alias() { return "write"; }

		typedef boost::mpl::list<
			/// Elliptics id to write to
			dnet_raw_id,
			/// Writing group list
			std::vector<int>,
			/// Raw bytes of the value
			std::string,
			/// Offset
			uint64_t
		> argument_type;

		typedef cocaine::io::option_of<
			/// Info about stored key
			dnet_async_service_result
		>::tag upstream_type;
	};

	struct lookup {
		typedef localnode_tag tag;

		static const char* alias() { return "lookup"; }

		typedef boost::mpl::list<
			/// Elliptics id
			dnet_raw_id,
			/// Reading group list
			std::vector<int>
		> argument_type;

		typedef cocaine::io::option_of<
			/// Info about stored key
			dnet_async_service_result
		>::tag upstream_type;
	};
};

}}} // namespace ioremap::elliptics::io


namespace cocaine { namespace io {

template<>
struct protocol<ioremap::elliptics::io::localnode_tag> {
	typedef boost::mpl::int_<
		1
	>::type version;

	typedef boost::mpl::list<
		ioremap::elliptics::io::localnode::read,
		ioremap::elliptics::io::localnode::write,
		ioremap::elliptics::io::localnode::lookup
	> messages;

	typedef ioremap::elliptics::io::localnode scope;
};

}} // namespace cocaine::io


#endif // LOCALNODE_SERVICE_INTERFACE_HPP
