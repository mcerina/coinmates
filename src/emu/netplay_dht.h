// license:BSD-3-Clause
/***************************************************************************

    netplay_dht.h

    Where netplay players meet without a server of our own: the BitTorrent
    DHT (BEP 5). Every player of a room announces itself on a key derived
    from the room code, and asks the nodes closest to that key for the
    others. The nodes take the address and port the packets come from, so
    what they give back are public addresses, of sockets whose routers have
    already sent packets out. Their answers also say what our own address
    looks like from outside (BEP 42).

    Only a client: it answers no queries, and says so (BEP 43).

***************************************************************************/

#ifndef MAME_EMU_NETPLAY_DHT_H
#define MAME_EMU_NETPLAY_DHT_H

#pragma once

#include "asio.h"

#include <array>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>


class netplay_dht
{
public:
	using udp = asio::ip::udp;
	using key = std::array<u8, 20>;

	// bootstrap: host:port separated by ;
	netplay_dht(udp::socket &socket, std::string_view bootstrap, std::mt19937 &rng);

	// look for whoever announced itself on the key, and announce this socket if asked
	void start(const key &target, bool announce);
	void tick(u32 now);

	// true if the packet was for the DHT
	bool handle(const udp::endpoint &from, const u8 *data, size_t size);

	const std::vector<udp::endpoint> &peers() const { return m_peers; }

	// our address as at least two nodes saw it
	std::optional<udp::endpoint> public_address() const;

	// three nodes or more saw three different addresses: the router changes port for every destination
	bool symmetric_nat() const;

private:
	struct node
	{
		udp::endpoint endpoint;
		key id;
		bool queried = false;
		bool answered = false;
		u32 asked_ms = 0;
		std::string token;
	};

	void get_peers(const udp::endpoint &to);
	void announce(const node &n);
	void send(const udp::endpoint &to, const std::string &message);
	void add_node(const key &id, const udp::endpoint &endpoint);

	udp::socket &m_socket;
	std::mt19937 &m_rng;
	std::vector<udp::endpoint> m_bootstrap;
	key m_id;
	key m_target;
	bool m_announce = false;
	std::vector<node> m_nodes;                      // closest to the target first
	std::vector<udp::endpoint> m_peers;
	std::map<udp::endpoint, udp::endpoint> m_seen;  // per node, our address as it saw it
	u32 m_step_ms = 0;
	u32 m_refresh_ms = 0;
};

#endif // MAME_EMU_NETPLAY_DHT_H
