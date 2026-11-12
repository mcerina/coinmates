// license:BSD-3-Clause
/***************************************************************************

    netplay_meet.h

    Where netplay players meet without a server of our own: public
    BitTorrent trackers (BEP 15, over UDP). Every player of a room
    announces itself under a hash of the room code, and the trackers
    answer with the others. A tracker takes the address the packet comes
    from but the port written in it, so STUN servers tell the public port
    first; two of them seeing different ports mean a symmetric NAT.

***************************************************************************/

#ifndef MAME_EMU_NETPLAY_MEET_H
#define MAME_EMU_NETPLAY_MEET_H

#pragma once

#include "asio.h"

#include <array>
#include <memory>
#include <optional>
#include <random>
#include <string_view>
#include <utility>
#include <vector>


class netplay_meet
{
public:
	using udp = asio::ip::udp;
	using hash = std::array<u8, 20>;

	// a choice: a joiner waits about this long; the trackers ask for 15 to 60 minutes between announces,
	// but 15 minutes of a request every 3 s brought no error and no block
	static constexpr u32 POLL_MS = 3000;
	// the slowest answer of a chosen tracker took 0.70 s from Italy; Australia is 0.48 s farther, from a phone
	static constexpr u32 ANSWER_MS = 1200;
	// between phones in Italy and Australia: 0.476 s at most from a phone here to servers there, and a
	// phone added 0.175 s at most to the same servers
	static constexpr u32 FAR_RTT_MS = 650;

	// trackers, stun: host:port separated by ;
	netplay_meet(udp::socket &socket, std::string_view trackers, std::string_view stun, const hash &room);

	// poll: ask the trackers for the others every POLL_MS, besides announcing once
	void tick(u32 now, bool poll);

	// true if the packet was for this
	bool handle(const udp::endpoint &from, const u8 *data, size_t size);

	// the socket was opened again: announce the new one
	void restart();

	// the others, as the trackers listed them last, without our own addresses
	const std::vector<udp::endpoint> &peers() const { return m_peers; }
	bool peers_changed() { return std::exchange(m_changed, false); }

	std::optional<udp::endpoint> public_address() const;
	bool symmetric_nat() const;
	bool tracker_answered() const { return m_answered; }
	// asked twice, a poll apart, and no tracker answered
	bool trackers_silent() const { return !m_answered && m_asked_ms && m_now - *m_asked_ms >= POLL_MS + ANSWER_MS; }

	// when a tracker first took the announce of this socket
	std::optional<u32> announced_ms() const { return m_announced_ms; }

private:
	struct server
	{
		udp::endpoint endpoint;
		u32 transaction = 0;            // of the request waiting for an answer; 0, none: a forged answer can't come after
		u8 stun_transaction[12] = { };
		std::optional<udp::endpoint> seen; // STUN: our address as it saw it
		bool connected = false;         // tracker: the connection id is valid
		u64 connection = 0;
		u32 connected_ms = 0;
		bool announced = false;
		std::vector<udp::endpoint> peers;
	};
	struct resolved;

	void new_stun_transaction(server &s);
	void ask_stun(server &s);
	void ask_tracker(server &t);
	void send(const udp::endpoint &to, const u8 *data, size_t size);
	std::optional<u16> public_port() const;
	void update_peers();

	udp::socket &m_socket;
	std::shared_ptr<resolved> m_resolved;   // filled by a thread for each name: one can take seconds
	bool m_stun_ready = false;          // every STUN name resolved, or failed to
	std::vector<server> m_trackers;
	std::vector<server> m_stun;
	hash m_room;
	std::array<u8, 20> m_peer_id;
	u32 m_key = 0;
	std::mt19937 m_rng;
	std::vector<udp::endpoint> m_own;       // of every socket, as STUN saw them
	std::vector<udp::endpoint> m_peers;
	bool m_changed = false;
	bool m_answered = false;
	std::optional<u32> m_announced_ms;
	u32 m_now = 0;
	std::optional<u32> m_stun_ms;
	std::optional<u32> m_stun_first_ms;     // of this socket
	std::optional<u32> m_asked_ms;          // the first request to a tracker
	std::optional<u32> m_poll_ms;           // the last announce of this socket
};

#endif // MAME_EMU_NETPLAY_MEET_H
