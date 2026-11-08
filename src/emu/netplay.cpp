// license:BSD-3-Clause
/***************************************************************************

    netplay.cpp

    Rollback netplay session between 2 to 4 instances over UDP.

    Enabled by -netplay_host <port>, which makes a room, and -netplay_join
    <room code or host:port>; players meet through the DHT (netplay_dht.h)
    or by broadcast at home. Every player
    sends its own inputs to all the others; missing inputs are predicted and
    corrected by rolling back. The session starts from the host's state, and
    the same transfer repairs a client whose state hash stops matching the
    host's. Everything runs on the main thread: the socket is polled at every
    frame boundary.

    Every packet carries a tag made with a key derived from the room code: only
    who knows the code takes part. A player who typed the host's address
    instead of the code is let in only from a private network or a VPN.

    The players connect to each other while they wait for the host to start.
    One who leaves during the game is replaced by neutral inputs: the others
    play on.

    Test hooks come from the MAME_NETPLAY_TEST environment variable (key=value
    separated by commas):
      bot=<seed>        a deterministic bot plays instead of the local player
      frames=<n>        leave at frame n
      log=<path>        write <path>.hash.csv and <path>.json
      sim=<ms>:<ms>:<%> delay, jitter and loss of every packet sent
      fault=<frame>     corrupt one byte of RAM at this frame
      dump=<from>:<to>  write <path>.<frame>.state for these confirmed frames
      draw=1            draw while resimulating: state computed while drawing then stops differing

***************************************************************************/

#include "emu.h"
#include "rollback.h"
#include "netplay_dht.h"

#include "emuopts.h"
#include "main.h"
#include "osdepend.h"
#include "screen.h"
#include "video.h"

#include "asio.h"
#include "hashing.h"
#include "multibyte.h"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <deque>
#include <map>
#include <random>
#include <sstream>
#include <string_view>
#include <vector>


namespace {

using udp = asio::ip::udp;

constexpr u32 MAGIC = 0x4d4e5031;
constexpr u32 PROTOCOL = 6;
constexpr int INPUT_RING = 1024;        // frames of input kept: must cover a state transfer
constexpr int MAX_INPUTS_PER_PACKET = 64;
constexpr u32 CHUNK_SIZE = 1200;
constexpr u32 CHUNK_WINDOW = 256;       // chunks in flight: an acknowledgement has a bit for each
constexpr u32 CHUNKS_PER_MS = 2;        // about 2.4 MB/s
constexpr u32 CHUNK_RESEND_MS = 200;
constexpr u32 PEER_TIMEOUT_MS = 10000;
constexpr u32 MESH_TIMEOUT_MS = 15000;
constexpr s64 LEFT_REPEAT = 300;        // frames during which the host repeats who left
constexpr size_t TAG_SIZE = 8;
constexpr u32 JOIN_TIMEOUT_MS = 30000;  // finding the room takes seconds; waiting for the start has no limit
constexpr u32 RESYNC_RETRY_MS = 5000;
constexpr u32 PUNCH_MS = 250;
constexpr u16 LAN_PORT = 7000;
constexpr u32 SWEEP_MS = 250;
constexpr u32 SWEEP_BATCH = 8;          // more fill the socket's buffer: packets to absent addresses wait seconds for ARP
constexpr char ROOM_LETTERS[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";  // no 0, 1, I, O: they look alike
constexpr size_t ROOM_LENGTH = 6;
constexpr s64 START_FRAME = 1;
constexpr s64 NO_FRAME = std::numeric_limits<s64>::max();

enum packet_type : u8 { P_HELLO = 1, P_WELCOME, P_REJECT, P_START, P_GO, P_INPUT, P_HASH, P_RESYNC, P_CHUNK, P_CHUNK_ACK, P_BYE, P_PUNCH, P_MEET, P_LEFT };

// pasted addresses come with spaces or a final full stop
std::string clean_address(std::string text)
{
	text.erase(std::remove_if(text.begin(), text.end(), [] (char c) { return std::isspace(u8(c)); }), text.end());
	while (!text.empty() && text.back() == '.')
		text.pop_back();
	return text;
}

std::string clean_room(std::string_view text)
{
	std::string room;
	for (char c : text)
		if (std::isalnum(u8(c)))
			room += char(std::toupper(u8(c)));
	return room;
}

std::string room_name(const std::string &room)
{
	return room.substr(0, 3) + '-' + room.substr(3);
}

util::sha1_t room_hash(const char *what, const std::string &room)
{
	const std::string text = util::string_format("coinmates %s %s", what, room);
	return util::sha1_creator::simple(text.data(), text.size());
}

// the key of the room's packets; without a code (an address was typed), the one everybody knows
util::sha1_t room_key(const std::string &room) { return room_hash("key", room); }

// HMAC-SHA1, cut to 64 bits: only who has the key can make it
u64 packet_tag(const util::sha1_t &key, const u8 *data, size_t size)
{
	u8 pad[64] = { };
	std::copy_n(key.m_raw, sizeof(key.m_raw), pad);
	for (u8 &b : pad)
		b ^= 0x36;
	util::sha1_creator inner;
	inner.append(pad, sizeof(pad));
	inner.append(data, u32(size));
	const util::sha1_t digest = inner.finish();
	for (u8 &b : pad)
		b ^= 0x36 ^ 0x5c;
	util::sha1_creator outer;
	outer.append(pad, sizeof(pad));
	outer.append(digest.m_raw, sizeof(digest.m_raw));
	return get_u64le(outer.finish().m_raw);
}

bool home_address(const asio::ip::address &address)
{
	const u32 a = address.is_v4() ? address.to_v4().to_uint() : 0;
	return (a >> 24) == 10 || (a >> 20) == 0xac1 || (a >> 16) == 0xc0a8;
}

// also a VPN such as Tailscale (100.64/10), or this computer
bool private_address(const asio::ip::address &address)
{
	const u32 a = address.is_v4() ? address.to_v4().to_uint() : 0;
	return home_address(address) || (a >> 22) == (0x64400000u >> 22) || (a >> 24) == 127;
}

u64 hash_bytes(u64 h, const void *data, size_t size)
{
	const u8 *p = reinterpret_cast<const u8 *>(data);
	for (size_t i = 0; i < size; i++)
		h = (h ^ p[i]) * 0x100000001b3ULL;
	return h;
}

template <typename T> u64 hash_value(u64 h, T value) { return hash_bytes(h, &value, sizeof(value)); }

constexpr u64 HASH_SEED = 0xcbf29ce484222325ULL;

u32 now_ms() { return u32(osd_ticks() / (osd_ticks_per_second() / 1000)); }


// everything read comes from the network: the reader checks the bounds
class packet_writer
{
public:
	packet_writer(packet_type type, u32 session, int sender)
	{
		put<u32>(MAGIC);
		put<u32>(session);
		put<u8>(type);
		put<u8>(u8(sender));
	}

	template <typename T> void put(T value)
	{
		for (size_t i = 0; i < sizeof(T); i++)
			m_data.push_back(u8(u64(value) >> (i * 8)));
	}

	void put_string(std::string_view s)
	{
		put<u16>(u16(s.size()));
		m_data.insert(m_data.end(), s.begin(), s.end());
	}

	void put_bytes(const u8 *data, size_t size) { m_data.insert(m_data.end(), data, data + size); }

	std::vector<u8> tagged(const util::sha1_t &key) const
	{
		std::vector<u8> data(m_data);
		data.resize(m_data.size() + TAG_SIZE);
		put_u64le(&data[m_data.size()], packet_tag(key, m_data.data(), m_data.size()));
		return data;
	}

private:
	std::vector<u8> m_data;
};

class packet_reader
{
public:
	packet_reader(const u8 *data, size_t size) : m_data(data), m_size(size) { }

	template <typename T> T get()
	{
		if (m_size < sizeof(T))
		{
			m_ok = false;
			return T(0);
		}
		u64 v = 0;
		for (size_t i = 0; i < sizeof(T); i++)
			v |= u64(m_data[i]) << (i * 8);
		m_data += sizeof(T);
		m_size -= sizeof(T);
		return T(v);
	}

	std::string get_string()
	{
		const size_t n = get<u16>();
		if (n > m_size)
		{
			m_ok = false;
			return std::string();
		}
		std::string s(reinterpret_cast<const char *>(m_data), n);
		m_data += n;
		m_size -= n;
		return s;
	}

	const u8 *remaining() const { return m_data; }
	size_t remaining_size() const { return m_size; }
	bool ok() const { return m_ok; }

private:
	const u8 *m_data;
	size_t m_size;
	bool m_ok = true;
};


class netplay_session : public rollback_session
{
public:
	netplay_session(running_machine &machine, bool host);
	~netplay_session();

	bool in_session() const override { return m_phase == phase::RUNNING; }
	void frame_inputs_begin() override;
	void frame_inputs_port(ioport_port &port) override;
	void post_timeslice() override;

private:
	enum class phase { BOOT, LOBBY, RUNNING, ENDED };

	struct transfer_out
	{
		bool active = false;
		std::shared_ptr<const std::vector<u8>> data;
		u32 id = 0;
		s64 frame = 0;
		u32 total = 0;
		u32 next_missing = 0;
		std::array<u64, CHUNK_WINDOW / 64> received_after = { }; // bit i: chunk next_missing + 1 + i
		u32 send_pos = 0;
		u32 last_tick_ms = 0;
		u32 last_pass_ms = 0;
	};

	struct transfer_in
	{
		bool active = false;
		bool ready = false;
		u32 id = 0;
		u32 done_id = 0;
		s64 frame = 0;
		u32 total = 0;
		u32 compressed = 0;
		u32 next_missing = 0;
		std::vector<u8> have;
		std::vector<u8> data;
		std::vector<u8> raw;
	};

	struct peer
	{
		bool connected = false;
		udp::endpoint endpoint;
		u64 nonce = 0;              // the same player, whichever way its packets come
		u32 last_rx_ms = 0;
		s64 acked = START_FRAME;
		s64 frame = START_FRAME;
		u32 frame_rx_ms = 0;
		u32 echo_time = 0;          // sent back, for the round trip
		u32 echo_rx_ms = 0;
		float rtt = 0;
		float advantage = 0;        // frames I am ahead of them
		float their_advantage = 0;  // as they see it
		transfer_out tx;
		bool session_ready = false;
		bool resync_wanted = false;
		s64 resync_frame = 0;       // its present: the state sent can't be newer
		s64 resync_check = 0;
		// in the lobby, host only: where it is at home and on the internet (as it says: the host may
		// see it at home), which players it reached, since when it's in
		udp::endpoint lan;
		udp::endpoint wan;
		std::vector<u64> met;
		u32 joined_ms = 0;
		// after its last frame its inputs are neutral; the host repeats it until left_until
		s64 left = NO_FRAME;
		s64 left_until = 0;
	};

	// another client in the lobby, as the host sees it
	struct other
	{
		u64 nonce;
		udp::endpoint endpoint;
		udp::endpoint lan;
		udp::endpoint wan;
	};

	struct hash_entry
	{
		s64 frame = -1;
		u64 mine = 0;
		u64 host = 0;
		bool have_mine = false;
		bool have_host = false;
	};

	struct checked_state
	{
		s64 frame = -1;
		std::vector<u8> data;
	};

	struct delayed_packet
	{
		u32 time;
		udp::endpoint endpoint;
		std::vector<u8> data;
	};

	void compute_signature();
	void start_session_buffers();

	u32 best(int player, s64 frame) const;
	u32 frame_word(int player, s64 frame) const override { return player == m_me ? m_input[player][frame % INPUT_RING] : m_used[player][frame % INPUT_RING]; }
	void apply_frame_to_ports(s64 frame) override;

	void resimulate(s64 from, s64 to, bool load = true);
	void resim_frame_done() override;
	// what's computed while drawing must be the same for all in the compared frames
	bool resim_draws(s64 frame) const override { return m_resim_draw || frame % m_hash_every == 0; }
	void rollback(s64 boundary);
	void corrupt_state(s64 frame);

	void run_lobby();
	void lobby_tick();
	void run_frame();
	void stall_tick();
	void finish_lobby();

	void advance_confirmed(s64 boundary);
	void hash_frame(s64 frame);
	void compare_hash(hash_entry &entry);
	void request_resync(s64 boundary, s64 frame);
	void apply_received_state(s64 boundary);
	void log_differences(const std::vector<u8> &mine, const std::vector<u8> &host);
	void serve_resyncs();

	void update_time_sync(s64 boundary);

	void send(const udp::endpoint &to, const packet_writer &packet) { send(to, packet, m_key); }
	void send(const udp::endpoint &to, const packet_writer &packet, const util::sha1_t &key);
	void send_all(const packet_writer &packet);
	void send_bye();
	void poll();
	void flush_simulated(bool all = false);
	void handle_packet(const udp::endpoint &from, const u8 *data, size_t size);
	void handle_hello(const udp::endpoint &from, packet_reader &in, const util::sha1_t &key);
	void meet_tick(u32 now);
	bool meshed(int joined, int &stuck) const;
	void remove_player(int player, const char *reason);
	void drop_player(int player);
	void player_left(int player, s64 last, s64 from);
	void handle_left(packet_reader &in);
	void send_left();
	void begin(int players);
	void sweep_lan(const packet_writer &packet);
	void handle_welcome(const udp::endpoint &from, packet_reader &in);
	void handle_start(packet_reader &in);
	void handle_input(int sender, packet_reader &in);
	void handle_hash(packet_reader &in);
	void handle_chunk(packet_reader &in);
	void handle_chunk_ack(int sender, packet_reader &in);
	void send_inputs();
	void send_welcome(int player, const util::sha1_t &key);
	void send_start(int player);
	void send_chunk_ack();
	void start_transfer(int player, s64 frame, const std::vector<u8> &state);
	void tick_transfers();
	void check_timeouts();
	void end_session(const char *reason);

	void write_log();

	phase m_phase = phase::BOOT;
	const bool m_host;
	std::string m_end_reason;

	// the host's options, sent to the clients
	u32 m_session = 0;
	int m_me = 0;
	int m_delay = 1;
	int m_window = 10;
	int m_hash_every = 30;
	u64 m_base_time = 0;

	u64 m_layout_hash = 0, m_rom_hash = 0, m_input_hash = 0;

	bool m_input_callbacks = false;     // they write machine state

	std::vector<u32> m_input[MAX_PLAYERS];
	std::vector<u32> m_used[MAX_PLAYERS];   // remote players only: what the frame applied
	s64 m_known[MAX_PLAYERS];

	s64 m_rollback_to = NO_FRAME;
	s64 m_floor = START_FRAME;      // no rollback before this frame: the state came from the host
	s64 m_confirmed = START_FRAME;
	std::vector<hash_entry> m_hashes;
	checked_state m_checked[2];     // a repair starts from the hashed frame that differed
	bool m_resync_pending = false;
	u32 m_resync_ms = 0;

	float m_frame_ms = 1000.0f / 60;
	int m_base_speed = 1000;
	bool m_slowed = false;

	asio::io_context m_io;
	udp::socket m_socket;
	udp::endpoint m_host_endpoint;
	peer m_peers[MAX_PLAYERS];
	transfer_in m_rx;
	bool m_send_ack = false;
	u32 m_next_transfer_id = 1;
	bool m_all_joined = false;
	u32 m_lobby_start_ms = 0;
	u32 m_last_hello_ms = 0;
	u32 m_last_draw_ms = 0;

	std::string m_room;
	util::sha1_t m_key;
	u64 m_nonce = 0;
	std::vector<other> m_others;    // client: the other clients, until the start
	std::map<u64, udp::endpoint> m_met;  // client: where the others' packets come from
	u32 m_meet_ms = 0;
	u32 m_mesh_ms = 0;
	std::unique_ptr<netplay_dht> m_dht;
	bool m_nat_reported = false;
	size_t m_peers_reported = 0;
	u32 m_room_message_ms = 0;
	u32 m_punch_ms = 0;
	asio::ip::address_v4 m_lan;
	u32 m_sweep_ms = 0;
	u32 m_sweep_next = 1;           // last byte of the next address
	u32 m_send_failed = 0;
	int m_wait_for = 0;             // players at which the host starts by itself
	bool m_welcomed = false;
	bool m_started = false;
	bool m_go = false;
	bool m_loaded = false;
	u64 m_tx_bytes = 0, m_rx_bytes = 0;

	u32 m_bot_seed = 0;
	s64 m_test_frames = 0;
	s64 m_test_fault = -1;
	long long m_test_dump_from = 0, m_test_dump_to = -1;
	std::string m_log_path;
	FILE *m_hash_log = nullptr;
	int m_sim_delay = 0, m_sim_jitter = 0, m_sim_loss = 0;
	std::mt19937 m_rng;
	std::deque<delayed_packet> m_sim_queue;

	u64 m_rollbacks = 0, m_resim_frames = 0, m_unrecoverable = 0;
	u64 m_desyncs = 0, m_resyncs = 0, m_hash_compared = 0, m_stall_ms = 0, m_slow_frames = 0;
	u64 m_depth_histogram[64] = { };
	double m_rollback_ms_total = 0, m_rollback_ms_max = 0;
	osd_ticks_t m_session_start = 0;
};


//-------------------------------------------------
//  construction
//-------------------------------------------------

netplay_session::netplay_session(running_machine &machine, bool host)
	: rollback_session(machine)
	, m_host(host)
	, m_socket(m_io)
	, m_rng(std::random_device()())
{
	if (machine.options().cheat())
		throw emu_fatalerror("Netplay: cheats must be off");

	for (ioport_port *port : m_ports)
	{
		for (ioport_field &field : port->fields())
			if (field.is_analog())
				throw emu_fatalerror("Netplay: analog controls are not supported yet (%s)", field.name());
		for (auto &dyn : port->live().writelist)
			m_input_callbacks = m_input_callbacks || dyn.field().type() != IPT_OUTPUT;
	}
	compute_signature();

	if (host)
	{
		const int most = std::min(system_players(machine.ioport().ports()), MAX_PLAYERS);
		if (most < 2)
			throw emu_fatalerror("Netplay: %s has controls for one player only", machine.system().type.fullname());
		m_players = most;
		m_wait_for = machine.options().netplay_players() ? std::clamp(machine.options().netplay_players(), 2, most) : most;
		m_delay = std::clamp(machine.options().netplay_delay(), 0, 8);
		// on a Steam Deck the heaviest systems redo few frames within a frame: their inputs
		// are delayed more, enough for a ping of 100 ms (RISULTATI-rollback.md)
		static const std::pair<std::string_view, int> heavy[] = {
			{ "gdarius2", 3 }, { "legendoh", 3 }, { "raystorm", 3 }, { "rdft", 3 }, { "rdft2", 3 }, { "rfjet", 3 },
			{ "aoh", 2 }, { "battlera", 2 }, { "dmnfrnt", 2 }, { "dragnblz", 2 }, { "gunbird2", 2 }, { "kov2", 2 },
			{ "rthun2", 2 }, { "s1945ii", 2 }, { "s1945iii", 2 }, { "soldivid", 2 }, { "thndzone", 2 } };
		const game_driver &system = machine.system();
		if (machine.options().get_entry(OPTION_NETPLAY_DELAY)->priority() == OPTION_PRIORITY_DEFAULT)
			for (const auto &[name, delay] : heavy)
				if (name == system.name || name == system.parent)
					m_delay = delay;
		m_window = std::clamp(machine.options().netplay_window(), 1, 60);
		m_hash_every = std::clamp(machine.options().netplay_hash_every(), 1, 3600);
		m_session = m_rng() | 1;
		// another port if taken: the code finds it anyway, and the host writes to the players at home (sweep_lan)
		asio::error_code busy;
		m_socket.open(udp::v4());
		m_socket.bind(udp::endpoint(udp::v4(), u16(machine.options().netplay_host_port())), busy);
		if (busy)
			m_socket.bind(udp::endpoint(udp::v4(), 0));
		const int port = m_socket.local_endpoint().port();
		for (size_t i = 0; i < ROOM_LENGTH; i++)
			m_room += ROOM_LETTERS[m_rng() % (sizeof(ROOM_LETTERS) - 1)];
		osd_printf_info("Netplay: room %s on UDP port %d: give the code to the others, then press Start (by itself with %d players)\n", room_name(m_room), port, m_wait_for);
	}
	else
	{
		const std::string target = clean_address(machine.options().netplay_join());
		const auto colon = target.rfind(':');
		if (colon == std::string::npos)
		{
			m_room = clean_room(target);
			if (m_room.size() != ROOM_LENGTH)
				throw emu_fatalerror("Netplay: -netplay_join needs a room code like K7Q-M3X, or host:port");
			osd_printf_info("Netplay: looking for room %s\n", room_name(m_room));
		}
		else
		{
			asio::error_code ec;
			udp::resolver resolver(m_io);
			auto results = resolver.resolve(udp::v4(), target.substr(0, colon), target.substr(colon + 1), ec);
			if (ec || results.empty())
				throw emu_fatalerror("Netplay: cannot resolve %s", target);
			m_host_endpoint = *results.begin();
			osd_printf_info("Netplay: joining %s\n", target);
		}
		// LAN_PORT if free: the host writes there (sweep_lan)
		asio::error_code busy;
		m_socket.open(udp::v4());
		m_socket.bind(udp::endpoint(udp::v4(), LAN_PORT), busy);
		if (busy)
			m_socket.bind(udp::endpoint(udp::v4(), 0));
		m_nonce = (u64(m_rng()) << 32 | m_rng()) | 1;
	}
	m_socket.non_blocking(true);
	asio::error_code ec;
	{
		// of the route to the internet: connecting a UDP socket sends nothing
		udp::socket probe(m_socket.get_executor());
		probe.open(udp::v4(), ec);
		probe.connect(udp::endpoint(asio::ip::make_address_v4("8.8.8.8"), 53), ec);
		const asio::ip::address_v4 address = ec ? asio::ip::address_v4() : probe.local_endpoint(ec).address().to_v4();
		if (!ec && home_address(address))
			m_lan = address;
	}
	m_socket.set_option(asio::socket_base::broadcast(true), ec);
	m_key = room_key(m_room);

	if (!m_room.empty() && *machine.options().netplay_dht())
	{
		m_dht = std::make_unique<netplay_dht>(m_socket, machine.options().netplay_dht(), m_rng);
		netplay_dht::key key;
		std::copy_n(room_hash("room", m_room).m_raw, key.size(), key.begin());
		m_dht->start(key, true);
	}

	if (screen_device *screen = screen_device_enumerator(machine.root_device()).first())
		m_frame_ms = float(screen->frame_period().as_double() * 1000.0);
	m_base_speed = machine.video().speed_factor();

	if (const char *cfg = std::getenv("MAME_NETPLAY_TEST"))
	{
		std::istringstream ss(cfg);
		std::string kv;
		while (std::getline(ss, kv, ','))
		{
			const auto eq = kv.find('=');
			if (eq == std::string::npos)
				continue;
			const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
			if (k == "bot") m_bot_seed = u32(std::stoul(v));
			else if (k == "frames") m_test_frames = std::stoll(v);
			else if (k == "fault") m_test_fault = std::stoll(v);
			else if (k == "draw") m_resim_draw = std::stoi(v) != 0;
			else if (k == "log") m_log_path = v;
			else if (k == "dump") std::sscanf(v.c_str(), "%lld:%lld", &m_test_dump_from, &m_test_dump_to);
			else if (k == "sim") std::sscanf(v.c_str(), "%d:%d:%d", &m_sim_delay, &m_sim_jitter, &m_sim_loss);
		}
		if (!m_log_path.empty())
			m_hash_log = std::fopen((m_log_path + ".hash.csv").c_str(), "w");
	}

	for (int p = 0; p < MAX_PLAYERS; p++)
	{
		m_input[p].assign(INPUT_RING, 0);
		m_used[p].assign(INPUT_RING, 0);
		m_known[p] = START_FRAME;
	}
}


netplay_session::~netplay_session()
{
	// a system started next from the menu plays alone, unless chosen again
	m_machine.options().set_value(OPTION_NETPLAY_HOST, 0, OPTION_PRIORITY_CMDLINE);
	m_machine.options().set_value(OPTION_NETPLAY_JOIN, "", OPTION_PRIORITY_CMDLINE);
	send_bye();
	write_log();
	if (m_hash_log)
		std::fclose(m_hash_log);
	if (m_slowed)
		m_machine.video().set_speed_factor(m_base_speed);
}


static bool user_setting(const ioport_field &field)
{
	return !field.settings().empty() || field.type() == IPT_ADJUSTER;
}

// what must be the same on all instances, and is not in the state
void netplay_session::compute_signature()
{
	m_layout_hash = HASH_SEED;
	for_each_item([this] (const char *name, size_t offset, size_t size)
	{
		m_layout_hash = hash_bytes(m_layout_hash, name, strlen(name));
		m_layout_hash = hash_value(m_layout_hash, u64(size));
	});

	std::vector<std::string> names;
	for (auto &r : m_machine.memory().regions())
		names.push_back(r.first);
	std::sort(names.begin(), names.end());
	m_rom_hash = HASH_SEED;
	for (auto &name : names)
	{
		memory_region *region = m_machine.memory().regions().find(name)->second.get();
		m_rom_hash = hash_bytes(m_rom_hash, name.data(), name.size());
		m_rom_hash = hash_bytes(m_rom_hash, region->base(), region->bytes());
	}

	// the DIP switch and configuration values are not in it: the players get the host's
	m_input_hash = HASH_SEED;
	for (ioport_port *port : m_ports)
		m_input_hash = hash_bytes(m_input_hash, port->tag(), strlen(port->tag()));
	for (ioport_type t : m_bit_type)
		m_input_hash = hash_value(m_input_hash, u32(t));
}


void netplay_session::start_session_buffers()
{
	allocate_slots(m_window + 3);
	m_hashes.assign(64, hash_entry());

	// sound samples are floating point, which builds may round differently; the chips' state stays
	compute_hash_skip("", true);
	for (int p = 0; p < m_players; p++)
		m_known[p] = START_FRAME + m_delay; // nobody has inputs before the first boundary
}


//-------------------------------------------------
//  inputs
//-------------------------------------------------

u32 netplay_session::best(int player, s64 frame) const
{
	if (frame > m_peers[player].left)
		return 0;
	return m_input[player][std::min(frame, m_known[player]) % INPUT_RING];
}

void netplay_session::frame_inputs_begin()
{
	if (m_phase == phase::BOOT)
		m_frame_ended = true;
	if (m_phase != phase::RUNNING)
		return;

	m_frame_ended = true;
	const s64 f = m_cur + 1;
	for (int p = 0; p < m_players; p++)
		if (p != m_me)
			m_used[p][f % INPUT_RING] = best(p, f);
	if (!m_resim)
		m_input[m_me][(f + m_delay) % INPUT_RING] = m_bot_seed ? bot_word(m_bot_seed, m_me, f + m_delay) : 0;
}

void netplay_session::frame_inputs_port(ioport_port &port)
{
	if (m_phase != phase::RUNNING)
	{
		port.live().digital = 0;
		return;
	}

	const int index = m_port_index[&port];
	const s64 f = m_cur + 1;
	if (!m_resim && !m_bot_seed)
	{
		// the local player's controls are those of player 1, whatever seat they got
		m_input[m_me][(f + m_delay) % INPUT_RING] |= local_word(index, port.live().digital);
	}
	port.live().digital = compose(index, f);
}

void netplay_session::apply_frame_to_ports(s64 frame)
{
	for (int p = 0; p < m_players; p++)
		if (p != m_me)
			m_used[p][frame % INPUT_RING] = best(p, frame);
	rollback_session::apply_frame_to_ports(frame);
}


//-------------------------------------------------
//  states
//-------------------------------------------------

void netplay_session::resimulate(s64 from, s64 to, bool load)
{
	const osd_ticks_t start = osd_ticks();
	if (load)
		load_state(slot_for(from).data);
	apply_frame_to_ports(from);

	// the slot was saved after the input callbacks ran with the inputs predicted
	// then: save it again, or its hash differs from the other machines' one
	if (m_input_callbacks)
		save_slot(from);
	run_frames(from, to);

	const double ms = double(osd_ticks() - start) * 1000.0 / osd_ticks_per_second();
	m_rollback_ms_total += ms;
	m_rollback_ms_max = std::max(m_rollback_ms_max, ms);
	m_depth_histogram[std::min<s64>(to - from, std::size(m_depth_histogram) - 1)]++;
	m_rollbacks++;
}

void netplay_session::resim_frame_done()
{
	save_slot(m_cur);
	m_resim_frames++;
}

void netplay_session::rollback(s64 boundary)
{
	const s64 oldest = std::max(m_floor, boundary - m_window);
	s64 frame = m_rollback_to;
	m_rollback_to = NO_FRAME;

	// a save can fail on anonymous timers
	while (frame >= oldest && !slot_valid(frame))
		frame--;
	if (frame < oldest)
	{
		// state lost: the hash comparison finds out and repairs it
		m_unrecoverable++;
		return;
	}

	// a wrong guess for the frame about to run: nothing has run since the state of
	// the present was saved, so only the inputs change (a load could also leave
	// events pending, from post_load handlers, that keep the state from being saved)
	resimulate(frame, boundary, frame != boundary);
}

void netplay_session::corrupt_state(s64 frame)
{
	if (!slot_valid(frame))
		return;

	// the middle of the first RAM that the hash looks at
	slot &s = slot_for(frame);
	size_t fault = 0;
	for_each_item([&] (std::string_view name, size_t offset, size_t size)
	{
		const bool skipped = std::find(m_hash_skip.begin(), m_hash_skip.end(), std::make_pair(offset, size)) != m_hash_skip.end();
		if (!fault && !skipped && name.substr(0, 7) == "memory/" && size >= 256)
			fault = offset + size / 2;
	});
	s.data[fault] ^= 0x5a;
	load_state(s.data);
	osd_printf_info("Netplay test: corrupted the state at frame %d\n", int(frame));
}


//-------------------------------------------------
//  lobby: wait for all players, then give them
//  the host's state
//-------------------------------------------------

void netplay_session::run_lobby()
{
	m_phase = phase::LOBBY;
	m_lobby_start_ms = now_ms();
	if (m_host)
	{
		m_me = 0;
		start_session_buffers();
		m_cur = START_FRAME;
		save_slot(START_FRAME);
		m_base_time = u64(m_machine.base_time());
		m_peers[0].connected = true;
	}

	while (m_phase == phase::LOBBY && !m_machine.exit_pending())
		lobby_tick();
}

void netplay_session::lobby_tick()
{
	poll();
	send_chunk_ack();
	if (m_phase != phase::LOBBY)
		return;

	const u32 now = now_ms();
	meet_tick(now);
	int joined = 1;
	while (joined < m_players && m_peers[joined].connected)
		joined++;
	int stuck = 0;
	const bool all_met = !m_host || meshed(joined, stuck);
	if ((m_host ? !m_all_joined : !m_started) && now - m_room_message_ms >= 1000)
	{
		// kept on screen, for whoever plays in front of a TV
		m_room_message_ms = now;
		if (m_host && joined < 2)
			m_machine.popmessage("Netplay: room %s\nwaiting for the other players", room_name(m_room));
		else if (m_host && !all_met)
			m_machine.popmessage("Netplay: room %s\n%d players: connecting them to each other", room_name(m_room), joined);
		else if (m_host)
			m_machine.popmessage("Netplay: room %s\n%d players: press Start to begin", room_name(m_room), joined);
		else if (m_welcomed)
			m_machine.popmessage("Netplay: joined room %s\nwaiting for the host to start", room_name(m_room));
		else if (m_room.empty())
			m_machine.popmessage("Netplay: joining");
		else
			m_machine.popmessage("Netplay: looking for room %s", room_name(m_room));
	}
	if (m_host)
	{
		for (int p = 1; p < m_players; p++)
		{
			if (!m_peers[p].connected || now - m_peers[p].last_rx_ms < PEER_TIMEOUT_MS)
				continue;
			if (m_all_joined)
				drop_player(p);
			else
				remove_player(p, nullptr);
			return;
		}
		if (all_met || m_all_joined)
			m_mesh_ms = 0;
		else if (!m_mesh_ms)
			m_mesh_ms = now;
		else if (now - m_mesh_ms >= MESH_TIMEOUT_MS && now - m_peers[stuck].joined_ms >= MESH_TIMEOUT_MS)
		{
			m_mesh_ms = 0;
			remove_player(stuck, "cannot connect to the other players: try Tailscale");
			return;
		}

		if (!m_all_joined && joined >= 2 && all_met && (joined >= m_wait_for || m_machine.ioport().type_pressed(IPT_START1)))
			begin(joined);
		if (!m_all_joined && now - m_sweep_ms >= SWEEP_MS)
		{
			m_sweep_ms = now;
			sweep_lan(packet_writer(P_PUNCH, 0, 0));
		}
		if (m_dht && !m_all_joined && now - m_punch_ms >= PUNCH_MS)
		{
			// the room's addresses: this opens the router to their packets
			m_punch_ms = now;
			for (const udp::endpoint &endpoint : m_dht->peers())
				send(endpoint, packet_writer(P_PUNCH, 0, 0));
		}
		tick_transfers();
		bool ready = m_all_joined;
		for (int p = 1; p < m_players; p++)
			ready = ready && (m_peers[p].session_ready || m_peers[p].left != NO_FRAME);
		if (ready)
			finish_lobby();
	}
	else
	{
		if (!m_started && now - m_last_hello_ms >= 250)
		{
			m_last_hello_ms = now;
			packet_writer out(P_HELLO, 0, 0);
			out.put<u32>(PROTOCOL);
			out.put_string(emulator_info::get_build_version());
			out.put_string(m_machine.system().name);
			out.put<u32>(u32(m_state_size));
			out.put<u64>(m_layout_hash);
			out.put<u64>(m_rom_hash);
			out.put<u64>(m_input_hash);
			out.put<u64>(m_nonce);
			const std::optional<udp::endpoint> wan = m_dht ? m_dht->public_address() : std::nullopt;
			out.put<u32>(m_lan.to_uint());
			out.put<u16>(m_socket.local_endpoint().port());
			out.put<u32>(wan ? wan->address().to_v4().to_uint() : 0);
			out.put<u16>(wan ? wan->port() : 0);
			out.put<u8>(u8(std::min<size_t>(m_met.size(), MAX_PLAYERS)));
			int listed = 0;
			for (const auto &met : m_met)
				if (listed++ < MAX_PLAYERS)
					out.put<u64>(met.first);
			if (m_welcomed || m_room.empty())
				send(m_host_endpoint, out);
			else
			{
				send(udp::endpoint(asio::ip::address_v4::broadcast(), LAN_PORT), out);
				if (now - m_sweep_ms >= SWEEP_MS)
				{
					m_sweep_ms = now;
					sweep_lan(out);
				}
				if (m_dht)
					for (const udp::endpoint &endpoint : m_dht->peers())
						send(endpoint, out);
			}
		}
		if (m_welcomed && !m_started && now - m_meet_ms >= PUNCH_MS)
		{
			// the other players, at home and through their routers: both sides
			// writing opens the routers (as with the host)
			m_meet_ms = now;
			packet_writer out(P_MEET, m_session, m_me);
			out.put<u64>(m_nonce);
			for (const other &o : m_others)
			{
				send(o.endpoint, out);
				if (o.lan.port() && o.lan != o.endpoint)
					send(o.lan, out);
				if (o.wan.port() && o.wan != o.endpoint)
					send(o.wan, out);
			}
		}
		if (!m_welcomed && now - m_lobby_start_ms >= JOIN_TIMEOUT_MS)
		{
			end_session(m_room.empty() ? "no answer from the host" : "room not found");
			return;
		}
		if (m_welcomed && now - m_peers[0].last_rx_ms >= PEER_TIMEOUT_MS)
		{
			end_session("no packets from the host");
			return;
		}
		if (m_rx.ready && !m_loaded)
		{
			// the machine is loaded when the session starts
			slot &s = slot_for(START_FRAME);
			s.data = std::move(m_rx.raw);
			s.frame = START_FRAME;
			s.valid = true;
			m_rx.ready = false;
			m_loaded = true;
			m_cur = START_FRAME;
		}
		if (m_loaded && m_started && now - m_last_hello_ms >= 100)
		{
			// resend the acknowledgement until the host answers
			m_last_hello_ms = now;
			m_send_ack = true;
			send_chunk_ack();
		}
		if (m_loaded && m_go && m_started)
			finish_lobby();
	}
	flush_simulated();

	// keep the UI alive; this does not run the machine, and the state is
	// loaded again at the end
	if (now - m_last_draw_ms >= 16)
	{
		m_last_draw_ms = now;
		if (m_phase == phase::LOBBY)
			m_machine.video().frame_update();
	}
	else
		osd_sleep(osd_ticks_per_second() / 1000);
}

void netplay_session::finish_lobby()
{
	if (m_host)
		for (int i = 0; i < 3; i++)
			send_all(packet_writer(P_GO, m_session, m_me));
	m_cur = START_FRAME;
	load_state(slot_for(START_FRAME).data);
	// every frame drawn, so that the compared ones are (video_manager::set_frameskip)
	m_machine.video().set_frameskip(0);
	m_phase = phase::RUNNING;
	apply_frame_to_ports(START_FRAME);
	const u32 now = now_ms();
	for (int p = 0; p < m_players; p++)
	{
		m_peers[p].last_rx_ms = now;
		m_peers[p].frame_rx_ms = now;
	}
	m_session_start = osd_ticks();
	m_machine.popmessage("Netplay: player %d of %d", m_me + 1, m_players);
	osd_printf_info("Netplay: session started as player %d of %d, delay %d, window %d\n", m_me + 1, m_players, m_delay, m_window);
}


//-------------------------------------------------
//  one frame of the session
//-------------------------------------------------

void netplay_session::post_timeslice()
{
	if (!m_frame_ended)
		return;
	m_frame_ended = false;

	if (m_phase == phase::BOOT)
	{
		// the first boundary with a state that can be saved
		if (m_machine.scheduler().can_save())
			run_lobby();
	}
	else if (m_phase == phase::RUNNING)
		run_frame();
}

void netplay_session::run_frame()
{
	s64 b = ++m_cur;
	m_known[m_me] = b + m_delay;
	const bool saved = save_slot(b);
	if (b == m_test_fault)
		corrupt_state(b);

	poll();
	send_chunk_ack();
	send_inputs();

	while (m_phase == phase::RUNNING && !m_machine.exit_pending())
	{
		// with anonymous timers pending neither save nor load is safe: retry at the next boundary
		if (saved)
		{
			if (m_rx.ready)
			{
				apply_received_state(b);
				b = m_cur;
			}
			else if (m_rollback_to != NO_FRAME)
				rollback(b);
		}
		advance_confirmed(b);

		// too far ahead of the slowest player: wait, the state to roll back to would be lost
		s64 slowest = b;
		for (int p = 0; p < m_players; p++)
			if (p != m_me)
				slowest = std::min(slowest, m_known[p]);
		if (b - slowest < m_window)
			break;
		stall_tick();
		if (!saved && m_rollback_to != NO_FRAME)
			break;
	}

	serve_resyncs();
	tick_transfers();
	flush_simulated();
	update_time_sync(b);
	check_timeouts();

	if (m_test_frames && b >= m_test_frames)
		end_session("test finished");
}

void netplay_session::stall_tick()
{
	const u32 start = now_ms();
	osd_sleep(osd_ticks_per_second() / 1000);
	poll();
	send_chunk_ack();
	send_inputs();
	tick_transfers();
	flush_simulated();
	check_timeouts();
	m_machine.osd().update(true);
	m_stall_ms += now_ms() - start;
}


//-------------------------------------------------
//  desync detection and repair
//-------------------------------------------------

void netplay_session::advance_confirmed(s64 boundary)
{
	s64 confirmed = boundary;
	for (int p = 0; p < m_players; p++)
		confirmed = std::min(confirmed, m_known[p]);
	if (m_rollback_to != NO_FRAME)
		confirmed = std::min(confirmed, m_rollback_to - 1);
	for (s64 f = m_confirmed + 1; f <= confirmed; f++)
		if (f % m_hash_every == 0)
			hash_frame(f);
	m_confirmed = std::max(m_confirmed, confirmed);
}

void netplay_session::hash_frame(s64 frame)
{
	if (!slot_valid(frame))
		return;
	checked_state &c = m_checked[(frame / m_hash_every) % 2];
	c.frame = frame;
	c.data = slot_for(frame).data;
	const u64 h = state_hash(c.data);
	if (frame >= m_test_dump_from && frame <= m_test_dump_to)
		if (FILE *f = std::fopen(util::string_format("%s.%d.state", m_log_path, int(frame)).c_str(), "wb"))
		{
			std::fwrite(slot_for(frame).data.data(), 1, m_state_size, f);
			std::fclose(f);
		}
	if (m_hash_log)
		std::fprintf(m_hash_log, "%lld,%016llx\n", (long long)frame, (unsigned long long)h);
	if (m_host)
	{
		packet_writer out(P_HASH, m_session, m_me);
		out.put<s32>(s32(frame));
		out.put<u64>(h);
		send_all(out);
	}
	else
	{
		hash_entry &e = m_hashes[(frame / m_hash_every) % m_hashes.size()];
		if (e.frame != frame)
			e = hash_entry();
		e.frame = frame;
		e.mine = h;
		e.have_mine = true;
		compare_hash(e);
	}
}

void netplay_session::compare_hash(hash_entry &e)
{
	if (!e.have_mine || !e.have_host)
		return;
	m_hash_compared++;
	const bool same = e.mine == e.host;
	e.have_mine = e.have_host = false;

	if (same)
		return;

	m_desyncs++;
	osd_printf_info("Netplay: desync at frame %d\n", int(e.frame));
	request_resync(m_cur, e.frame);
}

void netplay_session::request_resync(s64 boundary, s64 frame)
{
	const u32 now = now_ms();
	if (m_resync_pending && now - m_resync_ms < RESYNC_RETRY_MS)
		return;
	m_resync_pending = true;
	m_resync_ms = now;
	m_machine.popmessage("Netplay: repairing a desync");
	packet_writer out(P_RESYNC, m_session, m_me);
	out.put<s32>(s32(boundary));
	out.put<s32>(s32(frame));
	send(m_peers[0].endpoint, out);
}

void netplay_session::apply_received_state(s64 boundary)
{
	const s64 frame = m_rx.frame;
	m_rx.ready = false;
	if (frame > boundary || boundary - frame >= INPUT_RING - 1)
		return; // too old to catch up from: the next hash mismatch asks again

	// the host sends the state of the frame that differed, if it still has it
	const checked_state &c = m_checked[(frame / m_hash_every) % 2];
	if (c.frame == frame)
		log_differences(c.data, m_rx.raw);
	else if (slot_valid(frame))
		log_differences(slot_for(frame).data, m_rx.raw);

	slot &s = slot_for(frame);
	s.data = std::move(m_rx.raw);
	s.frame = frame;
	s.valid = true;
	m_floor = frame;
	m_confirmed = frame;
	m_rollback_to = NO_FRAME;
	m_hashes.assign(m_hashes.size(), hash_entry());
	resimulate(frame, boundary);
	m_resync_pending = false;
	m_resyncs++;
	m_machine.popmessage("Netplay: desync repaired");
	osd_printf_info("Netplay: state of frame %d loaded at frame %d\n", int(frame), int(boundary));
}

// what a desync touched: the only way to find out from a session in the field
void netplay_session::log_differences(const std::vector<u8> &mine, const std::vector<u8> &host)
{
	std::string names;
	int count = 0;
	for_each_item([&] (const char *name, size_t offset, size_t size)
	{
		const bool skipped = std::find(m_hash_skip.begin(), m_hash_skip.end(), std::make_pair(offset, size)) != m_hash_skip.end();
		if (!skipped && memcmp(&mine[offset], &host[offset], size) && count++ < 16)
			names.append(names.empty() ? "" : ";").append(name);
	});
	osd_printf_info("Netplay: %d items differ: %s\n", count, names);
}

// host: the state of the frame that differed, for the client to log what; else the newest one
void netplay_session::serve_resyncs()
{
	if (!m_host)
		return;
	for (int p = 1; p < m_players; p++)
	{
		peer &pr = m_peers[p];
		if (!pr.resync_wanted || pr.tx.active)
			continue;
		const s64 newest = std::min(m_confirmed, pr.resync_frame);
		const checked_state &c = m_checked[(pr.resync_check / m_hash_every) % 2];
		if (c.frame == pr.resync_check && c.frame <= newest)
		{
			pr.resync_wanted = false;
			start_transfer(p, c.frame, c.data);
			continue;
		}
		const s64 oldest = std::max<s64>(START_FRAME, newest - s64(slot_count()) + 1);
		for (s64 f = newest; f >= oldest; f--)
		{
			if (slot_valid(f))
			{
				pr.resync_wanted = false;
				start_transfer(p, f, slot_for(f).data);
				break;
			}
		}
	}
}


//-------------------------------------------------
//  time sync: whoever runs ahead slows down a
//  little, so that nobody has to stall
//-------------------------------------------------

void netplay_session::update_time_sync(s64 boundary)
{
	const u32 now = now_ms();
	float ahead = 0;
	for (int p = 0; p < m_players; p++)
	{
		peer &pr = m_peers[p];
		if (p == m_me || !pr.connected)
			continue;
		const float remote = float(pr.frame) + (float(now - pr.frame_rx_ms) + pr.rtt / 2) / m_frame_ms;
		pr.advantage += (float(boundary) - remote - pr.advantage) * 0.05f;
		ahead = std::max(ahead, (pr.advantage - pr.their_advantage) / 2);
	}

	const bool slow = m_slowed ? ahead > 0.25f : ahead > 1.0f;
	if (slow != m_slowed)
	{
		m_slowed = slow;
		m_machine.video().set_speed_factor(slow ? m_base_speed * 98 / 100 : m_base_speed);
	}
	if (m_slowed)
		m_slow_frames++;
}


//-------------------------------------------------
//  meeting: everybody announces itself on the DHT
//  under the room code, and writes to the others:
//  both sides writing opens the routers (UDP hole
//  punching). At home the players broadcast, and
//  write to every address to open the firewalls
//-------------------------------------------------

void netplay_session::meet_tick(u32 now)
{
	if (!m_dht)
		return;
	m_dht->tick(now);
	for (; m_peers_reported < m_dht->peers().size(); m_peers_reported++)
		osd_printf_verbose("Netplay: the DHT has %s:%d\n", m_dht->peers()[m_peers_reported].address().to_string(), int(m_dht->peers()[m_peers_reported].port()));
	if (!m_nat_reported && m_dht->symmetric_nat())
	{
		m_nat_reported = true;
		osd_printf_info("Netplay: your router uses a different port for every destination (symmetric NAT): players outside your network will probably not reach you; use a VPN such as Tailscale\n");
	}
}

//-------------------------------------------------
//  transport
//-------------------------------------------------

void netplay_session::send(const udp::endpoint &to, const packet_writer &packet, const util::sha1_t &key)
{
	std::vector<u8> data = packet.tagged(key);
	if (m_sim_delay || m_sim_jitter || m_sim_loss)
	{
		if (int(m_rng() % 100) < m_sim_loss)
			return;
		const u32 extra = m_sim_jitter ? m_rng() % (m_sim_jitter + 1) : 0;
		m_sim_queue.push_back({ now_ms() + m_sim_delay + extra, to, std::move(data) });
		return;
	}
	asio::error_code ec;
	m_socket.send_to(asio::buffer(data), to, 0, ec);
	m_tx_bytes += data.size();
	if (ec && !m_send_failed++)
		osd_printf_verbose("Netplay: cannot send to %s:%d: %s\n", to.address().to_string(), int(to.port()), ec.message());
}

// all: at the end, as the packets still queued would be lost with the process
void netplay_session::flush_simulated(bool all)
{
	// not in order: reordering is what jitter does anyway
	const u32 now = now_ms();
	for (auto it = m_sim_queue.begin(); it != m_sim_queue.end(); )
	{
		if (all || s32(now - it->time) >= 0)
		{
			asio::error_code ec;
			m_socket.send_to(asio::buffer(it->data), it->endpoint, 0, ec);
			m_tx_bytes += it->data.size();
			it = m_sim_queue.erase(it);
		}
		else
			++it;
	}
}

void netplay_session::send_all(const packet_writer &packet)
{
	for (int p = 0; p < m_players; p++)
		if (p != m_me && m_peers[p].connected)
			send(m_peers[p].endpoint, packet);
}

void netplay_session::send_bye()
{
	if (m_phase != phase::RUNNING && m_phase != phase::LOBBY)
		return;
	for (int i = 0; i < 3; i++)
		send_all(packet_writer(P_BYE, m_session, m_me));
	flush_simulated(true);
}

void netplay_session::poll()
{
	flush_simulated();
	u8 buffer[2048];
	for (;;)
	{
		udp::endpoint from;
		asio::error_code ec;
		const size_t size = m_socket.receive_from(asio::buffer(buffer), from, 0, ec);
		if (ec == asio::error::would_block || ec == asio::error::try_again)
			return;
		if (ec)
			continue; // e.g. ICMP port unreachable from a peer that left
		m_rx_bytes += size;
		if (!m_dht || !m_dht->handle(from, buffer, size))
			handle_packet(from, buffer, size);
		if (m_phase == phase::ENDED)
			return;
	}
}

void netplay_session::handle_packet(const udp::endpoint &from, const u8 *data, size_t size)
{
	if (size < 10 + TAG_SIZE)
		return;
	size -= TAG_SIZE;
	const u64 tag = get_u64le(data + size);
	packet_reader in(data, size);
	const u32 magic = in.get<u32>();
	const u32 session = in.get<u32>();
	const u8 type = in.get<u8>();
	const int sender = in.get<u8>();
	if (!in.ok() || magic != MAGIC)
		return;
	if (tag != packet_tag(m_key, data, size))
	{
		// a player who typed the host's address instead of the code: only from home or a VPN
		static const util::sha1_t open = room_key(std::string());
		if (m_host && type == P_HELLO && private_address(from.address()) && tag == packet_tag(open, data, size))
			handle_hello(from, in, open);
		return; // another room, or not a player
	}

	if (type == P_HELLO)
	{
		if (m_host)
			handle_hello(from, in, m_key);
		return;
	}
	if (type == P_PUNCH)
	{
		// there to open the sender's router; from the host, also to say that it's there
		if (!m_host && m_welcomed && from == m_host_endpoint)
			m_peers[0].last_rx_ms = now_ms();
		return;
	}

	if (type == P_WELCOME || type == P_REJECT)
	{
		// until the welcome, the room's host may answer from any of its addresses
		if (m_host || m_started || (from != m_host_endpoint && (m_room.empty() || m_welcomed)))
			return;
		if (type == P_REJECT)
		{
			const std::string reason = in.get_string();
			if (in.ok())
				end_session(util::string_format("refused by the host: %s", reason).c_str());
		}
		else
		{
			m_session = session;
			handle_welcome(from, in);
		}
		return;
	}

	if (session != m_session || !m_session || sender >= m_players || sender == m_me || !slot_count())
		return;
	if (type == P_MEET)
	{
		// another client, while waiting: its address as seen from here, the one at home if it works
		const u64 nonce = in.get<u64>();
		if (m_host || !in.ok() || nonce == m_nonce)
			return;
		auto it = m_met.find(nonce);
		if (it == m_met.end())
			m_met.emplace(nonce, from);
		else if (!home_address(it->second.address()) && home_address(from.address()))
			it->second = from;
		return;
	}
	if (type == P_START)
	{
		if (!m_host && from == m_host_endpoint)
			handle_start(in);
		return;
	}
	peer &pr = m_peers[sender];
	if (!pr.connected || pr.endpoint != from)
		return;
	pr.last_rx_ms = now_ms();

	switch (type)
	{
	case P_GO:
		if (!m_host && sender == 0)
			m_go = true;
		break;
	case P_INPUT:
		handle_input(sender, in);
		break;
	case P_HASH:
		if (!m_host && sender == 0)
			handle_hash(in);
		break;
	case P_RESYNC:
		if (m_host)
		{
			const s64 boundary = in.get<s32>();
			const s64 frame = in.get<s32>();
			if (in.ok() && boundary <= m_cur + INPUT_RING / 2)
			{
				pr.resync_wanted = true;
				pr.resync_frame = boundary;
				pr.resync_check = frame;
				m_desyncs++;
			}
		}
		break;
	case P_CHUNK:
		if (!m_host && sender == 0)
			handle_chunk(in);
		break;
	case P_CHUNK_ACK:
		if (m_host)
			handle_chunk_ack(sender, in);
		break;
	case P_LEFT:
		if (!m_host && sender == 0)
			handle_left(in);
		break;
	case P_BYE:
		// the host ends the game; a client is dropped by the host, which tells the others
		if (sender == 0)
			end_session("the host left");
		else if (m_host && m_phase == phase::LOBBY && !m_all_joined)
			remove_player(sender, nullptr);
		else if (m_host)
			drop_player(sender);
		break;
	default:
		break;
	}
}

void netplay_session::handle_hello(const udp::endpoint &from, packet_reader &in, const util::sha1_t &key)
{
	const u32 protocol = in.get<u32>();
	const std::string build = in.get_string();
	const std::string system = in.get_string();
	const u32 state_size = in.get<u32>();
	const u64 layout = in.get<u64>();
	const u64 rom = in.get<u64>();
	const u64 inputs = in.get<u64>();
	const u64 nonce = in.get<u64>();
	const u32 lan_address = in.get<u32>();
	const u16 lan_port = in.get<u16>();
	const u32 wan_address = in.get<u32>();
	const u16 wan_port = in.get<u16>();
	std::vector<u64> met(std::min<int>(in.get<u8>(), MAX_PLAYERS));
	for (u64 &n : met)
		n = in.get<u64>();
	if (!in.ok() && protocol == PROTOCOL)
		return;

	int player = -1;
	for (int p = 1; p < m_players && player < 0; p++)
		if (m_peers[p].connected && m_peers[p].endpoint == from)
			player = p;
	if (m_phase != phase::LOBBY)
	{
		if (player < 0)
		{
			packet_writer out(P_REJECT, 0, 0);
			out.put_string("the game has already started");
			send(from, out, key);
		}
		return;
	}
	// a player already in, by another way: at home and through the router
	for (int p = 1; p < m_players; p++)
		if (m_peers[p].connected && m_peers[p].nonce == nonce && m_peers[p].endpoint != from)
			return;

	std::string reason;
	if (protocol != PROTOCOL || build != emulator_info::get_build_version())
		reason = util::string_format("different MAME build: the host has %s, you have %s", emulator_info::get_build_version(), build);
	else if (system != m_machine.system().name)
		reason = util::string_format("different game: the host is playing %s", m_machine.system().type.fullname());
	else if (rom != m_rom_hash)
		reason = "different ROM contents";
	else if (state_size != m_state_size || layout != m_layout_hash)
		reason = "different save state layout";
	else if (inputs != m_input_hash)
		reason = "different inputs";

	if (reason.empty() && player < 0)
	{
		for (int p = 1; p < m_players && player < 0; p++)
			if (!m_peers[p].connected)
				player = p;
		if (player < 0)
			reason = m_all_joined ? "the game has already started" : "the room is full";
		else
		{
			m_peers[player] = peer();
			m_peers[player].connected = true;
			m_peers[player].endpoint = from;
			m_peers[player].nonce = nonce;
			m_peers[player].joined_ms = now_ms();
			osd_printf_info("Netplay: player %d joined from %s\n", player + 1, from.address().to_string());
		}
	}
	if (!reason.empty())
	{
		packet_writer out(P_REJECT, 0, 0);
		out.put_string(reason);
		send(from, out, key);
		osd_printf_info("Netplay: refused %s: %s\n", from.address().to_string(), reason);
		return;
	}

	peer &pr = m_peers[player];
	pr.last_rx_ms = now_ms();
	pr.lan = udp::endpoint(asio::ip::address_v4(lan_address), lan_port);
	pr.wan = udp::endpoint(asio::ip::address_v4(wan_address), wan_port);
	pr.met = std::move(met);
	send_welcome(player, key);
	if (m_all_joined)
		send_start(player); // the first one was lost
}

// a firewall lets in only answers from the addresses written to, which a broadcast isn't
void netplay_session::sweep_lan(const packet_writer &packet)
{
	if (m_lan.is_unspecified())
		return;
	const u32 me = m_lan.to_uint();
	for (u32 i = 0; i < SWEEP_BATCH; i++, m_sweep_next = m_sweep_next % 254 + 1)
		if ((me & ~0xffu) + m_sweep_next != me)
			send(udp::endpoint(asio::ip::address_v4((me & ~0xffu) + m_sweep_next), LAN_PORT), packet);
}

// the players who have joined play; whoever comes later is refused
void netplay_session::begin(int players)
{
	m_players = players;
	m_all_joined = true;
	osd_printf_info("Netplay: starting with %d players\n", players);
	for (int p = 1; p < m_players; p++)
	{
		send_start(p);
		start_transfer(p, START_FRAME, slot_for(START_FRAME).data);
	}
}

// whether the clients that joined have all heard from each other; if not, the last one of a pair that hasn't
bool netplay_session::meshed(int joined, int &stuck) const
{
	for (int p = 1; p < joined; p++)
		for (int q = 1; q < joined; q++)
			if (p != q && std::find(m_peers[p].met.begin(), m_peers[p].met.end(), m_peers[q].nonce) == m_peers[p].met.end())
			{
				stuck = std::max(p, q);
				return false;
			}
	return true;
}

// in the lobby: the ones after it move up a place, and get it with the next welcome
void netplay_session::remove_player(int player, const char *reason)
{
	if (reason)
	{
		packet_writer out(P_REJECT, 0, 0);
		out.put_string(reason);
		send(m_peers[player].endpoint, out);
	}
	osd_printf_info("Netplay: player %d left the room%s%s\n", player + 1, reason ? ": " : "", reason ? reason : "");
	for (int p = player; p + 1 < m_players; p++)
		m_peers[p] = m_peers[p + 1];
	m_peers[m_players - 1] = peer();
}

// host, once started: the frame after the last inputs it sent is where its neutral inputs start
void netplay_session::drop_player(int player)
{
	peer &pr = m_peers[player];
	if (pr.left != NO_FRAME)
		return;
	pr.tx.active = false;
	pr.resync_wanted = false;
	pr.left_until = m_cur + LEFT_REPEAT;
	player_left(player, m_known[player], m_known[player] + 1);
}

// from frame `from` on, what was predicted for the player may have been wrong
void netplay_session::player_left(int player, s64 last, s64 from)
{
	peer &pr = m_peers[player];
	pr.connected = false;
	pr.left = last;
	m_known[player] = NO_FRAME;
	for (s64 f = std::max(from, m_floor + 1); f <= m_cur; f++)
		if (m_used[player][f % INPUT_RING] != best(player, f))
		{
			m_rollback_to = std::min(m_rollback_to, f);
			break;
		}
	m_machine.popmessage("Netplay: player %d left", player + 1);
	osd_printf_info("Netplay: player %d left after frame %d\n", player + 1, int(last));
}

// host: who left, with its last inputs, for whoever didn't get them
void netplay_session::send_left()
{
	for (int p = 1; p < m_players; p++)
	{
		const peer &pr = m_peers[p];
		if (pr.left == NO_FRAME || m_cur > pr.left_until)
			continue;
		const s64 first = std::max<s64>(START_FRAME, pr.left - MAX_INPUTS_PER_PACKET + 1);
		packet_writer out(P_LEFT, m_session, m_me);
		out.put<u8>(u8(p));
		out.put<s32>(s32(pr.left));
		out.put<s32>(s32(first));
		for (s64 f = first; f <= pr.left; f++)
			out.put<u32>(m_input[p][f % INPUT_RING]);
		send_all(out);
	}
}

void netplay_session::handle_left(packet_reader &in)
{
	const int player = in.get<u8>();
	const s64 last = in.get<s32>();
	const s64 first = in.get<s32>();
	if (!in.ok() || player < 1 || player >= m_players || first > last || last - first >= MAX_INPUTS_PER_PACKET || in.remaining_size() < size_t(last - first + 1) * 4)
		return;
	if (player == m_me)
	{
		end_session("the host stopped receiving your packets");
		return;
	}
	if (m_peers[player].left != NO_FRAME)
		return;
	const s64 from = std::min(m_known[player], last) + 1;
	for (s64 f = first; f <= last; f++)
	{
		const u32 word = in.get<u32>();
		if (f > m_known[player])
			m_input[player][f % INPUT_RING] = word;
	}
	player_left(player, last, from);
}

void netplay_session::send_welcome(int player, const util::sha1_t &key)
{
	packet_writer out(P_WELCOME, m_session, 0);
	out.put<u8>(u8(player));
	out.put<u8>(u8(m_players));
	out.put<u8>(u8(m_delay));
	out.put<u8>(u8(m_window));
	out.put<u16>(u16(m_hash_every));
	out.put<u64>(m_base_time);
	out.put_string(m_room); // for whoever typed the address: the key of the next packets
	for (ioport_port *port : m_ports)
		for (ioport_field &field : port->fields())
			if (user_setting(field))
				out.put<u32>(field.live().value);
	int others = 0;
	for (int p = 1; p < m_players; p++)
		others += p != player && m_peers[p].connected;
	out.put<u8>(u8(others));
	for (int p = 1; p < m_players; p++)
	{
		if (p == player || !m_peers[p].connected)
			continue;
		out.put<u64>(m_peers[p].nonce);
		out.put<u32>(m_peers[p].endpoint.address().to_v4().to_uint());
		out.put<u16>(m_peers[p].endpoint.port());
		for (const udp::endpoint &endpoint : { m_peers[p].lan, m_peers[p].wan })
		{
			out.put<u32>(endpoint.address().to_v4().to_uint());
			out.put<u16>(endpoint.port());
		}
	}
	send(m_peers[player].endpoint, out, key);
}

// again at every hello until the start: the place can change when somebody leaves
void netplay_session::handle_welcome(const udp::endpoint &from, packet_reader &in)
{
	const int player = in.get<u8>();
	const int players = in.get<u8>();
	const int delay = in.get<u8>();
	const int window = in.get<u8>();
	const int hash_every = in.get<u16>();
	const u64 base_time = in.get<u64>();
	const std::string room = in.get_string();
	std::vector<u32> settings;
	for (ioport_port *port : m_ports)
		for (ioport_field &field : port->fields())
			if (user_setting(field))
				settings.push_back(in.get<u32>());
	std::vector<other> others(in.get<u8>());
	for (other &o : others)
	{
		o.nonce = in.get<u64>();
		for (udp::endpoint *endpoint : { &o.endpoint, &o.lan, &o.wan })
		{
			const u32 address = in.get<u32>();
			const u16 port = in.get<u16>();
			*endpoint = udp::endpoint(asio::ip::address_v4(address), port);
		}
	}
	if (!in.ok() || players < 2 || players > MAX_PLAYERS || player < 1 || player >= players || delay > 8 || window < 1 || window > 60 || hash_every < 1 || others.size() >= MAX_PLAYERS)
		return;
	m_me = player;
	m_others = std::move(others);
	m_peers[0].last_rx_ms = now_ms();
	if (m_welcomed)
		return;

	if (m_room.empty())
	{
		m_room = clean_room(room);
		m_key = room_key(m_room);
	}
	m_host_endpoint = from;
	m_players = players;
	m_delay = delay;
	m_window = window;
	m_hash_every = hash_every;
	m_machine.set_base_time(time_t(base_time));

	// the host's DIP switches and configuration, which then can't change (ioport_field::set_user_settings)
	auto value = settings.begin();
	for (ioport_port *port : m_ports)
		for (ioport_field &field : port->fields())
			if (user_setting(field))
				field.live().value = *value++ & field.mask();
	for (bool flush : { true, false })
		for (ioport_port *port : m_ports)
			port->update_defvalue(flush);
	m_peers[0].connected = true;
	m_peers[0].endpoint = m_host_endpoint;
	start_session_buffers();
	m_welcomed = true;
}

// every client gets the others, which it already reached: it knows from where
void netplay_session::send_start(int player)
{
	packet_writer out(P_START, m_session, 0);
	out.put<u8>(u8(m_players));
	for (int p = 1; p < m_players; p++)
	{
		out.put<u64>(m_peers[p].nonce);
		out.put<u32>(m_peers[p].endpoint.address().to_v4().to_uint());
		out.put<u16>(m_peers[p].endpoint.port());
	}
	send(m_peers[player].endpoint, out);
}

void netplay_session::handle_start(packet_reader &in)
{
	const int players = in.get<u8>();
	if (players < 2 || players > MAX_PLAYERS || m_me >= players)
		return;
	udp::endpoint endpoints[MAX_PLAYERS];
	for (int p = 1; p < players; p++)
	{
		const u64 nonce = in.get<u64>();
		const u32 address = in.get<u32>();
		const u16 port = in.get<u16>();
		auto met = m_met.find(nonce);
		endpoints[p] = met != m_met.end() ? met->second : udp::endpoint(asio::ip::address_v4(address), port);
	}
	if (!in.ok())
		return;
	m_players = players;
	for (int p = 1; p < m_players; p++)
	{
		if (p != m_me)
		{
			m_peers[p].connected = true;
			m_peers[p].endpoint = endpoints[p];
			m_peers[p].last_rx_ms = now_ms();
		}
	}
	m_started = true;
}

void netplay_session::send_inputs()
{
	if (m_host)
		send_left();
	const s64 newest = m_known[m_me];
	const u32 now = now_ms();
	for (int p = 0; p < m_players; p++)
	{
		peer &pr = m_peers[p];
		if (p == m_me || !pr.connected)
			continue;
		const s64 first = pr.acked + 1;
		const int count = int(std::clamp<s64>(newest - first + 1, 0, MAX_INPUTS_PER_PACKET));
		packet_writer out(P_INPUT, m_session, m_me);
		out.put<s32>(s32(m_cur));
		out.put<s32>(s32(m_known[p]));
		out.put<u32>(now);
		out.put<u32>(pr.echo_time);
		out.put<u16>(u16(std::min<u32>(now - pr.echo_rx_ms, 65535)));
		out.put<s16>(s16(std::clamp(pr.advantage * 16, -32000.0f, 32000.0f)));
		out.put<s32>(s32(first));
		out.put<u8>(u8(count));
		for (int i = 0; i < count; i++)
			out.put<u32>(m_input[m_me][(first + i) % INPUT_RING]);
		send(pr.endpoint, out);
	}
}

void netplay_session::handle_input(int sender, packet_reader &in)
{
	peer &pr = m_peers[sender];
	const s64 frame = in.get<s32>();
	const s64 acked = in.get<s32>();
	const u32 time = in.get<u32>();
	const u32 echo = in.get<u32>();
	const u32 hold = in.get<u16>();
	const float advantage = float(in.get<s16>()) / 16;
	const s64 first = in.get<s32>();
	const int count = in.get<u8>();
	if (!in.ok() || in.remaining_size() < size_t(count) * 4)
		return;

	const u32 now = now_ms();
	if (frame >= pr.frame)
	{
		pr.frame = frame;
		pr.frame_rx_ms = now;
		pr.their_advantage = advantage;
		pr.echo_time = time;
		pr.echo_rx_ms = now;
		if (echo)
		{
			const float sample = float(now - echo - hold);
			if (sample >= 0 && sample < 10000)
				pr.rtt = pr.rtt ? pr.rtt + (sample - pr.rtt) * 0.1f : sample;
		}
	}
	if (acked <= m_known[m_me])
		pr.acked = std::max(pr.acked, acked);

	for (int i = 0; i < count; i++)
	{
		const s64 f = first + i;
		const u32 word = in.get<u32>();
		if (f <= m_known[sender])
			continue;
		if (f != m_known[sender] + 1 || f > m_cur + INPUT_RING / 2)
			break;
		m_input[sender][f % INPUT_RING] = word;
		m_known[sender] = f;
		if (f <= m_cur && f > m_floor && m_used[sender][f % INPUT_RING] != word)
			m_rollback_to = std::min(m_rollback_to, f);
	}
	if (sender == 0)
		m_go = true; // the host is running, in case its "go" was lost
}

void netplay_session::handle_hash(packet_reader &in)
{
	const s64 frame = in.get<s32>();
	const u64 hash = in.get<u64>();
	if (!in.ok() || frame <= m_floor || frame % m_hash_every != 0)
		return;
	hash_entry &e = m_hashes[(frame / m_hash_every) % m_hashes.size()];
	if (e.frame != frame)
		e = hash_entry();
	e.frame = frame;
	e.host = hash;
	e.have_host = true;
	compare_hash(e);
}

void netplay_session::start_transfer(int player, s64 frame, const std::vector<u8> &state)
{
	auto data = std::make_shared<std::vector<u8>>(ZSTD_compressBound(state.size()));
	const size_t size = ZSTD_compress(data->data(), data->size(), state.data(), state.size(), 1);
	if (ZSTD_isError(size))
	{
		end_session("cannot compress the state");
		return;
	}
	data->resize(size);

	transfer_out &tx = m_peers[player].tx;
	tx.active = true;
	tx.data = std::move(data);
	tx.id = m_next_transfer_id++;
	tx.frame = frame;
	tx.total = u32((size + CHUNK_SIZE - 1) / CHUNK_SIZE);
	tx.next_missing = 0;
	tx.received_after.fill(0);
	tx.send_pos = 0;
	tx.last_tick_ms = tx.last_pass_ms = now_ms();
	osd_printf_verbose("Netplay: sending the state of frame %d to player %d, %u bytes\n", int(frame), player + 1, unsigned(size));
}

void netplay_session::tick_transfers()
{
	if (!m_host)
		return;
	for (int p = 1; p < m_players; p++)
	{
		peer &pr = m_peers[p];
		transfer_out &tx = pr.tx;
		if (!tx.active)
			continue;
		const u32 now = now_ms();
		u32 budget = std::min((now - tx.last_tick_ms) * CHUNKS_PER_MS, CHUNK_WINDOW);
		if (budget)
			tx.last_tick_ms = now;
		for (u32 tried = 0; budget && tried < 2 * CHUNK_WINDOW; tried++)
		{
			const u32 limit = std::min(tx.total, tx.next_missing + CHUNK_WINDOW);
			if (tx.send_pos >= limit)
			{
				if (now - tx.last_pass_ms < CHUNK_RESEND_MS)
					break;
				tx.last_pass_ms = now;
				tx.send_pos = tx.next_missing;
			}
			const u32 index = tx.send_pos++;
			const u32 after = index - tx.next_missing - 1;
			if (index > tx.next_missing && after < CHUNK_WINDOW && BIT(tx.received_after[after / 64], after % 64))
				continue;

			const size_t offset = size_t(index) * CHUNK_SIZE;
			const size_t size = std::min<size_t>(CHUNK_SIZE, tx.data->size() - offset);
			packet_writer out(P_CHUNK, m_session, m_me);
			out.put<u32>(tx.id);
			out.put<s32>(s32(tx.frame));
			out.put<u32>(u32(tx.data->size()));
			out.put<u32>(index);
			out.put_bytes(tx.data->data() + offset, size);
			send(pr.endpoint, out);
			budget--;
		}
	}
}

void netplay_session::handle_chunk(packet_reader &in)
{
	const u32 id = in.get<u32>();
	const s64 frame = in.get<s32>();
	const u32 compressed = in.get<u32>();
	const u32 index = in.get<u32>();
	if (!in.ok())
		return;
	m_send_ack = true;
	if (id == m_rx.done_id)
		return;

	if (!m_rx.active || id != m_rx.id)
	{
		if (compressed == 0 || compressed > ZSTD_compressBound(m_state_size))
			return;
		m_rx.active = true;
		m_rx.id = id;
		m_rx.frame = frame;
		m_rx.compressed = compressed;
		m_rx.total = (compressed + CHUNK_SIZE - 1) / CHUNK_SIZE;
		m_rx.next_missing = 0;
		m_rx.have.assign(m_rx.total, 0);
		m_rx.data.resize(compressed);
	}
	if (frame != m_rx.frame || compressed != m_rx.compressed || index >= m_rx.total)
		return;

	const size_t offset = size_t(index) * CHUNK_SIZE;
	const size_t size = std::min<size_t>(CHUNK_SIZE, compressed - offset);
	if (in.remaining_size() != size)
		return;
	memcpy(&m_rx.data[offset], in.remaining(), size);
	m_rx.have[index] = 1;
	while (m_rx.next_missing < m_rx.total && m_rx.have[m_rx.next_missing])
		m_rx.next_missing++;

	if (m_rx.next_missing == m_rx.total)
	{
		m_rx.active = false;
		m_rx.done_id = id;
		m_rx.raw.resize(m_state_size);
		const size_t raw = ZSTD_decompress(m_rx.raw.data(), m_rx.raw.size(), m_rx.data.data(), m_rx.data.size());
		if (ZSTD_isError(raw) || raw != m_state_size)
		{
			end_session("the host's state is corrupt");
			return;
		}
		m_rx.ready = true;
	}
}

void netplay_session::send_chunk_ack()
{
	if (!m_send_ack)
		return;
	m_send_ack = false;
	const bool done = m_rx.done_id && !m_rx.active;
	if (done && !m_started)
		return; // "done" lets the session start: first the addresses of the other clients are needed
	u64 after[CHUNK_WINDOW / 64] = { };
	if (!done)
		for (u32 i = 0; i < CHUNK_WINDOW && m_rx.next_missing + 1 + i < m_rx.total; i++)
			if (m_rx.have[m_rx.next_missing + 1 + i])
				after[i / 64] |= u64(1) << (i % 64);
	packet_writer out(P_CHUNK_ACK, m_session, m_me);
	out.put<u32>(done ? m_rx.done_id : m_rx.id);
	out.put<u8>(done ? 1 : 0);
	out.put<u32>(m_rx.next_missing);
	for (u64 bits : after)
		out.put<u64>(bits);
	send(m_peers[0].endpoint, out);
}

void netplay_session::handle_chunk_ack(int sender, packet_reader &in)
{
	peer &pr = m_peers[sender];
	const u32 id = in.get<u32>();
	const bool done = in.get<u8>() != 0;
	const u32 next_missing = in.get<u32>();
	std::array<u64, CHUNK_WINDOW / 64> after;
	for (u64 &bits : after)
		bits = in.get<u64>();
	if (!in.ok())
		return;

	// the starting state is the first transfer: finished means ready to go
	if (done && id == pr.tx.id)
	{
		pr.tx.active = false;
		pr.session_ready = true;
	}
	else if (done && pr.session_ready && m_phase == phase::RUNNING)
	{
		packet_writer out(P_GO, m_session, m_me);
		send(pr.endpoint, out);
	}
	else if (done && pr.session_ready)
		send(pr.endpoint, packet_writer(P_PUNCH, 0, 0)); // the others are still getting it
	else if (pr.tx.active && id == pr.tx.id && next_missing <= pr.tx.total && next_missing >= pr.tx.next_missing)
	{
		pr.tx.next_missing = next_missing;
		pr.tx.received_after = after;
	}
}

// a client gives up on another client later than the host would drop it
void netplay_session::check_timeouts()
{
	const u32 now = now_ms();
	for (int p = 0; p < m_players; p++)
	{
		if (p == m_me || !m_peers[p].connected)
			continue;
		const u32 silent = now - m_peers[p].last_rx_ms;
		if (m_host && silent >= PEER_TIMEOUT_MS)
			drop_player(p);
		else if (p == 0 && silent >= PEER_TIMEOUT_MS)
			end_session("no packets from the host");
		else if (silent >= 2 * PEER_TIMEOUT_MS)
			end_session(util::string_format("no packets from player %d", p + 1).c_str());
	}
}

void netplay_session::end_session(const char *reason)
{
	if (m_phase == phase::ENDED)
		return;
	osd_printf_info("Netplay: session over: %s\n", reason);
	s_end_message = util::string_format("Netplay: %s", reason);
	send_bye();
	m_end_reason = reason;
	m_phase = phase::ENDED;
	write_log();
	m_machine.schedule_exit();
}


void netplay_session::write_log()
{
	if (m_log_path.empty())
		return;
	FILE *f = std::fopen((m_log_path + ".json").c_str(), "w");
	if (!f)
		return;
	const double seconds = m_session_start ? double(osd_ticks() - m_session_start) / osd_ticks_per_second() : 0;
	float rtt = 0;
	for (int p = 0; p < m_players; p++)
		if (p != m_me)
			rtt = std::max(rtt, m_peers[p].rtt);
	std::fprintf(f, "{\"system\":\"%s\",\"player\":%d,\"players\":%d,\"frames\":%lld,\"seconds\":%.2f,\"end\":\"%s\",\"rtt_ms\":%.1f,"
			"\"rollbacks\":%llu,\"resim_frames\":%llu,\"rollback_ms_max\":%.2f,\"rollback_ms_mean\":%.2f,\"unrecoverable\":%llu,\"saves_failed\":%llu,"
			"\"hash_compared\":%llu,\"desyncs\":%llu,\"resyncs\":%llu,\"stall_ms\":%llu,\"slow_frames\":%llu,\"tx_bytes\":%llu,\"rx_bytes\":%llu,\"depth\":[",
			m_machine.system().name, m_me + 1, m_players, (long long)m_cur, seconds, m_end_reason.c_str(), rtt,
			(unsigned long long)m_rollbacks, (unsigned long long)m_resim_frames, m_rollback_ms_max, m_rollbacks ? m_rollback_ms_total / m_rollbacks : 0.0,
			(unsigned long long)m_unrecoverable, (unsigned long long)m_saves_failed,
			(unsigned long long)m_hash_compared, (unsigned long long)m_desyncs, (unsigned long long)m_resyncs,
			(unsigned long long)m_stall_ms, (unsigned long long)m_slow_frames, (unsigned long long)m_tx_bytes, (unsigned long long)m_rx_bytes);
	for (size_t i = 0; i < std::size(m_depth_histogram); i++)
		std::fprintf(f, "%s%llu", i ? "," : "", (unsigned long long)m_depth_histogram[i]);
	std::fprintf(f, "]}\n");
	std::fclose(f);
}

} // anonymous namespace


std::unique_ptr<rollback_session> create_netplay_session(running_machine &machine, bool host)
{
	return std::make_unique<netplay_session>(machine, host);
}
