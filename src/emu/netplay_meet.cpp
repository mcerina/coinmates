// license:BSD-3-Clause
/***************************************************************************

    netplay_meet.cpp

    Where netplay players meet without a server of our own: public
    BitTorrent trackers and STUN. See netplay_meet.h.

***************************************************************************/

#include "emu.h"
#include "netplay_meet.h"

#include "multibyte.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>


namespace {

using udp = asio::ip::udp;

// the answers took 0.26 s at most, from a phone; a late one still counts (the same transaction)
constexpr u32 STUN_RETRY_MS = 300;
// no answer from any after a far round trip and a lost request: STUN is blocked, or no router in the
// way; the port as at home
constexpr u32 STUN_GIVE_UP_MS = netplay_meet::FAR_RTT_MS + STUN_RETRY_MS;
constexpr u32 CONNECTION_MS = 60000;    // BEP 15: a connection id is good for a minute
// a socket that left stays listed for the tracker's interval, 15 to 60 minutes: other ports and rejoins
// pile up. A choice: a dozen sockets for each of 4 players
constexpr int NUM_WANT = 50;
constexpr u64 PROTOCOL_ID = 0x41727101980;
constexpr u32 STUN_MAGIC = 0x2112a442;

enum : u32 { ACTION_CONNECT = 0, ACTION_ANNOUNCE = 1, ACTION_ERROR = 3 };
enum : u32 { EVENT_NONE = 0, EVENT_STARTED = 2 };

std::optional<udp::endpoint> resolve(const std::string &server)
{
	const auto colon = server.rfind(':');
	if (colon == std::string::npos)
		return std::nullopt;
	asio::io_context io;
	udp::resolver resolver(io);
	asio::error_code ec;
	auto results = resolver.resolve(udp::v4(), server.substr(0, colon), server.substr(colon + 1), ec);
	return ec || results.empty() ? std::nullopt : std::optional<udp::endpoint>(*results.begin());
}

} // anonymous namespace


struct netplay_meet::resolved
{
	std::mutex lock;
	int stun_left = 0;
	std::vector<udp::endpoint> stun, trackers;
	std::vector<std::string> failed;
};

netplay_meet::netplay_meet(udp::socket &socket, std::string_view trackers, std::string_view stun, const hash &room)
	: m_socket(socket)
	, m_resolved(std::make_shared<resolved>())
	, m_room(room)
	, m_rng(std::random_device()())
{
	// as a client 0.0.1 would write it; the rest random, from the system: it is sent to strangers
	std::random_device device;
	std::memcpy(m_peer_id.data(), "-CM0001-", 8);
	for (size_t i = 8; i < m_peer_id.size(); i++)
		m_peer_id[i] = u8(device());
	m_key = device();

	// each name in a thread of its own: a slow DNS takes seconds for every name
	for (const bool is_stun : { true, false })
	{
		const std::string list(is_stun ? stun : trackers);
		for (size_t start = 0; start < list.size(); )
		{
			const size_t end = std::min(list.find(';', start), list.size());
			std::string server = list.substr(start, end - start);
			start = end + 1;
			if (server.empty())
				continue;
			if (is_stun)
				m_resolved->stun_left++;
			std::thread([r = m_resolved, server = std::move(server), is_stun] ()
			{
				const std::optional<udp::endpoint> endpoint = resolve(server);
				std::lock_guard guard(r->lock);
				if (!endpoint)
					r->failed.push_back(server);
				else
					(is_stun ? r->stun : r->trackers).push_back(*endpoint);
				if (is_stun)
					r->stun_left--;
			}).detach();
		}
	}
}

void netplay_meet::tick(u32 now, bool poll)
{
	m_now = now;
	const size_t old_stun = m_stun.size(), old_trackers = m_trackers.size();
	{
		std::lock_guard guard(m_resolved->lock);
		for (size_t i = m_stun.size(); i < m_resolved->stun.size(); i++)
		{
			m_stun.emplace_back().endpoint = m_resolved->stun[i];
			new_stun_transaction(m_stun.back());
		}
		for (size_t i = m_trackers.size(); i < m_resolved->trackers.size(); i++)
			m_trackers.emplace_back().endpoint = m_resolved->trackers[i];
		for (const std::string &name : m_resolved->failed)
			osd_printf_verbose("Netplay: cannot resolve %s\n", name);
		m_resolved->failed.clear();
		m_stun_ready = !m_resolved->stun_left;
	}

	// a new one at once; the unanswered ones again, until two have answered: two are enough to compare
	for (size_t i = old_stun; i < m_stun.size(); i++)
	{
		ask_stun(m_stun[i]);
		m_stun_ms = now;
	}
	const auto seen = std::count_if(m_stun.begin(), m_stun.end(), [] (const server &s) { return s.seen.has_value(); });
	if (seen < std::min<ptrdiff_t>(2, m_stun.size()) && (!m_stun_ms || now - *m_stun_ms >= STUN_RETRY_MS))
	{
		m_stun_ms = now;
		for (server &s : m_stun)
			if (!s.seen)
				ask_stun(s);
	}

	if (!public_port())
		return;
	if (!m_poll_ms || (poll && now - *m_poll_ms >= POLL_MS))
	{
		m_poll_ms = now;
		for (server &t : m_trackers)
			ask_tracker(t);
	}
	else
		for (size_t i = old_trackers; i < m_trackers.size(); i++)
			ask_tracker(m_trackers[i]);
}

void netplay_meet::restart()
{
	for (server &s : m_stun)
	{
		s.seen.reset();
		new_stun_transaction(s);
	}
	// a tracker refused a connection id of another port
	for (server &t : m_trackers)
	{
		t.connected = false;
		t.announced = false;
	}
	m_stun_ms.reset();
	m_stun_first_ms.reset();
	m_poll_ms.reset();
	m_announced_ms.reset();
}

// RFC 5389: the same for the retransmissions, so that a late answer counts
void netplay_meet::new_stun_transaction(server &s)
{
	for (u8 &b : s.stun_transaction)
		b = u8(m_rng());
}

// RFC 5389: a binding request, answered with the address it came from
void netplay_meet::ask_stun(server &s)
{
	if (!m_stun_first_ms)
		m_stun_first_ms = m_now;
	u8 request[20];
	put_u16be(request, 0x0001);
	put_u16be(request + 2, 0);
	put_u32be(request + 4, STUN_MAGIC);
	std::memcpy(request + 8, s.stun_transaction, sizeof(s.stun_transaction));
	send(s.endpoint, request, sizeof(request));
}

// BEP 15: a connection id, then announces with it
void netplay_meet::ask_tracker(server &t)
{
	if (!m_asked_ms)
		m_asked_ms = m_now;
	t.transaction = u32(m_rng()) | 1;
	if (!t.connected || m_now - t.connected_ms >= CONNECTION_MS)
	{
		u8 request[16];
		put_u64be(request, PROTOCOL_ID);
		put_u32be(request + 8, ACTION_CONNECT);
		put_u32be(request + 12, t.transaction);
		send(t.endpoint, request, sizeof(request));
		return;
	}
	const std::optional<u16> port = public_port();
	if (!port)
		return;
	u8 request[98];
	put_u64be(request, t.connection);
	put_u32be(request + 8, ACTION_ANNOUNCE);
	put_u32be(request + 12, t.transaction);
	std::copy(m_room.begin(), m_room.end(), request + 16);
	std::copy(m_peer_id.begin(), m_peer_id.end(), request + 36);
	put_u64be(request + 56, 0);         // downloaded
	put_u64be(request + 64, 1);         // left: a peer that still wants the "file"
	put_u64be(request + 72, 0);         // uploaded
	put_u32be(request + 80, t.announced ? EVENT_NONE : EVENT_STARTED);
	put_u32be(request + 84, 0);         // the address the packet comes from
	put_u32be(request + 88, m_key);
	put_u32be(request + 92, NUM_WANT);
	put_u16be(request + 96, *port);
	send(t.endpoint, request, sizeof(request));
}

void netplay_meet::send(const udp::endpoint &to, const u8 *data, size_t size)
{
	asio::error_code ec;
	m_socket.send_to(asio::buffer(data, size), to, 0, ec);
	if (ec)
		osd_printf_verbose("Netplay: cannot send to %s:%d: %s\n", to.address().to_string(), int(to.port()), ec.message());
}

bool netplay_meet::handle(const udp::endpoint &from, const u8 *data, size_t size)
{
	for (server &s : m_stun)
	{
		if (s.endpoint != from)
			continue;
		if (size < 20 || get_u16be(data) != 0x0101 || get_u32be(data + 4) != STUN_MAGIC || std::memcmp(data + 8, s.stun_transaction, 12))
			return true;
		for (size_t i = 20; i + 4 <= size; )
		{
			const u16 type = get_u16be(data + i);
			const u16 length = get_u16be(data + i + 2);
			// XOR-MAPPED-ADDRESS, IPv4
			if (type == 0x0020 && length >= 8 && i + 12 <= size && data[i + 5] == 1)
			{
				s.seen = udp::endpoint(asio::ip::address_v4(get_u32be(data + i + 8) ^ STUN_MAGIC), get_u16be(data + i + 6) ^ (STUN_MAGIC >> 16));
				if (std::find(m_own.begin(), m_own.end(), *s.seen) == m_own.end())
				{
					m_own.push_back(*s.seen);
					update_peers();
				}
				break;
			}
			i += 4 + ((length + 3) & ~3);
		}
		return true;
	}

	for (server &t : m_trackers)
	{
		if (t.endpoint != from)
			continue;
		if (size < 8 || !t.transaction || get_u32be(data + 4) != t.transaction)
			return true;
		t.transaction = 0;
		const u32 action = get_u32be(data);
		if (action == ACTION_CONNECT && size >= 16)
		{
			m_answered = true;
			t.connected = true;
			t.connection = get_u64be(data + 8);
			t.connected_ms = m_now;
			ask_tracker(t);
		}
		else if (action == ACTION_ANNOUNCE && size >= 20)
		{
			m_answered = true;
			t.announced = true;
			if (!m_announced_ms)
				m_announced_ms = m_now;
			t.peers.clear();
			for (size_t i = 20; i + 6 <= size && t.peers.size() < NUM_WANT; i += 6)
				if (get_u32be(data + i) && get_u16be(data + i + 4))
					t.peers.emplace_back(asio::ip::address_v4(get_u32be(data + i)), get_u16be(data + i + 4));
			update_peers();
		}
		else if (action == ACTION_ERROR)
		{
			t.connected = false;
			std::string message(reinterpret_cast<const char *>(data + 8), std::min<size_t>(size - 8, 100));
			std::replace_if(message.begin(), message.end(), [] (char c) { return u8(c) < 0x20 || u8(c) >= 0x7f; }, '?');
			osd_printf_verbose("Netplay: the tracker %s:%d says: %s\n", from.address().to_string(), int(from.port()), message);
		}
		return true;
	}
	return false;
}

std::optional<u16> netplay_meet::public_port() const
{
	for (const server &s : m_stun)
		if (s.seen)
			return s.seen->port();
	if ((m_stun.empty() && m_stun_ready) || (m_stun_first_ms && m_now - *m_stun_first_ms >= STUN_GIVE_UP_MS))
		return m_socket.local_endpoint().port();
	return std::nullopt;
}

std::optional<asio::ip::udp::endpoint> netplay_meet::public_address() const
{
	for (const server &s : m_stun)
		if (s.seen)
			return s.seen;
	return std::nullopt;
}

bool netplay_meet::symmetric_nat() const
{
	const std::optional<udp::endpoint> first = public_address();
	return first && std::any_of(m_stun.begin(), m_stun.end(), [&first] (const server &s) { return s.seen && *s.seen != *first; });
}

void netplay_meet::update_peers()
{
	std::vector<udp::endpoint> peers;
	for (const server &t : m_trackers)
		for (const udp::endpoint &endpoint : t.peers)
			if (std::find(m_own.begin(), m_own.end(), endpoint) == m_own.end() && std::find(peers.begin(), peers.end(), endpoint) == peers.end())
				peers.push_back(endpoint);
	if (peers != m_peers)
	{
		for (const udp::endpoint &endpoint : peers)
			if (std::find(m_peers.begin(), m_peers.end(), endpoint) == m_peers.end())
				osd_printf_verbose("Netplay: the trackers have %s:%d\n", endpoint.address().to_string(), int(endpoint.port()));
		m_peers = std::move(peers);
		m_changed = true;
	}
}
