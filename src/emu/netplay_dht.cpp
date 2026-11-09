// license:BSD-3-Clause
/***************************************************************************

    netplay_dht.cpp

    Where netplay players meet without a server of our own: the BitTorrent
    DHT. See netplay_dht.h.

***************************************************************************/

#include "emu.h"
#include "netplay_dht.h"

#include "multibyte.h"

#include <algorithm>
#include <random>
#include <sstream>


namespace {

using udp = asio::ip::udp;

constexpr u32 STEP_MS = 200;        // more nodes asked, until the closest have answered
constexpr u32 ANSWER_MS = 2000;     // a node that hasn't answered by then is forgotten
constexpr u32 REFRESH_MS = 3000;    // the closest asked again for new players, and announced to
constexpr int ALPHA = 4;            // nodes asked at every step
constexpr size_t K = 16;            // the closest nodes asked and announced to (BEP 5 keeps 8 per bucket): more against the nodes that don't keep them
constexpr size_t MAX_NODES = 64;
constexpr size_t MAX_PEERS = 64;

// a bencoded value; the strings point into the packet
struct bvalue
{
	char type = 0;                  // i, s, l or d
	std::string_view str;           // a string, or the digits of an integer
	std::vector<bvalue> items;      // a list, or a dictionary as key, value, key, value...

	const bvalue *get(std::string_view key) const
	{
		if (type == 'd')
			for (size_t i = 0; i + 1 < items.size(); i += 2)
				if (items[i].type == 's' && items[i].str == key)
					return &items[i + 1];
		return nullptr;
	}

	std::string_view string(std::string_view key) const
	{
		const bvalue *v = get(key);
		return v && v->type == 's' ? v->str : std::string_view();
	}
};

bool bdecode(std::string_view &in, bvalue &out, int depth = 0)
{
	if (in.empty() || depth > 4)
		return false;
	const char c = in[0];
	if (c == 'i')
	{
		const auto end = in.find('e');
		if (end == std::string_view::npos)
			return false;
		out.type = 'i';
		out.str = in.substr(1, end - 1);
		in.remove_prefix(end + 1);
		return true;
	}
	if (c == 'l' || c == 'd')
	{
		out.type = c;
		in.remove_prefix(1);
		while (!in.empty() && in[0] != 'e')
			if (!bdecode(in, out.items.emplace_back(), depth + 1))
				return false;
		if (in.empty())
			return false;
		in.remove_prefix(1);
		return c == 'l' || out.items.size() % 2 == 0;
	}
	size_t length = 0, i = 0;
	while (i < in.size() && i < 6 && in[i] >= '0' && in[i] <= '9')
		length = length * 10 + (in[i++] - '0');
	if (i == 0 || i >= in.size() || in[i] != ':' || in.size() - i - 1 < length)
		return false;
	out.type = 's';
	out.str = in.substr(i + 1, length);
	in.remove_prefix(i + 1 + length);
	return true;
}

std::string bstring(std::string_view s)
{
	return std::to_string(s.size()) + ':' + std::string(s);
}

std::string_view chars(const netplay_dht::key &k)
{
	return std::string_view(reinterpret_cast<const char *>(k.data()), k.size());
}

// an id that shares its first 32 bits with the target: of the about 20 million nodes, the closest
// share about 25. Made up to be the closest (Sybil nodes), they keep no announces, and in the K
// closest they hide the real ones: the host and its players then announce and look in different places
bool made_up(const netplay_dht::key &id, const netplay_dht::key &target)
{
	return std::equal(id.begin(), id.begin() + 4, target.begin());
}

// 4 bytes of address and 2 of port, big-endian
udp::endpoint compact(const char *data)
{
	const u8 *p = reinterpret_cast<const u8 *>(data);
	return udp::endpoint(asio::ip::address_v4(get_u32be(p)), get_u16be(p + 4));
}

} // anonymous namespace


netplay_dht::netplay_dht(udp::socket &socket, std::string_view bootstrap)
	: m_socket(socket)
{
	// random, from the system: it is sent to strangers
	std::random_device device;
	for (u8 &b : m_id)
		b = u8(device());
	m_target = m_id;

	udp::resolver resolver(m_socket.get_executor());
	std::istringstream servers{ std::string(bootstrap) };
	std::string server;
	while (std::getline(servers, server, ';'))
	{
		const auto colon = server.rfind(':');
		if (colon == std::string::npos)
			continue;
		asio::error_code ec;
		auto results = resolver.resolve(udp::v4(), server.substr(0, colon), server.substr(colon + 1), ec);
		if (ec || results.empty())
			osd_printf_verbose("Netplay: cannot resolve the DHT node %s\n", server);
		else
			m_bootstrap.push_back(*results.begin());
	}
}

void netplay_dht::start(const key &target, bool announce)
{
	m_target = target;
	m_announce = announce;
	m_nodes.clear();
	m_peers.clear();
	for (const udp::endpoint &endpoint : m_bootstrap)
	{
		// no id yet: the farthest from the target
		key farthest;
		for (size_t i = 0; i < farthest.size(); i++)
			farthest[i] = target[i] ^ 0xff;
		add_node(farthest, endpoint);
	}
}

void netplay_dht::tick(u32 now)
{
	if (now - m_step_ms >= STEP_MS)
	{
		m_step_ms = now;
		m_nodes.erase(
				std::remove_if(m_nodes.begin(), m_nodes.end(), [now] (const node &n) { return n.queried && !n.answered && now - n.asked_ms >= ANSWER_MS; }),
				m_nodes.end());
		// a Kademlia lookup: ask the closest nodes, which tell closer ones
		int asked = 0;
		for (size_t i = 0; i < std::min(K, m_nodes.size()) && asked < ALPHA; i++)
		{
			node &n = m_nodes[i];
			if (!n.queried)
			{
				n.queried = true;
				n.asked_ms = now;
				get_peers(n.endpoint);
				asked++;
			}
		}
	}

	if (now - m_refresh_ms >= REFRESH_MS)
	{
		m_refresh_ms = now;
		size_t asked = 0;
		for (const node &n : m_nodes)
		{
			if (n.answered && asked < K)
			{
				asked++;
				get_peers(n.endpoint);
				if (m_announce && !n.token.empty())
					announce(n);
			}
		}
		if (m_nodes.empty())
			start(m_target, m_announce); // nobody ever answered: the network may have come up later
	}
}

bool netplay_dht::handle(const udp::endpoint &from, const u8 *data, size_t size)
{
	if (!size || data[0] != 'd')
		return false;

	std::string_view in(reinterpret_cast<const char *>(data), size);
	bvalue message;
	auto const n = std::find_if(m_nodes.begin(), m_nodes.end(), [&from] (const node &n) { return n.endpoint == from; });
	if (n == m_nodes.end() || !bdecode(in, message) || message.string("y") != "r")
		return true;
	const bvalue *r = message.get("r");
	const std::string_view id = r ? r->string("id") : std::string_view();
	if (id.size() != n->id.size())
		return true;

	key answered;
	std::copy(id.begin(), id.end(), answered.begin());
	if (made_up(answered, m_target))
	{
		m_nodes.erase(n);
		return true;
	}
	n->answered = true;
	const std::string_view token = r->string("token");
	if (!token.empty() && token.size() <= 64)
		n->token = token;
	if (answered != n->id)
	{
		// a node of the bootstrap, or one another node told a wrong id of: its place among the closest
		n->id = answered;
		sort_nodes();
	}
	const std::string_view ip = message.string("ip");
	if (ip.size() == 6)
		m_seen[from] = compact(ip.data());

	if (const bvalue *values = r->get("values"); values && values->type == 'l')
	{
		for (const bvalue &v : values->items)
		{
			if (v.type != 's' || v.str.size() != 6)
				continue;
			const udp::endpoint peer = compact(v.str.data());
			if (peer.port() && m_peers.size() < MAX_PEERS && std::find(m_peers.begin(), m_peers.end(), peer) == m_peers.end())
				m_peers.push_back(peer);
		}
	}
	const std::string_view nodes = r->string("nodes");
	for (size_t i = 0; i + 26 <= nodes.size(); i += 26)
	{
		key other;
		std::copy_n(nodes.begin() + i, other.size(), other.begin());
		const udp::endpoint endpoint = compact(nodes.data() + i + 20);
		if (endpoint.port())
			add_node(other, endpoint);
	}
	return true;
}

std::optional<asio::ip::udp::endpoint> netplay_dht::public_address() const
{
	for (const auto &[node, seen] : m_seen)
		if (std::count_if(m_seen.begin(), m_seen.end(), [&seen = seen] (const auto &s) { return s.second == seen; }) >= 2)
			return seen;
	return std::nullopt;
}

bool netplay_dht::symmetric_nat() const
{
	return m_seen.size() >= 3 && !public_address();
}

void netplay_dht::get_peers(const udp::endpoint &to)
{
	send(to, "d1:ad2:id" + bstring(chars(m_id)) + "9:info_hash" + bstring(chars(m_target))
			+ "e1:q9:get_peers2:roi1e1:t2:gp1:y1:qe");
}

void netplay_dht::announce(const node &n)
{
	// implied_port: the port the node sees, which is the one the router opened
	send(n.endpoint, "d1:ad2:id" + bstring(chars(m_id)) + "12:implied_porti1e9:info_hash" + bstring(chars(m_target))
			+ "4:porti" + std::to_string(m_socket.local_endpoint().port()) + "e5:token" + bstring(n.token)
			+ "e1:q13:announce_peer2:roi1e1:t2:ap1:y1:qe");
}

void netplay_dht::send(const udp::endpoint &to, const std::string &message)
{
	asio::error_code ec;
	m_socket.send_to(asio::buffer(message), to, 0, ec);
}

void netplay_dht::add_node(const key &id, const udp::endpoint &endpoint)
{
	if (id == m_id || made_up(id, m_target) || std::any_of(m_nodes.begin(), m_nodes.end(), [&endpoint] (const node &n) { return n.endpoint == endpoint; }))
		return;
	node &n = m_nodes.emplace_back();
	n.endpoint = endpoint;
	n.id = id;
	sort_nodes();
	if (m_nodes.size() > MAX_NODES)
		m_nodes.resize(MAX_NODES);
}

// closest to the target first
void netplay_dht::sort_nodes()
{
	const key &target = m_target;
	std::stable_sort(m_nodes.begin(), m_nodes.end(), [&target] (const node &a, const node &b)
	{
		for (size_t i = 0; i < target.size(); i++)
			if ((a.id[i] ^ target[i]) != (b.id[i] ^ target[i]))
				return (a.id[i] ^ target[i]) < (b.id[i] ^ target[i]);
		return false;
	});
}
