#include "../../include/elliptics/cppdef.h"

namespace ioremap { namespace elliptics {

template <typename T>
class async_result<T>::data
{
	public:
		data() : total(0), finished(false) {}

		std::mutex lock;
		std::condition_variable condition;

		async_result<T>::result_function result_handler;
		async_result<T>::final_function final_handler;

		result_filter filter;
		result_checker checker;
		uint32_t policy;

		std::vector<T> results;
		error_info error;

		std::vector<dnet_cmd> statuses;
		size_t total;

		bool finished;
};

template <typename T>
async_result<T>::async_result(const session &sess) : m_data(std::make_shared<data>())
{
	m_data->filter = sess.get_filter();
	m_data->checker = sess.get_checker();
	m_data->policy = sess.get_exceptions_policy();
}

template <typename T>
async_result<T>::async_result(async_result &&other)
{
	std::swap(other.m_data, m_data);
}

template <typename T>
async_result<T>::~async_result()
{
}

template <typename T>
void async_result<T>::connect(const result_function &result_handler, const final_function &final_handler)
{
	std::unique_lock<std::mutex> locker(m_data->lock);
	if (result_handler) {
		m_data->result_handler = result_handler;
		if (!m_data->results.empty()) {
			for (auto it = m_data->results.begin(), end = m_data->results.end(); it != end; ++it) {
				result_handler(*it);
			}
		}
	}
	if (final_handler) {
		m_data->final_handler = final_handler;
		if (m_data->finished)
			final_handler(m_data->error);
	}
}

template <typename T>
void async_result<T>::connect(const result_array_function &handler)
{
	auto keeper = std::make_shared<data_keeper>();
	keeper->data_ptr = m_data;
	connect(result_function(), std::bind(aggregator_final_handler, keeper, handler));
}

template <typename T>
void async_result<T>::connect(const async_result_handler<T> &handler)
{
	connect(std::bind(handler_process, handler, std::placeholders::_1),
		std::bind(handler_complete, handler, std::placeholders::_1));
}

template <typename T>
void async_result<T>::wait()
{
	wait(session::throw_at_wait);
}

template <typename T>
error_info async_result<T>::error() const
{
	return m_data->error;
}

template <typename T>
std::vector<T> async_result<T>::get()
{
	wait(session::throw_at_get);
	return m_data->results;
}

template <typename T>
bool async_result<T>::get(T &entry)
{
	wait(session::throw_at_get);
	for (auto it = m_data->results.begin(); it != m_data->results.end(); ++it) {
		if (it->status() == 0 && !it->data().empty()) {
			entry = *it;
			return true;
		}
	}
	return false;
}

template <typename T>
T async_result<T>::get_one()
{
	T result;
	get(result);
	return result;
}

template <typename T>
async_result<T>::operator std::vector<T> ()
{
	return get();
}

template <typename T>
class async_result<T>::iterator::data
{
	public:
		std::mutex mutex;
		std::condition_variable condition;
		std::queue<T> results;
		uint32_t policy;
		bool finished;
		error_info error;
};

template <typename T>
async_result<T>::iterator::iterator() : m_state(data_at_end)
{
}

template <typename T>
async_result<T>::iterator::iterator(async_result &result) : d(std::make_shared<data>()), m_state(data_waiting)
{
	d->finished = false;
	d->policy = result.m_data->policy;
	result.connect(std::bind(process, d, std::placeholders::_1),
	std::bind(complete, d, std::placeholders::_1));
}

template <typename T>
async_result<T>::iterator::iterator(const iterator &other) : d(other.d)
{
	other.ensure_data();
	m_state = other.m_state;
	m_result = other.m_result;
}

template <typename T>
async_result<T>::iterator::~iterator()
{
}

template <typename T>
typename async_result<T>::iterator &async_result<T>::iterator::operator =(const iterator &other)
{
	other.ensure_data();
	m_state = other.m_state;
	m_result = other.m_result;
	return *this;
}

template <typename T>
bool async_result<T>::iterator::operator ==(const iterator &other) const
{
	return at_end() == other.at_end();
}

template <typename T>
bool async_result<T>::iterator::operator !=(const iterator &other) const
{
	return !operator ==(other);
}

template <typename T>
T async_result<T>::iterator::operator *() const
{
	ensure_data();
	if (m_state == data_at_end) {
		throw_error(-ENOENT, "async_result::iterator::operator *(): end iterator");
	}
	return m_result;
}

template <typename T>
T *async_result<T>::iterator::operator ->() const
{
	ensure_data();
	if (m_state == data_at_end) {
		throw_error(-ENOENT, "async_result::iterator::operator ->(): end iterator");
	}
	return &m_result;
}

template <typename T>
typename async_result<T>::iterator &async_result<T>::iterator::operator ++()
{
	ensure_data();
	if (m_state == data_at_end) {
		throw_error(-ENOENT, "async_result::iterator::operator ++(): end iterator");
	}
	m_state = data_waiting;
	ensure_data();
	return *this;
}

template <typename T>
typename async_result<T>::iterator async_result<T>::iterator::operator ++(int)
{
	ensure_data();
	iterator tmp = *this;
	++(*this);
	return tmp;
}

template <typename T>
bool async_result<T>::iterator::at_end() const
{
	ensure_data();
	return m_state == data_at_end;
}

template <typename T>
void async_result<T>::iterator::ensure_data() const
{
	if (m_state == data_waiting) {
		std::unique_lock<std::mutex> locker(d->mutex);
		while (!d->finished && d->results.empty())
			d->condition.wait(locker);

		if (d->results.empty()) {
			m_state = data_at_end;
			if (d->policy & session::throw_at_iterator_end)
				d->error.throw_error();
		} else {
			m_state = data_ready;
			m_result = d->results.front();
			d->results.pop();
		}
	}
}

template <typename T>
void async_result<T>::iterator::process(const std::weak_ptr<data> &weak_data, const T &result)
{
	if (std::shared_ptr<data> d = weak_data.lock()) {
		std::unique_lock<std::mutex> locker(d->mutex);
		d->results.push(result);
		d->condition.notify_all();
	}
}

template <typename T>
void async_result<T>::iterator::complete(const std::weak_ptr<data> &weak_data, const error_info &error)
{
	if (std::shared_ptr<data> d = weak_data.lock()) {
		std::unique_lock<std::mutex> locker(d->mutex);
		d->finished = true;
		d->error = error;
		d->condition.notify_all();
	}
}

template <typename T>
struct async_result<T>::data_keeper
{
	typedef std::shared_ptr<data_keeper> ptr;

	std::shared_ptr<data> data_ptr;
};

template <typename T>
void async_result<T>::wait(uint32_t policy)
{
	std::unique_lock<std::mutex> locker(m_data->lock);
	while (!m_data->finished)
		m_data->condition.wait(locker);
	if (m_data->policy & policy)
		m_data->error.throw_error();
}

template <typename T>
void async_result<T>::aggregator_final_handler(const std::shared_ptr<data_keeper> &keeper, const result_array_function &handler)
{
	std::shared_ptr<data> d;
	std::swap(d, keeper->data_ptr);
	handler(d->results, d->error);
}

template <typename T>
void async_result<T>::handler_process(async_result_handler<T> handler, const T &result)
{
	handler.process(result);
}

template <typename T>
void async_result<T>::handler_complete(async_result_handler<T> handler, const error_info &error)
{
	handler.complete(error);
}

template <typename T>
async_result_handler<T>::async_result_handler(const async_result<T> &result)
	: m_data(result.m_data)
{
}

template <typename T>
async_result_handler<T>::async_result_handler(const async_result_handler &other)
	: m_data(other.m_data)
{
}

template <typename T>
async_result_handler<T>::~async_result_handler()
{
}

template <typename T>
async_result_handler<T> &async_result_handler<T>::operator =(const async_result_handler &other)
{
	m_data = other.m_data;
	return *this;
}

template <typename T>
void async_result_handler<T>::set_total(size_t total)
{
	m_data->total = total;
}

template <typename T>
size_t async_result_handler<T>::get_total()
{
	return m_data->total;
}

template <typename T>
void async_result_handler<T>::process(const T &result)
{
	std::unique_lock<std::mutex> locker(m_data->lock);
	const dnet_cmd *cmd = result.command();
	if (!(cmd->flags & DNET_FLAGS_MORE))
		m_data->statuses.push_back(*cmd);
	if (!m_data->filter(result))
		return;
	if (m_data->result_handler) {
		m_data->result_handler(result);
	} else {
		m_data->results.push_back(result);
	}
}

template <typename T>
void async_result_handler<T>::complete(const error_info &error)
{
	std::unique_lock<std::mutex> locker(m_data->lock);
	m_data->finished = true;
	m_data->error = error;
	if (!error)
		check(&m_data->error);
	if (m_data->final_handler) {
		m_data->final_handler(error);
	}
	m_data->condition.notify_all();
}

template <typename T>
bool async_result_handler<T>::check(error_info *error)
{
	if (!m_data->checker(m_data->statuses, m_data->total)) {
		if (error) {
			size_t success = 0;
			dnet_cmd command;
			command.status = 0;
			for (auto it = m_data->statuses.begin(); it != m_data->statuses.end(); ++it) {
				if (it->status == 0) {
					++success;
				} else if (command.status == 0) {
					command = *it;
				}
			}
			if (success == 0) {
				if (command.status) {
					*error = create_error(command);
				} else {
					*error = create_error(-ENXIO, "insufficiant results count due to checker: "
							"%zu of %zu (%zu)",
						success, m_data->total, m_data->statuses.size());
				}
			}
		}
		return false;
	}
	if (error)
		*error = error_info();
	return true;
}

template class async_result<callback_result_entry>;
template class async_result<read_result_entry>;
template class async_result<lookup_result_entry>;
template class async_result<stat_result_entry>;
template class async_result<stat_count_result_entry>;
template class async_result<exec_result_entry>;
template class async_result<iterator_result_entry>;

template class async_result_handler<callback_result_entry>;
template class async_result_handler<read_result_entry>;
template class async_result_handler<lookup_result_entry>;
template class async_result_handler<stat_result_entry>;
template class async_result_handler<stat_count_result_entry>;
template class async_result_handler<exec_result_entry>;
template class async_result_handler<iterator_result_entry>;
} }