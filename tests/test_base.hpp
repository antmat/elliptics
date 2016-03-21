#ifndef TEST_BASE_HPP
#define TEST_BASE_HPP

#include <elliptics/session.hpp>
#include <boost/variant.hpp>

#ifndef TEST_DO_NOT_INCLUDE_PLACEHOLDERS
# include <boost/bind/placeholders.hpp>
#endif

namespace tests {

using namespace ioremap::elliptics;

#define ELLIPTICS_CHECK_IMPL(R, C, CMD) \
	auto R = (C); \
	R.wait(); \
	{ \
		auto base_message = BOOST_TEST_STRINGIZE(C); \
		std::string message(base_message.begin(), base_message.end()); \
		message += ", err: \""; \
		message += R.error().message(); \
		message += "\""; \
		CMD(!R.error(), message); \
	}

#define ELLIPTICS_CHECK_ERROR_IMPL(R, C, E, CMD) \
	auto R = (C); \
	R.wait(); \
	if (R.error().code() != (E)) { \
		auto base_message = BOOST_TEST_STRINGIZE(C); \
		std::stringstream out; \
		out << std::string(base_message.begin(), base_message.end()) \
		<< ", expected error: " << (E) << ", received: \"" << R.error().message() << "\""; \
		CMD(false, out.str()); \
	}

#define ELLIPTICS_COMPARE_REQUIRE(R, C, D) ELLIPTICS_REQUIRE(R, C); \
	do { \
		auto R ## _result = (R).get_one(); \
		BOOST_REQUIRE_EQUAL((R ## _result).file().to_string(), (D)); \
	} while (0)

#define ELLIPTICS_WARN(R, C) ELLIPTICS_CHECK_IMPL(R, (C), BOOST_WARN_MESSAGE)
#define ELLIPTICS_CHECK(R, C) ELLIPTICS_CHECK_IMPL(R, (C), BOOST_CHECK_MESSAGE)
#define ELLIPTICS_REQUIRE(R, C) ELLIPTICS_CHECK_IMPL(R, (C), BOOST_REQUIRE_MESSAGE)

#define ELLIPTICS_WARN_ERROR(R, C, E) ELLIPTICS_CHECK_ERROR_IMPL(R, (C), (E), BOOST_WARN_MESSAGE)
#define ELLIPTICS_CHECK_ERROR(R, C, E) ELLIPTICS_CHECK_ERROR_IMPL(R, (C), (E), BOOST_CHECK_MESSAGE)
#define ELLIPTICS_REQUIRE_ERROR(R, C, E) ELLIPTICS_CHECK_ERROR_IMPL(R, (C), (E), BOOST_REQUIRE_MESSAGE)


//
// test_wrapper_with_session hold and report test names
// and also acts as kind of client session fixture.
//
//XXX: consider dropping all this and make a use of Boost.Test auto macros
// as well as a use of fixtures; or, even better, move to gtest,
// then all this stuff with test names construction will be unnecessary
//

typedef std::tuple<dnet_node *, std::vector<int>, uint64_t, uint32_t> session_create_args;

// special kind of wrapper for tests which require client session
struct test_wrapper_with_session
{
	std::string test_name;
	session_create_args session_args;
	std::function<void (session &client)> test_body;

	void operator() () const;
};

// Special test maker for tests which require client session.
// Note that args needed to create client session must come second to test method.
template <typename Method, typename... Args>
std::function<void ()> make(const char *test_name, Method method, session_create_args session_args, Args... args)
{
	namespace ph = std::placeholders;

	return test_wrapper_with_session{
		test_name,
		session_args,
		std::bind(method, ph::_1, std::forward<Args>(args)...)
	};
}

// General test maker for tests which do not use client session.
template <typename Method, typename... Args>
std::function<void ()> make(const char *, Method method, Args... args)
{
	return std::bind(method, std::forward<Args>(args)...);
}

static inline
session_create_args use_session(dnet_node *n, std::initializer_list<int> groups, uint64_t cflags = 0, uint32_t ioflags = 0)
{
	return session_create_args{n, groups, cflags, ioflags};
}

#define ELLIPTICS_MAKE_TEST(...) \
	boost::unit_test::make_test_case(tests::make(BOOST_STRINGIZE((__VA_ARGS__)), __VA_ARGS__), BOOST_TEST_STRINGIZE((__VA_ARGS__)))

#ifndef USE_CUSTOM_SUITE
#  define ELLIPTICS_TEST_CASE(M, C...) do { boost::unit_test::framework::master_test_suite().add(ELLIPTICS_MAKE_TEST(M, ##C )); } while (false)
#  define ELLIPTICS_TEST_CASE_NOARGS(M) do { boost::unit_test::framework::master_test_suite().add(ELLIPTICS_MAKE_TEST(M)); } while (false)
#else
#  define ELLIPTICS_TEST_CASE(M, C...) do { suite->add(ELLIPTICS_MAKE_TEST(M, ##C )); } while (false)
#  define ELLIPTICS_TEST_CASE_NOARGS(M) do { suite->add(ELLIPTICS_MAKE_TEST(M)); } while (false)
#endif

class directory_handler
{
public:
	directory_handler();
	directory_handler(const std::string &path, bool remove);
	directory_handler(directory_handler &&other);
	~directory_handler();

	directory_handler &operator= (directory_handler &&other);

	directory_handler(const directory_handler &) = delete;
	directory_handler &operator =(const directory_handler &) = delete;

	std::string path() const;

private:
	std::string m_path;
	bool m_remove;
};

void create_directory(const std::string &path);

#ifndef NO_SERVER

class server_config;

class config_data
{
protected:
	typedef boost::variant<std::vector<std::string>, std::string, bool, int64_t, config_data> variant;
	typedef std::vector<std::pair<std::string, variant> > container_t;

public:
	config_data &operator() (const std::string &name, const std::vector<std::string> &value);
	config_data &operator() (const std::string &name, const std::string &value);
	config_data &operator() (const std::string &name, const char *value);
	config_data &operator() (const std::string &name, int64_t value);
	config_data &operator() (const std::string &name, int value);
	config_data &operator() (const std::string &name, bool value);
	config_data &operator() (const std::string &name, const config_data &value);

	bool has_value(const std::string &name) const;
	std::string string_value(const std::string &name) const;

	typedef container_t::const_iterator const_iterator;
	const_iterator cbegin() const;
	const_iterator cend() const;

protected:
	config_data &operator() (const std::string &name, const variant &value);
	const variant *value_impl(const std::string &name) const;

	container_t m_data;
	friend class server_config;
};

class server_config
{
public:
	static server_config default_srw_value();
	static server_config default_value();

	void write(const std::string &path);
	server_config &apply_options(const config_data &data);

	config_data options;
	std::vector<config_data> backends;
	std::string log_path;
};

class server_node
{
public:
	server_node();
	server_node(const std::string &path, const server_config &config, const address &remote, int monitor_port, int locator_port, bool fork);
	server_node(server_node &&other);

	server_node &operator =(server_node &&other);

	server_node(const server_node &other) = delete;
	server_node &operator =(const server_node &other) = delete;

	~server_node();

	void start();
	void stop();
	void wait_to_stop();
	bool is_started() const;
	bool is_stopped() const;

	std::string config_path() const;
	server_config config() const;

	address remote() const;
	int monitor_port() const;
	int locator_port() const;
	pid_t pid() const;
	dnet_node *get_native() const;

private:
	dnet_node *m_node;
	std::string m_path;
	server_config m_config;
	address m_remote;
	int m_monitor_port;
	int m_locator_port;
	bool m_fork;
	bool m_kill_sent;
	mutable pid_t m_pid;
};

#endif // NO_SERVER

struct nodes_data
{
	typedef std::shared_ptr<nodes_data> ptr;

	~nodes_data();

	directory_handler run_directory;
	directory_handler directory;
#ifndef NO_SERVER
	std::vector<server_node> nodes;
#endif // NO_SERVER

	std::unique_ptr<logger_base> logger;
	std::unique_ptr<ioremap::elliptics::node> node;
};

#ifndef NO_SERVER

struct start_nodes_config {
	std::ostream &debug_stream;
	std::vector<server_config> configs;
	std::string path;
	bool fork;
	bool monitor;
	bool srw;
	bool isolated;
	int client_node_flags;
	int client_wait_timeout;
	int client_check_timeout;
	int client_stall_count;

	start_nodes_config(std::ostream &debug_stream, const std::vector<server_config> &&configs, const std::string &path);
};

nodes_data::ptr start_nodes(start_nodes_config &config);

#endif // NO_SERVER

nodes_data::ptr start_nodes(std::ostream &debug_stream, const std::vector<std::string> &remotes, const std::string &path);

std::string read_file(const char *file_path);

} // namespace tests

#endif // TEST_BASE_HPP
