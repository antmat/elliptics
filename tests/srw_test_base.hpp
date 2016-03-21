#ifndef SRW_TEST_HPP
#define SRW_TEST_HPP

#include <msgpack.hpp>

#include "test_base.hpp"

#ifndef BOOST_REQUIRE_EQUAL
# define BOOST_REQUIRE_EQUAL(a, b) do { \
		if ((a) != (b)) { \
			std::stringstream error_stream; \
			error_stream << "error occured at function: " << __PRETTY_FUNCTION__ \
				<< ", line: " << __LINE__  << ": " << #a << " != " << #b \
				<< " (" << (a) << " != " << (b) << ")"; \
			throw std::runtime_error(error_stream.str()); \
		} \
	} while (false)

# undef ELLIPTICS_REQUIRE
# define ELLIPTICS_REQUIRE(result, command) \
	auto result = (command); \
	result.wait(); \
	result.error().throw_error();
#endif

namespace tests {

struct node_info
{
	std::string path;
	std::vector<std::string> remotes;
	std::vector<int> groups;

	void unpack(const std::string &data)
	{
		msgpack::unpacked msg;
		msgpack::unpack(&msg, data.c_str(), data.size());
		msgpack::object &obj = msg.get();

		if (obj.type != msgpack::type::ARRAY || obj.via.array.size != 3)
			throw msgpack::type_error();

		obj.via.array.ptr[0].convert(&remotes);
		obj.via.array.ptr[1].convert(&groups);
		obj.via.array.ptr[2].convert(&path);
	}

	std::string pack()
	{
		msgpack::sbuffer buffer;
		msgpack::packer<msgpack::sbuffer> packer(buffer);

		packer.pack_array(3);
		packer << remotes;
		packer << groups;
		packer << path;

		return std::string(buffer.data(), buffer.size());
	}
};

std::string application_name();
void start_application(int locator_port, const std::string &app_name);
void upload_application(int locator_port, const std::string &app_name, const std::string &path);

void init_application_impl(session &sess, const std::string &app_name, const nodes_data *setup);

} // namespace tests

#endif // SRW_TEST_HPP
