#include <libpolly.h>
#include <libspy.h>
#include <libusby.hpp>
#include <libpolly.hpp>

#include <iostream>
#include <vector>
#include <string>
#include <list>
#include <deque>

#include <boost/variant.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/weak_ptr.hpp>
#include <boost/assert.hpp>
#include <boost/function.hpp>
#include <boost/bind.hpp>

#include <libyb/packet.hpp>
#include <libyb/descriptor.hpp>
#include <libyb/vector_ref.hpp>

#include <windows.h>

struct connect_params
{
	enum kind_t { ck_serial_port, ck_usb } kind;
	boost::variant<std::string, boost::shared_ptr<libusby_device_handle> > params;

	libusby_device * get_dev() const
	{
		return libusby_get_device(boost::get<boost::shared_ptr<libusby_device_handle> >(params).get());
	}
};

struct connect_params_info
{
	connect_params params;
	std::string description;
};

struct async_context_impl
{
	virtual void cancel() = 0;
	virtual void wait() = 0;
};

struct event_async_context
	: async_context_impl
{
	event_async_context(libpolly_context * ctx)
		: ev(ctx)
	{
	}

	virtual void cancel() {}
	virtual void wait()
	{
		ev.wait();
	}

	void complete()
	{
		ev.set();
	}

	libpolly::event ev;
};

class async_context
{
public:
	async_context()
	{
	}

	async_context(boost::weak_ptr<async_context_impl> const & pimpl)
		: m_pimpl(pimpl)
	{
	}

	~async_context()
	{
		this->cancel_and_wait();
	}

	async_context(async_context && rhs)
		: m_pimpl(std::move(rhs.m_pimpl))
	{
		rhs.m_pimpl.reset();
	}

	async_context & operator=(boost::weak_ptr<async_context_impl> const & pimpl)
	{
		this->cancel_and_wait();
		m_pimpl = pimpl;
		return *this;
	}

	async_context & operator=(boost::weak_ptr<async_context_impl> && pimpl)
	{
		this->cancel_and_wait();
		m_pimpl = std::move(pimpl);
		return *this;
	}

	async_context & operator=(async_context && rhs)
	{
		this->cancel_and_wait();
		m_pimpl = std::move(rhs.m_pimpl);
		rhs.m_pimpl.reset();
		return *this;
	}

	void detach()
	{
		m_pimpl.reset();
	}

	bool empty() const
	{
		return m_pimpl._empty();
	}

	void cancel()
	{
		if (boost::shared_ptr<async_context_impl> pimpl = m_pimpl.lock())
			pimpl->cancel();
	}

	void wait()
	{
		if (boost::shared_ptr<async_context_impl> pimpl = m_pimpl.lock())
			pimpl->wait();
	}

	void cancel_and_wait()
	{
		if (boost::shared_ptr<async_context_impl> pimpl = m_pimpl.lock())
		{
			pimpl->cancel();
			pimpl->wait();
		}
	}

private:
	boost::weak_ptr<async_context_impl> m_pimpl;

	async_context(async_context const &);
	async_context & operator=(async_context const &);
};

typedef boost::weak_ptr<async_context_impl> detached_async_context;

class stream
{
public:
	virtual libpolly_context * polly() const = 0;
	virtual detached_async_context read_async(uint8_t * buf, size_t len, boost::function<void(size_t)> const & handler) = 0;
	virtual detached_async_context write_async(uint8_t const * buf, size_t len, boost::function<void(size_t)> const & handler) = 0;

	size_t read(uint8_t * buf, size_t len)
	{
		size_t res;
		libpolly::event e(libpolly::context::ref(this->polly()));
		this->read_async(buf, len, [&e, &res](size_t s){
			res = s;
			e.set();
		});
		e.wait();
		return res;
	}

	size_t write(uint8_t const * buf, size_t len)
	{
		size_t res;
		libpolly::event e(libpolly::context::ref(this->polly()));
		this->write_async(buf, len, [&e, &res](size_t s){
			res = s;
			e.set();
		});
		e.wait();
		return res;
	}

	detached_async_context write_all_async(uint8_t const * buf, size_t len, boost::function<void()> const & handler)
	{
		boost::shared_ptr<write_all_context> ctx(new write_all_context(this->polly()));
		ctx->buf.assign(buf, buf + len);
		ctx->handler = handler;
		ctx->keep_alive = ctx;
		ctx->write_ctx = this->write_async(ctx->buf.data(), ctx->buf.size(), boost::bind(&stream::write_all_handler, this, _1, ctx.get()));
		return ctx;
	}

	void write_all(uint8_t const * buf, size_t len)
	{
		while (len)
		{
			size_t r = this->write(buf, len);
			buf += r;
			len -= r;
		}
	}

private:
	struct write_all_context
		: async_context_impl
	{
		write_all_context(libpolly_context * polly)
			: sent(0), cancelled(false), done_ev(polly)
		{
		}

		void cancel()
		{
			cancelled = true;
			write_ctx.cancel();
		}

		void wait()
		{
			done_ev.wait();
		}

		std::vector<uint8_t> buf;
		size_t sent;
		boost::function<void()> handler;
		async_context write_ctx;
		bool cancelled;
		boost::shared_ptr<write_all_context> keep_alive;
		libpolly::event done_ev;
	};

	void write_all_handler(size_t s, write_all_context * ctx)
	{
		ctx->sent += s;

		if (ctx->sent == ctx->buf.size() || ctx->cancelled)
		{
			if (ctx->handler)
				ctx->handler();
			ctx->done_ev.set();
			ctx->write_ctx.detach();
			ctx->keep_alive.reset();
		}
		else
		{
			ctx->write_ctx = this->write_async(ctx->buf.data(), ctx->buf.size(), boost::bind(&stream::write_all_handler, this, _1, ctx));
		}
	}
};

class shupito_dev
{
public:
	virtual libpolly_context * polly() const = 0;

	yb::packet read_packet()
	{
		yb::packet res;
		libpolly::event e(libpolly::context::ref(this->polly()));

		this->read_packet_async([&e, &res](yb::packet const & p){
			res = p;
			e.set();
		});

		e.wait();
		return res;
	}

	void write_packet(yb::packet const & p)
	{
		libpolly::event e(libpolly::context::ref(this->polly()));
		this->write_packet_async(p, [&e](){
			e.set();
		});
		e.wait();
	}

	virtual detached_async_context write_packet_async(yb::packet const & p, boost::function<void()> const & handler) = 0;

	virtual detached_async_context read_packet_async(boost::function<void (yb::packet)> const & handler) = 0;
};

class serial_port_stream
	: public stream
{
public:
	serial_port_stream()
		: m_handle(0)
	{
	}

	~serial_port_stream()
	{
		this->close();
	}

	libpolly_context * polly() const
	{
		if (!m_handle)
			return 0;

		libspy_context * ctx = libspy_get_context(m_handle);
		return libspy_get_polly(ctx);
	}

	void open(libspy_context * ctx, std::string const & name)
	{
		libspy_device_handle * handle;
		libspy_device_settings settings;
		settings.baud_rate = 38400;
		settings.bits = 8;
		settings.parity = libspy_parity_none;
		settings.stopbits = libspy_stopbits_1;
		settings.timeout = 0;

		if (libspy_open(ctx, name.c_str(), &settings, &handle) >= 0)
		{
			this->close();
			m_handle = handle;
		}
	}

	void open_async(libspy_context * ctx, std::string const & name, boost::function<void ()> const & handler)
	{
		libspy_device_settings settings;
		settings.baud_rate = 38400;
		settings.bits = 8;
		settings.parity = libspy_parity_none;
		settings.stopbits = libspy_stopbits_1;
		settings.timeout = 0;

		async_open_tran * tran = new async_open_tran();
		tran->self = this;
		tran->handler = handler;
		libspy_open_future * future;
		libspy_begin_open(ctx, name.c_str(), &settings, &open_async_handler, tran, &future);
	}

	void close()
	{
		if (m_handle != 0)
		{
			libspy_close(m_handle);
			m_handle = 0;
		}
	}

	detached_async_context read_async(uint8_t * buf, size_t len, boost::function<void(size_t)> const & handler)
	{
		BOOST_ASSERT(m_handle != 0);

		boost::shared_ptr<async_read_tran> tran_ctx(new async_read_tran(libspy_get_context(m_handle)));
		tran_ctx->handler = handler;
		tran_ctx->keep_alive = tran_ctx;

		libspy_set_user_data(tran_ctx->tran, tran_ctx.get());
		int r = libspy_submit_read(tran_ctx->tran, m_handle, buf, len, &read_async_handler);
		if (r < 0)
			tran_ctx->keep_alive.reset();

		return tran_ctx;
	}

	detached_async_context write_async(uint8_t const * buf, size_t len, boost::function<void(size_t)> const & handler)
	{
		BOOST_ASSERT(m_handle != 0);

		boost::shared_ptr<async_read_tran> tran_ctx(new async_read_tran(libspy_get_context(m_handle)));
		tran_ctx->handler = handler;
		tran_ctx->keep_alive = tran_ctx;

		libspy_set_user_data(tran_ctx->tran, tran_ctx.get());
		int r = libspy_submit_write(tran_ctx->tran, m_handle, buf, len, &write_async_handler);
		if (r < 0)
			tran_ctx->keep_alive.reset();

		return tran_ctx;
	}

private:
	libspy_device_handle * m_handle;

	struct async_open_tran
	{
		serial_port_stream * self;
		std::function<void()> handler;
	};

	static void open_async_handler(libspy_open_future * future, void * user_data, libspy_device_handle * handle)
	{
		async_open_tran * tran = (async_open_tran *)user_data;
		libspy_free_open_future(future);
		tran->self->close();
		tran->self->m_handle = handle;
		if (tran->handler)
			tran->handler();
		delete tran;
	}

	struct async_read_tran
		: async_context_impl
	{
	public:
		async_read_tran(libspy_context * ctx)
		{
			tran = libspy_alloc_transfer(ctx);
		}

		~async_read_tran()
		{
			libspy_free_transfer(tran);
		}

		void cancel()
		{
			libspy_cancel_transfer(tran);
		}

		void wait()
		{
			libspy_wait_for_transfer(tran);
		}

		libspy_transfer * tran;
		boost::shared_ptr<async_context_impl> keep_alive;
		boost::function<void(size_t)> handler;

	private:
		async_read_tran(async_read_tran const &);
		async_read_tran & operator=(async_read_tran const &);
	};

	static void read_async_handler(libspy_transfer * transfer, libspy_transfer_status status)
	{
		async_read_tran * tran_ctx = (async_read_tran *)libspy_get_user_data(transfer);
		if (tran_ctx->handler)
			tran_ctx->handler(libspy_get_transfer_length(transfer));
		tran_ctx->keep_alive.reset();
	}

	static void write_async_handler(libspy_transfer * transfer, libspy_transfer_status status)
	{
		async_read_tran * tran_ctx = (async_read_tran *)libspy_get_user_data(transfer);
		if (tran_ctx->handler)
			tran_ctx->handler(libspy_get_transfer_length(transfer));
		tran_ctx->keep_alive.reset();
	}

	serial_port_stream(serial_port_stream const &);
	serial_port_stream & operator=(serial_port_stream const &);
};

class cdc_acm_stream
	: public stream
{
public:
	cdc_acm_stream()
		: m_handle(0)
	{
	}

	~cdc_acm_stream()
	{
		this->close();
	}

	libpolly_context * polly() const
	{
		if (!m_handle)
			return 0;

		libusby_device * dev = libusby_get_device(m_handle);
		libusby_context * ctx = libusby_get_context(dev);
		return libusby_get_polly(ctx);
	}

	void open(libusby_context * ctx, libusby_device_handle * handle)
	{
		m_ctx = ctx;
		m_handle = handle;
	}

	void close()
	{
		m_handle = 0;
	}

	detached_async_context read_async(uint8_t * buf, size_t len, boost::function<void(size_t)> const & callback)
	{
		boost::shared_ptr<transfer_ctx> ctx(new transfer_ctx(m_ctx));
		ctx->callback = callback;
		ctx->keep_alive = ctx;

		libusby_fill_bulk_transfer(ctx->tran, m_handle, 0x84, buf, len, &tran_completed, ctx.get(), 0);
		int r = libusby_submit_transfer(ctx->tran);
		if (r < 0)
			ctx->keep_alive.reset();
		return ctx;
	}

	detached_async_context write_async(uint8_t const * buf, size_t len, boost::function<void(size_t)> const & callback)
	{
		boost::shared_ptr<transfer_ctx> ctx(new transfer_ctx(m_ctx));
		ctx->callback = callback;
		ctx->keep_alive = ctx;

		libusby_fill_bulk_transfer(ctx->tran, m_handle, 0x03, (uint8_t *)buf, len, &tran_completed, ctx.get(), 0);
		int r = libusby_submit_transfer(ctx->tran);
		if (r < 0)
			ctx->keep_alive.reset();
		return ctx;
	}

	size_t write(uint8_t const * buf, size_t len)
	{
		size_t res;
		libpolly::event e(this->polly());
		this->write_async(buf, len, [&res, &e](size_t s){
			res = s;
			e.set();
		});
		e.wait();
		return res;
	}

private:
	struct transfer_ctx
		: async_context_impl
	{
		transfer_ctx(libusby_context * ctx)
		{
			tran = libusby_alloc_transfer(ctx, 0);
		}

		~transfer_ctx()
		{
			if (tran)
				libusby_free_transfer(tran);
		}

		void cancel()
		{
			libusby_cancel_transfer(tran);
		}

		void wait()
		{
			libusby_wait_for_transfer(tran);
		}

		libusby_transfer * tran;
		boost::shared_ptr<transfer_ctx> keep_alive;
		boost::function<void(size_t)> callback;

	private:
		transfer_ctx(transfer_ctx const &);
		transfer_ctx & operator=(transfer_ctx const &);
	};

	static void tran_completed(libusby_transfer * tran)
	{
		transfer_ctx * ctx = (transfer_ctx *)tran->user_data;
		if (ctx->callback)
			ctx->callback(tran->actual_length);
		ctx->keep_alive.reset();
	}

	libusby_context * m_ctx;
	libusby_device_handle * m_handle;
};

class yb_packet_handler
{
public:
	virtual bool handle_packet(yb::packet const & p) = 0;
};

class shupito_dispatcher
{
public:
	typedef std::list<yb_packet_handler *>::iterator packet_handler_registration;
	typedef libpolly::event event;

	shupito_dispatcher()
		: m_dev(0), m_in_handler(false)
	{
	}

	shupito_dev * get_dev() const
	{
		return m_dev;
	}

	void attach(shupito_dev * dev)
	{
		m_dev = dev;
	}

	void detach()
	{
		m_dev = 0;
	}

	packet_handler_registration register_handler(yb_packet_handler & handler)
	{
		m_handlers.push_front(&handler);
		if (m_handlers.size() == 1 && !m_in_handler)
			m_read_ctx = m_dev->read_packet_async(boost::bind(&shupito_dispatcher::run_one, this, _1));
		return m_handlers.begin();
	}

	void unregister_handler(packet_handler_registration reg)
	{
		m_handlers.erase(reg);
		if (m_handlers.empty())
			m_read_ctx.cancel();
	}

private:
	libpolly::context m_ctx;
	shupito_dev * m_dev;
	std::list<yb_packet_handler *> m_handlers;
	async_context m_read_ctx;
	bool m_in_handler;

	void run_one(yb::packet const & p)
	{
		m_read_ctx.detach();
		m_in_handler = true;

		for (std::list<yb_packet_handler *>::iterator it = m_handlers.begin(); it != m_handlers.end();)
		{
			std::list<yb_packet_handler *>::iterator next = it;
			++next;
			if ((*it)->handle_packet(p))
				break;
			it = next;
		}

		if (!m_handlers.empty())
			m_read_ctx = m_dev->read_packet_async(boost::bind(&shupito_dispatcher::run_one, this, _1));
		m_in_handler = false;
	}
};

class desc_reader
	: public yb_packet_handler
{
public:
	desc_reader(shupito_dispatcher & disp, boost::function<void (yb::device_descriptor const &)> const & handler)
		: m_disp(disp), m_handler(handler)
	{
		m_reg = m_disp.register_handler(*this);
	}

	bool handle_packet(yb::packet const & p)
	{
		if (p[0] != 0)
			return false;

		m_desc.insert(m_desc.end(), p.begin() + 1, p.end());
		if (p.size() < 16 && m_handler)
		{
			m_disp.unregister_handler(m_reg);
			m_handler(yb::device_descriptor::parse(m_desc.data(), m_desc.data() + m_desc.size()));
			delete this;
		}
		return true;
	}

private:
	shupito_dispatcher & m_disp;
	shupito_dispatcher::packet_handler_registration m_reg;

	std::vector<uint8_t> m_desc;
	boost::function<void (yb::device_descriptor const &)> m_handler;
};

void read_desc(shupito_dispatcher & disp, boost::function<void (yb::device_descriptor const &)> const & handler)
{
	disp.get_dev()->write_packet_async(yb::make_packet(0) % 0, [&disp, handler](){
		new desc_reader(disp, handler);
	});
}

class tunnel_handler
	: private yb_packet_handler
{
public:
	tunnel_handler(shupito_dispatcher & disp)
		: m_disp(disp)
	{
	}

	~tunnel_handler()
	{
		if (!m_cfg.empty())
			m_disp.unregister_handler(m_reg);
	}

	void set_config(yb::device_config const & cfg)
	{
		m_cfg = cfg;
		if (!m_cfg.empty())
			m_reg = m_disp.register_handler(*this);
	}

	void get_tunnel_list(boost::function<void (std::vector<std::string> const &)> const & callback)
	{
		m_disp.get_dev()->write_packet(yb::make_packet(m_cfg.cmd) % 0 % 0);
		m_pipe_list_callback = callback;
	}

	shupito_dispatcher & disp()
	{
		return m_disp;
	}

private:
	friend class tunnel_stream;

	void open_tunnel(yb::string_ref const & name, boost::function<void (uint8_t)> const & callback)
	{
		if (callback)
		{
			open_tunnel_irp irp;
			irp.callback = callback;
			m_open_tunnel_irps.push_back(std::move(irp));
		}
		m_disp.get_dev()->write_packet(yb::make_packet(m_cfg.cmd) % 0 % 1 % name);
	}

	void register_pipe_handler(uint8_t pipe_no, yb_packet_handler & handler)
	{
		m_pipe_handlers[pipe_no] = &handler;
	}

	void unregister_pipe_handler(uint8_t pipe_no)
	{
		m_pipe_handlers.erase(pipe_no);
	}

	detached_async_context write_async(uint8_t pipe_no, yb::buffer_ref data, boost::function<void(size_t)> const & handler)
	{
		size_t chunk = (std::min)(data.size(), static_cast<size_t>(14));
		return m_disp.get_dev()->write_packet_async(yb::make_packet(m_cfg.cmd) % pipe_no % yb::buffer_ref(data.data(), chunk), [chunk, handler](){
			if (handler)
				handler(chunk);
		});
	}

private:
	bool handle_packet(yb::packet const & p)
	{
		if (p[0] != m_cfg.cmd)
			return false;
		
		if (p.size() >= 3 && p[1] == 0 && p[2] == 0)
		{
			boost::function<void (std::vector<std::string> const &)> callback = std::move(m_pipe_list_callback);
			m_pipe_list_callback.clear();

			if (callback)
				callback(this->handle_pipe_list(p));
			return true;
		}
		else if (p.size() == 4 && p[1] == 0 && p[2] == 1)
		{
			if (m_open_tunnel_irps.empty())
				return false;

			open_tunnel_irp irp = m_open_tunnel_irps.front();
			m_open_tunnel_irps.pop_front();

			uint8_t pipe_no = p[3];
			if (irp.callback)
				irp.callback(pipe_no);
			return true;
		}
		else if (p.size() > 1)
		{
			std::map<uint8_t, yb_packet_handler *>::const_iterator it = m_pipe_handlers.find(p[1]);
			if (it != m_pipe_handlers.end())
				it->second->handle_packet(p);
		}

		return false;
	}

	std::vector<std::string> handle_pipe_list(yb::packet const & p)
	{
		std::vector<std::string> pipe_names;
		for (size_t i = 3; i < p.size();)
		{
			if (i + 1 + p[i] > p.size())
				throw std::runtime_error("Invalid response while enumerating pipes.");

			std::string pipe_name;
			for (size_t j = 0; j < p[i]; ++j)
				pipe_name.push_back(p[i+j+1]);
			pipe_names.push_back(pipe_name);

			i += 1 + p[i];
		}

		return pipe_names;
	}

	shupito_dispatcher & m_disp;
	yb::device_config m_cfg;
	shupito_dispatcher::packet_handler_registration m_reg;

	struct open_tunnel_irp
	{
		boost::function<void (uint8_t)> callback;
	};

	std::list<open_tunnel_irp> m_open_tunnel_irps;
	boost::function<void (std::vector<std::string> const &)> m_pipe_list_callback;
	std::map<uint8_t, yb_packet_handler *> m_pipe_handlers;
};

class tunnel_stream
	: public stream, private yb_packet_handler
{
public:
	tunnel_stream()
		: m_th(0)
	{
	}

	~tunnel_stream()
	{
		this->close_quietly();
	}

	libpolly_context * polly() const
	{
		return m_th->disp().get_dev()->polly();
	}

	void close_quietly()
	{
		if (m_pipe_no)
		{
			m_th->unregister_pipe_handler(m_pipe_no);
			m_pipe_no = 0;
		}
	}

	void open_async(tunnel_handler & th, yb::string_ref const & name, boost::function<void()> const & callback)
	{
		m_th = &th;
		m_th->open_tunnel(name, boost::bind(&tunnel_stream::open_pipe_completed, this, _1, callback));
	}

	detached_async_context read_async(uint8_t * buf, size_t len, boost::function<void (size_t)> const & callback)
	{
		if (m_read_buffer.empty())
		{
			read_irp irp;
			irp.buf = buf;
			irp.size = len;
			irp.callback = callback;
			m_read_irps.push_back(std::move(irp));
		}
		else
		{
			size_t chunk = (std::min)(m_read_buffer.size(), len);
			std::copy(m_read_buffer.begin(), m_read_buffer.begin() + chunk, buf);
			m_read_buffer.erase(m_read_buffer.begin(), m_read_buffer.begin() + chunk);
			if (callback)
				callback(chunk);
		}

		return detached_async_context();
	}

	detached_async_context write_async(uint8_t const * buf, size_t len, boost::function<void (size_t)> const & callback)
	{
		return m_th->write_async(m_pipe_no, yb::buffer_ref(buf, len), callback);
	}

private:
	void open_pipe_completed(uint8_t pipe_no, boost::function<void()> const & callback)
	{
		m_pipe_no = pipe_no;
		m_th->register_pipe_handler(pipe_no, *this);
		if (callback)
			callback();
	}

	bool handle_packet(yb::packet const & p)
	{
		BOOST_ASSERT(p.size() >= 2);
		BOOST_ASSERT(p[1] == m_pipe_no);

		uint8_t const * first = p.data() + 2;
		uint8_t const * last = p.data() + p.size();

		while (first != last && !m_read_irps.empty())
		{
			read_irp irp = std::move(m_read_irps.front());
			m_read_irps.pop_front();

			size_t chunk = (std::min)(static_cast<size_t>(last - first), irp.size);
			std::copy(first, first + chunk, irp.buf);
			first += chunk;
			if (irp.callback)
				irp.callback(chunk);
		}

		m_read_buffer.insert(m_read_buffer.end(), first, last);
		return true;
	}

	tunnel_handler * m_th;
	uint8_t m_pipe_no;

	struct read_irp
	{
		uint8_t * buf;
		size_t size;
		boost::function<void (size_t)> callback;
	};

	std::deque<read_irp> m_read_irps;
	std::deque<uint8_t> m_read_buffer;
};

class stream_shupito_dev
	: public shupito_dev
{
public:
	stream_shupito_dev()
		: m_packet_pos(0)
	{
	}

	libpolly_context * polly() const
	{
		return m_stream->polly();
	}

	void attach(boost::shared_ptr<stream> const & s)
	{
		this->detach();
		m_stream = s;
	}

	void detach()
	{
		m_stream.reset();
	}

	detached_async_context read_packet_async(boost::function<void (yb::packet)> const & handler)
	{
		// XXX: lock
		if (!m_packets.empty())
		{
			boost::shared_ptr<event_async_context> ctx(new event_async_context(this->polly()));

			yb::packet p = m_packets.front();
			m_packets.pop_front();

			libpolly::context::ref(this->polly()).post([handler, ctx, p](){
				handler(p);
				ctx->complete();
			});
			return ctx;
		}
		else
		{
			boost::shared_ptr<read_packet_ctx> ctx(new read_packet_ctx(this->polly()));
			ctx->buf.resize(64);
			ctx->handler = handler;
			ctx->read_ctx = m_stream->read_async(ctx->buf.data(), ctx->buf.size(), boost::bind(&stream_shupito_dev::read_packet_handler, this, ctx, _1));
			return ctx;
		}
	}

	detached_async_context write_packet_async(yb::packet const & p, boost::function<void()> const & handler)
	{
		BOOST_ASSERT(!p.empty());
		BOOST_ASSERT(p.size() <= 16);
		BOOST_ASSERT(p[0] < 16);

		uint8_t buf[17];
		buf[0] = 0x80;
		buf[1] = static_cast<uint8_t>((p.size() - 1) | (p[0] << 4));
		std::copy(p.begin() + 1, p.end(), buf + 2);

		return m_stream->write_all_async(buf, p.size() + 1, handler);
	}

private:
	struct read_packet_ctx
		: async_context_impl
	{
		read_packet_ctx(libpolly_context * ctx)
			: cancelled(false), done_ev(ctx)
		{
		}

		void cancel()
		{
			cancelled = true;
			read_ctx.cancel();
		}

		void wait()
		{
			done_ev.wait();
		}

		bool cancelled;
		libpolly::event done_ev;
		async_context read_ctx;
		std::vector<uint8_t> buf;
		boost::function<void (yb::packet)> handler;
	};

	void read_packet_handler(boost::shared_ptr<read_packet_ctx> const & ctx, size_t r)
	{
		uint8_t const * buf = ctx->buf.data();
		for (std::size_t i = 0; i < r; )
		{
			switch (m_packet_pos)
			{
			case 0:
				while (buf[i] != 0x80 && i < r)
					++i;
				if (i < r)
				{
					m_packet_pos = 1;
					++i;
				}
				break;

			case 1:
				m_partial_packet.resize((buf[i] & 0xf) + 1);
				m_partial_packet[0] = buf[i] >> 4;
				++i;
				++m_packet_pos;

				if (m_partial_packet.size() == 1)
				{
					m_packets.push_back(std::move(m_partial_packet));
					m_partial_packet.clear();
					m_packet_pos = 0;
					break;
				}

				// fallthrough

			default:
				while (i < r)
				{
					m_partial_packet[m_packet_pos-1] = buf[i];
					++i;

					if (m_packet_pos == m_partial_packet.size())
					{
						m_packets.push_back(std::move(m_partial_packet));
						m_partial_packet.clear();
						m_packet_pos = 0;
						break;
					}

					++m_packet_pos;
				}
			}
		}

		if (!m_packets.empty() || ctx->cancelled)
		{
			if (!m_packets.empty())
			{
				yb::packet p = m_packets.front();
				m_packets.pop_front();
				ctx->handler(p);
			}
			else
			{
				ctx->handler(yb::packet());
			}

			ctx->read_ctx.detach();
			ctx->done_ev.set();
		}
		else
		{
			ctx->read_ctx = m_stream->read_async(ctx->buf.data(), ctx->buf.size(), boost::bind(&stream_shupito_dev::read_packet_handler, this, ctx, _1));
		}
	}

	boost::shared_ptr<stream> m_stream;
	std::list<yb::packet> m_packets;

	std::vector<uint8_t> m_partial_packet;
	std::size_t m_packet_pos;
};

class context
{
public:
	explicit context(libpolly_context * polly)
		: polly(polly), spy(0), usby(0)
	{
		libspy_init_with_polly(&spy, polly);
		libusby_init_with_polly(&usby, polly);
	}

	~context()
	{
		libusby_exit(usby);
		libspy_exit(spy);
	}

	libpolly_context * get_polly() const { return polly; }

	void open_stream(connect_params const & info, boost::function<void(boost::shared_ptr<stream>)> const & handler)
	{
		switch (info.kind)
		{
		case connect_params::ck_serial_port:
			{
				boost::shared_ptr<serial_port_stream> port(new serial_port_stream());
				port->open_async(spy, boost::get<std::string>(info.params), [handler, port](){
					if (handler)
						handler(port);
				});
				break;
			}
		case connect_params::ck_usb:
			{
				boost::shared_ptr<cdc_acm_stream> port(new cdc_acm_stream());
				port->open(usby, boost::get<boost::shared_ptr<libusby_device_handle> >(info.params).get());
				if (handler)
					handler(port);
				break;
			}
		}
	}

	void open_shupito_dev(connect_params const & info, boost::function<void(boost::shared_ptr<shupito_dev>)> const & handler)
	{
		boost::shared_ptr<stream_shupito_dev> res(new stream_shupito_dev());

		this->open_stream(info, [res, handler](boost::shared_ptr<stream> const & s){
			res->attach(s);
			if (handler)
				handler(res);
		});
	}

	libpolly_context * polly;
	libspy_context * spy;
	libusby_context * usby;
};

static connect_params_info const & choose_connection(std::vector<connect_params_info> const & connect_infos, std::string const & prompt)
{
	for (;;)
	{
		std::cout << prompt << " [";
		if (connect_infos.size() == 0)
			std::cout << "c";
		else if (connect_infos.size() == 1)
			std::cout << "1c";
		else
			std::cout << "1-" << connect_infos.size() << "c";
		std::cout << "]: " << std::flush;

		std::string line;
		if (!std::getline(std::cin, line))
			continue;

		long choice = strtol(line.c_str(), 0, 10);
		if (choice >= 1 && (size_t)choice <= connect_infos.size())
			return connect_infos[choice-1];
	}
}

class blackhole_reader
{
public:
	blackhole_reader()
		: m_enabled(true)
	{
	}

	~blackhole_reader()
	{
		static_cast<bool volatile &>(m_enabled) = false;
		m_read_context.cancel();
		m_read_context.wait();
	}

	void attach(stream * s)
	{
		m_s = s;
		this->start_read();
	}

private:
	stream * m_s;
	uint8_t m_buf[256];

	bool m_enabled;
	async_context m_read_context;

	void start_read()
	{
		if (static_cast<bool volatile &>(m_enabled))
			m_read_context = m_s->read_async(m_buf, sizeof m_buf, boost::bind(&blackhole_reader::read_complete, this));
	}

	void read_complete()
	{
		m_read_context.detach();
		this->start_read();
	}
};

class timer
{
public:
	timer()
		: m_timer(0)
	{
	}

	~timer()
	{
		if (m_timer)
			libpolly_destroy_timer(m_timer);
	}

	void attach(libpolly_context * polly)
	{
		m_polly = polly;
		// XXX
		libpolly_create_timer(polly, &m_timer);
	}

	detached_async_context set(int timeout, boost::function<void(libpolly_timer_status)> const & handler)
	{
		boost::shared_ptr<timer_context> ctx(new timer_context(m_polly));
		ctx->m_timer = m_timer;
		ctx->handler = handler;
		ctx->keep_alive = ctx;
		libpolly_set_timer(m_timer, timeout, &timer::timer_handler, ctx.get());
		return ctx;
	}

private:
	struct timer_context
		: async_context_impl
	{
		explicit timer_context(libpolly_context * polly)
			: done_ev(polly)
		{
		}

		void cancel()
		{
			libpolly_cancel_timer(m_timer);
		}

		void wait()
		{
			done_ev.wait();
		}

		libpolly::event done_ev;
		boost::shared_ptr<timer_context> keep_alive;
		boost::function<void(libpolly_timer_status)> handler;
		libpolly_timer * m_timer;
	};

	static void timer_handler(void * user_data, libpolly_timer_status status)
	{
		timer_context * ctx = (timer_context *)user_data;
		if (ctx->handler)
			ctx->handler(status);
		ctx->done_ev.set();
		ctx->keep_alive.reset();
	}

	libpolly_context * m_polly;
	libpolly_timer * m_timer;

	timer(timer const &);
	timer & operator=(timer const &);
};

static uint8_t const flash_mode_sequence[] = { 0x74, 0x7E, 0x7A, 0x33 };

class avr232boot_handler
{
public:
	avr232boot_handler()
		: m_s(0)
	{
	}

	void attach(stream & s)
	{
		m_s = &s;
		m_timer.attach(s.polly());
	}

	detached_async_context switch_to_flash_mode_async(boost::function<void()> const & handler)
	{
		boost::shared_ptr<switch_to_flash_mode_ctx> ctx(new switch_to_flash_mode_ctx(m_s->polly()));
		ctx->handler = handler;
		ctx->read_buf.resize(256);
		ctx->m_state = switch_to_flash_mode_ctx::st_no_data;
		ctx->read_ctx = m_s->read_async(ctx->read_buf.data(), ctx->read_buf.size(), boost::bind(&avr232boot_handler::switch_to_flash_mode_handler, this, ctx, _1));
		ctx->write_ctx = m_s->write_all_async(flash_mode_sequence, sizeof flash_mode_sequence, boost::bind(&avr232boot_handler::switch_to_flash_mode_write_handler, this, ctx));
		return ctx;
	}

private:
	struct switch_to_flash_mode_ctx
		: async_context_impl
	{
		switch_to_flash_mode_ctx(libpolly_context * polly)
			: cancelled(false), done_ev(polly)
		{
		}

		void cancel()
		{
			cancelled = true;
			read_ctx.cancel();
			write_ctx.cancel();
			timer_ctx.cancel();
		}

		void wait()
		{
			done_ev.wait();
		}

		bool cancelled;
		std::vector<uint8_t> read_buf;
		boost::function<void()> handler;
		async_context read_ctx;
		async_context write_ctx;
		async_context timer_ctx;
		libpolly::event done_ev;

		enum { st_no_data, st_ok, st_error } m_state;
	};

	void switch_to_flash_mode_write_handler(boost::shared_ptr<switch_to_flash_mode_ctx> const & ctx)
	{
		ctx->write_ctx.detach();
		if (!ctx->cancelled)
		{
			ctx->timer_ctx = m_timer.set(1000, boost::bind(&avr232boot_handler::switch_to_flash_mode_timer_handler, this, ctx, _1));
		}
	}

	void switch_to_flash_mode_handler(boost::shared_ptr<switch_to_flash_mode_ctx> const & ctx, size_t r)
	{
		ctx->read_ctx.detach();

		if (ctx->m_state == switch_to_flash_mode_ctx::st_no_data && r == 1 && ctx->read_buf[0] == 0x14)
			ctx->m_state = switch_to_flash_mode_ctx::st_ok;
		else if (r > 0)
			ctx->m_state = switch_to_flash_mode_ctx::st_error;

		if (!ctx->cancelled)
		{
			ctx->read_ctx = m_s->read_async(ctx->read_buf.data(), ctx->read_buf.size(), boost::bind(&avr232boot_handler::switch_to_flash_mode_handler, this, ctx, _1));
		}
	}

	void switch_to_flash_mode_timer_handler(boost::shared_ptr<switch_to_flash_mode_ctx> const & ctx, libpolly_timer_status status)
	{
		ctx->timer_ctx.detach();

		if (ctx->m_state == switch_to_flash_mode_ctx::st_ok)
		{
			ctx->cancelled = true;
			ctx->read_ctx.cancel();
			if (ctx->handler)
				ctx->handler();
			ctx->done_ev.set();
		}
		else if (!ctx->cancelled && status != libpolly_timer_cancelled)
		{
			ctx->m_state = switch_to_flash_mode_ctx::st_no_data;
			ctx->write_ctx = m_s->write_all_async(flash_mode_sequence, sizeof flash_mode_sequence, boost::bind(&avr232boot_handler::switch_to_flash_mode_write_handler, this, ctx));
		}
	}

	stream * m_s;
	timer m_timer;
};

class main_handler
{
public:
	main_handler(context & ctx, connect_params_info usb_conn, connect_params_info aux_conn)
		: m_ctx(ctx), m_usb_conn(usb_conn), m_aux_conn(aux_conn), m_tunnel_handler(m_shupito_disp), m_done_ev(ctx.get_polly())
	{
	}

	void run()
	{
		m_ctx.open_stream(m_usb_conn.params, [this](boost::shared_ptr<stream> const & s){
			m_usb_dev = s;
			m_usb_bh_reader.attach(s.get());
			this->dev_opened();
		});

		m_ctx.open_shupito_dev(m_aux_conn.params, [this](boost::shared_ptr<shupito_dev> const & s){
			m_aux_dev = s;
			this->dev_opened();
		});

		m_done_ev.wait();
	}

private:
	void dev_opened()
	{
		if (!m_usb_dev || !m_aux_dev)
			return;

		m_shupito_disp.attach(m_aux_dev.get());
		read_desc(m_shupito_disp, [this](yb::device_descriptor const & desc){
			m_device_desc = desc;
			if (yb::device_config const * tunnel_cfg = m_device_desc.get_config("356e9bf7-8718-4965-94a4-0be370c8797c"))
			{
				m_tunnel_handler.set_config(*tunnel_cfg);
				m_tunnel_stream.open_async(m_tunnel_handler, "usb", boost::bind(&main_handler::usb_tunnel_opened, this));
			}
			else
			{
				m_done_ev.set();
			}
		});
	}

	void usb_tunnel_opened()
	{
		m_avr232boot_handler.attach(m_tunnel_stream);
		m_avr232boot_handler.switch_to_flash_mode_async([this]{
			m_done_ev.set();
		});
	}

	context & m_ctx;
	connect_params_info m_usb_conn;
	connect_params_info m_aux_conn;

	boost::shared_ptr<stream> m_usb_dev;
	boost::shared_ptr<shupito_dev> m_aux_dev;

	blackhole_reader m_usb_bh_reader;

	shupito_dispatcher m_shupito_disp;
	tunnel_handler m_tunnel_handler;
	tunnel_stream m_tunnel_stream;

	avr232boot_handler m_avr232boot_handler;

	yb::device_descriptor m_device_desc;

	libpolly::event m_done_ev;
};

int main()
{
	libpolly::context polly = libpolly::context::create();
	context ctx(polly.get());

	std::vector<connect_params_info> connect_infos;

	{
		libusby_device ** devs;
		int dev_count = libusby_get_device_list(ctx.usby, &devs);
		for (int i = 0; i < dev_count; ++i)
		{
			libusby_device_descriptor desc;
			libusby_get_device_descriptor_cached(devs[i], &desc);

			if (desc.idVendor != 0x4a61 || desc.idProduct != 0x679a)
				continue;

			libusby_device_handle * h;
			if (libusby_open(devs[i], &h) < 0)
				continue;

			boost::shared_ptr<libusby_device_handle> hh(h, &libusby_close);

			std::string product, manufacturer, sn;
			if (desc.iProduct)
				product = libusby::get_string_desc_utf8(h, desc.iProduct);
			if (desc.iManufacturer)
				manufacturer = libusby::get_string_desc_utf8(h, desc.iManufacturer);
			if (desc.iSerialNumber)
				sn = libusby::get_string_desc_utf8(h, desc.iSerialNumber);

			connect_params_info info;
			info.description = "USB: " + manufacturer + " " + product + " " + sn;
			info.params.kind = connect_params::ck_usb;
			info.params.params = hh;
			connect_infos.push_back(std::move(info));
		}

		libusby_free_device_list(devs, 1);
	}

	{
		libspy_device const * devs;
		int dev_count = libspy_get_device_list(ctx.spy, &devs);
		for (int i = 0; i < dev_count; ++i)
		{
			connect_params_info cpi;
			cpi.description = devs[i].friendly_name;
			cpi.params.kind = connect_params::ck_serial_port;
			cpi.params.params = devs[i].path;
			connect_infos.push_back(cpi);
		}

		libspy_free_device_list(devs);
	}

	for (size_t i = 0; i < connect_infos.size(); ++i)
		std::cout << (i+1) << ". " << connect_infos[i].description << "\n";
	std::cout << "\n";

	boost::shared_ptr<shupito_dev> aux_dev;
	boost::shared_ptr<stream> usb_dev;

	connect_params_info usb_conn = choose_connection(connect_infos, "Choose the main shupito connection");
	connect_params_info aux_conn = choose_connection(connect_infos, "Choose the aux connection");
	connect_infos.clear();

	main_handler h(ctx, usb_conn, aux_conn);
	h.run();
}
