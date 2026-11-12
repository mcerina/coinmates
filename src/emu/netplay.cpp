// license:BSD-3-Clause
/***************************************************************************

    netplay.cpp

    Rollback netplay between 2 to 4 instances over UDP.

    -netplay_host makes a room, -netplay_join <room code or host:port>
    joins it: players meet through public trackers (netplay_meet.h) or by broadcast
    at home. Each sends its inputs to all the others; missing inputs are
    predicted and corrected by rolling back. The host's state starts the
    session, and repairs a client whose state hash differs. Everything runs
    on the main thread, polling the socket at every frame boundary.

    Packets are signed with a key from the room code; a typed address
    instead of the code is let in only from a private network or a VPN.
    Whoever leaves during the game gets neutral inputs, so the others play
    on; a late joiner takes a free place from the host's state; a lost
    connection joins again by itself.

    Test hooks come from the MAME_NETPLAY_TEST environment variable (key=value
    separated by commas):
      bot=<seed>        a deterministic bot plays instead of the local player
      frames=<n>        leave at frame n
      log=<path>        write <path>.hash.csv, <path>.json and <path>.trip.csv: each input packet, when,
                        from whom, if it brought new inputs, sent how long before by their clock and how
                        many frames before, the round trip it gave
      sim=<ms>:<ms>:<%>[:<ms>:<ms>] delay, jitter and loss of every packet sent; and every that many ms,
                        hold them that long and send them in order, as a phone's radio does
      cut=<frame>:<ms>  lose every packet for that long from that frame on, once
      fault=<frame>     corrupt one byte of RAM at this frame
      dump=<from>:<to>  write <path>.<frame>.state for these confirmed frames
      draw=1            draw the redone frames too: if a desync goes away, the game's drawing changes its state
      audio=<ms>        the sound's delay, instead of AUDIO_DELAY_MS
      port=<n>          a joiner's port instead of LAN_PORT: a new one for each test, unknown to routers

***************************************************************************/

#include "emu.h"
#include "rollback.h"
#include "netplay_meet.h"

#include "emuopts.h"
#include "fileio.h"
#include "main.h"
#include "osdepend.h"
#include "osdfile.h"
#include "screen.h"
#include "sound.h"
#include "video.h"

#include "asio.h"
#include "hashing.h"
#include "multibyte.h"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <shellapi.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif


namespace {

using udp = asio::ip::udp;

constexpr u32 MAGIC = 0x4d4e5031;
constexpr int INPUT_RING = 1024;        // frames of input kept: must cover a state transfer
constexpr int MAX_INPUTS_PER_PACKET = 64;   // unacknowledged inputs: a far round trip (FAR_RTT_MS) is 39 frames, plus a lost packet each way
constexpr u32 CHUNK_SIZE = 1200;        // under every path's MTU, as QUIC
constexpr u32 CHUNK_WINDOW = 256;       // chunks in flight: an acknowledgement has a bit for each
// choices, not measured against a phone's upload: ~2.4 MB/s before the game, ~600 KB/s in it, as the
// inputs need the host's upload too
constexpr u32 CHUNKS_PER_MS = 2;
constexpr u32 MS_PER_CHUNK_PLAYING = 2;
constexpr s64 BACK_FRAMES = 180;        // a choice: 3 s for a late joiner to catch up after its state comes; the others play on
constexpr size_t TAG_SIZE = 8;
constexpr u32 AUDIO_DELAY_MS = 60;      // 3 frames of the 20 ms sound updates: rollbacks within it aren't heard; ~125 ms isn't noticed
constexpr u32 FAR_RTT_MS = netplay_meet::FAR_RTT_MS;
constexpr int WINDOW = 10;              // a choice: the most frames predicted, and redone; a PC redoes 9 within a frame in all games but one
constexpr int MIN_DELAY = 1;            // a frame's own inputs are all read only after its ports are set
// a choice: samples of each estimate of the load (frames: 1 s) and of how late packets come (packets:
// 1 s); the load's sixth slowest, so that a slow frame now and then (a state compressed for a joiner)
// doesn't count; the packets' second slowest (about the 97th percentile over 1-2 s), so that one alone doesn't
constexpr size_t LOAD_FRAMES = 60;
// hello to welcome took 0.25 s at most in Europe: a lost hello goes again about when its answer would come
constexpr u32 HELLO_MS = 250;
constexpr u32 PUNCH_MS = HELLO_MS;      // the same for a lost punch
// a joiner's announce was taken half an answer before it heard back; the next poll of those in the room
// may need a new connection id first: two answers. Their punch then opens their router, sent or lost
constexpr u32 HOST_FOUND_MS = netplay_meet::POLL_MS + 2 * netplay_meet::ANSWER_MS - netplay_meet::ANSWER_MS / 2;
// no welcome by then after the first hello to a router outside home, even with that hello lost: ours
// had shut the socket to the host, and another socket writes first
constexpr u32 WELCOME_MS = FAR_RTT_MS + HELLO_MS;
// the longest silence measured was 0.62 s, plus a lost packet: a player silent this long is having
// trouble
constexpr u32 SILENT_MS = 700;
// the others wait this long for one who went silent: an outage of seconds (wifi roaming, a cell
// change) didn't show in the measures, so this is a choice; one who quits says so (send_bye)
constexpr u32 PEER_TIMEOUT_MS = 10000;
// from the host's welcome of a newcomer: the others find it on the trackers (HOST_FOUND_MS), then
// meet, hear that they're heard, and tell the host: four trips, each maybe waiting a hello; one lost
constexpr u32 MESH_TIMEOUT_MS = HOST_FOUND_MS + 4 * (HELLO_MS + FAR_RTT_MS / 2) + HELLO_MS;
// the worst join with the times above (Australia, another port, a lost packet) is 16.1 s; and 8 s for
// a slow DNS, as seen in a container (a choice: not measured on a real network). Waiting for the start
// has no limit
constexpr u32 JOIN_TIMEOUT_MS = 25000;
constexpr u32 RESYNC_RETRY_MS = 5000;   // a choice: asks again if the repairing state hasn't come
constexpr u16 LAN_PORT = 7000;
// the home network, for computers with a firewall that lets in only answers: every address in 8 s;
// packets to absent ones wait 3 s for ARP in the socket's buffer: 100 KB of its 213 KB, no send failed
constexpr u32 SWEEP_MS = 250;
constexpr u32 SWEEP_BATCH = 8;
constexpr char ROOM_LETTERS[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";  // no 0, 1, I, O: they look alike
constexpr size_t ROOM_LENGTH = 6;
constexpr s64 START_FRAME = 1;
constexpr s64 NO_FRAME = std::numeric_limits<s64>::max();
constexpr size_t MAX_NOTES = 500;      // log lines per player
constexpr size_t MAX_NOTE_SIZE = 600;
constexpr u32 NOTES_WRITE_MS = 5000;
constexpr size_t LOGS_KEPT = 10;        // a reconnection starts a new one

// named <YYYYmmdd-HHMMSS>-<system>-<room>.txt; the oldest first
constexpr size_t LOG_DATE_SIZE = 15;
std::vector<std::string> room_logs(const std::string &dir)
{
	std::vector<std::string> logs;
	if (osd::directory::ptr d = dir.empty() ? nullptr : osd::directory::open(dir))
		while (const osd::directory::entry *e = d->read())
		{
			const std::string_view name(e->name);
			if (e->type == osd::directory::entry::entry_type::FILE && name.size() > LOG_DATE_SIZE + 1 && name[8] == '-' && name[LOG_DATE_SIZE] == '-' && name.ends_with(".txt")
					&& std::all_of(name.begin(), name.begin() + 8, [] (char c) { return std::isdigit(u8(c)); }))
				logs.emplace_back(name);
		}
	std::sort(logs.begin(), logs.end());
	return logs;
}

enum packet_type : u8 { P_HELLO = 1, P_WELCOME, P_REJECT, P_START, P_INPUT, P_HASH, P_RESYNC, P_CHUNK, P_CHUNK_ACK, P_BYE, P_PUNCH, P_MEET, P_ROSTER, P_NOTE };

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

// HMAC-SHA1, with a key of up to 64 bytes
util::sha1_t hmac_sha1(const u8 *key, size_t key_size, const u8 *data, size_t size)
{
	u8 pad[64] = { };
	std::copy_n(key, std::min(key_size, sizeof(pad)), pad);
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
	return outer.finish();
}

// PBKDF2, slow on purpose (~0.1 s): the trackers show a hash of the secret to strangers, and trying
// all ~2^30 codes then takes years, not seconds
constexpr u32 ROOM_SECRET_ROUNDS = 100000;
util::sha1_t room_secret(const std::string &room)
{
	if (room.empty())
		return util::sha1_t::null; // no code: nothing to hide
	const u8 *const code = reinterpret_cast<const u8 *>(room.data());
	static const u8 salt[] = { 'c', 'o', 'i', 'n', 'm', 'a', 't', 'e', 's', 0, 0, 0, 1 };
	util::sha1_t u = hmac_sha1(code, room.size(), salt, sizeof(salt));
	util::sha1_t secret = u;
	for (u32 i = 1; i < ROOM_SECRET_ROUNDS; i++)
	{
		u = hmac_sha1(code, room.size(), u.m_raw, sizeof(u.m_raw));
		for (size_t j = 0; j < sizeof(secret.m_raw); j++)
			secret.m_raw[j] ^= u.m_raw[j];
	}
	return secret;
}

util::sha1_t room_hash(const char *what, const util::sha1_t &secret)
{
	return hmac_sha1(secret.m_raw, sizeof(secret.m_raw), reinterpret_cast<const u8 *>(what), strlen(what));
}

u64 packet_tag(const util::sha1_t &key, const u8 *data, size_t size)
{
	return get_u64le(hmac_sha1(key.m_raw, sizeof(key.m_raw), data, size).m_raw);
}

// for the secrets: unpredictable, unlike a seeded generator
u64 random_u64()
{
	std::random_device device;
	return u64(device()) << 32 | device();
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

// as hash_bytes, faster (every system is little-endian): for the ROMs
u64 hash_words(u64 h, const void *data, size_t size)
{
	const u8 *p = reinterpret_cast<const u8 *>(data);
	for (; size >= 8; p += 8, size -= 8)
	{
		u64 word;
		memcpy(&word, p, 8);
		h = (h ^ word) * 0x100000001b3ULL;
	}
	return hash_bytes(h, p, size);
}

constexpr u64 HASH_SEED = 0xcbf29ce484222325ULL;

u32 now_ms() { return u32(osd_ticks() / (osd_ticks_per_second() / 1000)); }


// everything read comes from the network: the reader checks the bounds
class packet_writer
{
public:
	packet_writer() = default;

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
	const std::vector<u8> &data() const { return m_data; }

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
	std::string status() const override;
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
		// frames late that their new inputs come, measured here: the two slowest of the last samples, and
		// the second slowest of those before
		s32 late_slow[2] = { }, late_before = 0;
		size_t late_samples = 0;
		bool late_full = false;     // a whole block measured
		int late = 0;               // the same of our packets, as they tell
		float advantage = 0;        // frames I am ahead of them
		float their_advantage = 0;  // as they see it
		int redo = 0;               // frames it can redo within one, as it says; none until it knows
		transfer_out tx;
		bool session_ready = false;
		bool resync_wanted = false;
		s64 resync_frame = 0;       // its present: the state sent can't be newer
		s64 resync_check = 0;
		// host, until it plays: its addresses as it says (we may see it at home), whom it reached
		udp::endpoint lan;
		udp::endpoint wan;
		std::vector<u64> met;
		u32 joined_ms = 0;
		// neutral inputs after this frame
		s64 left = NO_FRAME;
		// host: a late joiner connects to the others, then plays from `back`, from the state of `back_from`
		bool joining = false;
		s64 back_from = 0;
		s64 back = 0;
		u32 roster_acked = 0;
		size_t notes_acked = 0;
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

	// a line of a player's log, at that player's frame: the frames are the same for all
	struct note_line
	{
		s64 frame;
		std::string text;
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
	float delay_need() const;
	void publish_inputs(s64 frame);
	void measure_load();
	void predict(s64 frame);
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
	void meet_tick(u32 now, bool poll);
	void other_socket();
	const char *room_not_found() const;
	int unmet(int player) const;
	void remove_player(int player, const char *reason);
	void drop_player(int player);
	void player_left(int player, s64 last, s64 from);
	void join_tick();
	void punch_tick(u32 now);
	void punch_peers(u32 now);
	bool may_write_first(u32 now) const;
	void client_game_tick(u32 now);
	void player_back(int player, s64 from, s64 back, udp::endpoint endpoint, u64 nonce);
	void send_roster();
	void handle_roster(packet_reader &in);
	void meet_others(u32 now);
	void lose_connection(const char *reason);
	s64 newest_state(s64 newest);
	void put_player(packet_writer &out, int player) const;
	static other get_player(packet_reader &in);
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
	std::shared_ptr<const std::vector<u8>> compress(const std::vector<u8> &state);
	void start_transfer(int player, s64 frame, std::shared_ptr<const std::vector<u8>> data);
	void tick_transfers();
	void check_timeouts();
	void end_session(const char *reason, bool rejoin = false);

	void write_log();

	// every player sends its log lines to all the others: anyone can attach the whole room's to a
	// report (-netplay_logs); no addresses
	template <typename... T> void note(const char *format, T &&... args) { add_note(util::string_format(format, std::forward<T>(args)...)); }
	void add_note(std::string text, const udp::endpoint *from = nullptr);
	void handle_note(int sender, packet_reader &in);
	void write_notes();

	phase m_phase = phase::BOOT;
	const bool m_host;
	const bool m_rejoining = std::exchange(s_rejoin, false);
	std::string m_end_reason;

	// the host's options, sent to the clients
	u32 m_session = 0;
	int m_me = 0;
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
	int m_delay = MIN_DELAY;        // of our inputs
	u32 m_sample = 0;               // the local inputs of the frame running
	int m_redo = 0;
	std::vector<float> m_busy;      // ms of the last frames, besides waiting and redoing
	double m_load_mark = 0;         // ms so far besides waiting and redoing
	double m_load_redo_ms = 0;      // redoing, at the last estimate
	u64 m_load_redo_frames = 0;
	float m_redo_cost = 0;          // ms of a frame redone
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
	u32 m_inputs_ms = 0;

	std::string m_room;
	util::sha1_t m_key;
	u64 m_nonce = 0;
	std::vector<other> m_others;    // client: whom to connect to
	std::vector<u8> m_roster;
	u32 m_roster_version = 0;
	std::map<u64, udp::endpoint> m_met;  // client: where the others' packets come from
	std::set<u64> m_heard_by;       // client: the others that said they get our packets
	s64 m_back = 0;                 // client that joined during the game: its inputs count from this frame
	static inline bool s_rejoin = false; // the machine restarts after a lost connection, to join again
	u32 m_meet_ms = 0;
	u32 m_mesh_ms = 0;
	std::unique_ptr<netplay_meet> m_meet;
	bool m_nat_reported = false;
	bool m_trackers_reported = false;
	std::optional<udp::endpoint> m_punched; // client: the host's packets come, from there
	std::optional<u32> m_hello_ms;  // client: the first hello to an address of the trackers outside home
	bool m_other_socket = false;    // client: the first one had no welcome
	u32 m_room_message_ms = 0;
	u32 m_punch_ms = 0;
	asio::ip::address_v4 m_lan;
	u32 m_sweep_ms = 0;
	u32 m_sweep_next = 1;           // last byte of the next address
	u32 m_send_failed = 0;
	u32 m_silence_max_ms = 0;       // from another player, during the game
	std::optional<u32> m_room_hello_ms; // client: the first hello to the room's addresses, of this socket
	s32 m_welcome_ms = -1;          // and the welcome after it
	int m_wait_for = 0;             // players at which the host starts by itself
	// client: looking for the room, in it, told who plays, the host's inputs come; the state is there
	// once m_rx.done_id is set
	enum class stage { LOOKING, WELCOMED, STARTED, PLAYING };
	stage m_stage = stage::LOOKING;
	u64 m_tx_bytes = 0, m_rx_bytes = 0;

	std::vector<note_line> m_my_notes;
	std::vector<note_line> m_notes[MAX_PLAYERS];
	std::vector<std::pair<int, std::vector<note_line>>> m_old_notes; // of whoever left a place that another took
	std::string m_notes_path;
	u32 m_notes_ms = 0;

	u32 m_bot_seed = 0;
	s64 m_test_frames = 0;
	s64 m_test_fault = -1;
	long long m_test_dump_from = 0, m_test_dump_to = -1;
	std::string m_log_path;
	FILE *m_hash_log = nullptr, *m_trip_log = nullptr;
	int m_sim_delay = 0, m_sim_jitter = 0, m_sim_loss = 0, m_sim_every = 0, m_sim_hold = 0;
	s64 m_cut_frame = -1;
	u32 m_cut_ms = 0, m_cut_until = 0;
	u32 m_audio_delay_ms = AUDIO_DELAY_MS;
	u16 m_test_port = 0;            // instead of LAN_PORT: the same after a restart, as at home
	static inline bool s_cut_done = false;
	std::mt19937 m_rng;
	std::deque<delayed_packet> m_sim_queue;

	u64 m_rollbacks = 0, m_resim_frames = 0, m_unrecoverable = 0;
	u64 m_desyncs = 0, m_resyncs = 0, m_hash_compared = 0, m_slow_frames = 0;
	osd_ticks_t m_stall_ticks = 0;
	int m_delay_max = MIN_DELAY, m_delay_changes = 0;
	u64 m_delay_histogram[32] = { };
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
	, m_rng(std::random_device()()) // the tests' simulated network
{
	if (machine.options().cheat())
		throw emu_fatalerror("Netplay: cheats must be off");

	for (ioport_port *port : m_ports)
		for (auto &dyn : port->live().writelist)
			m_input_callbacks = m_input_callbacks || dyn.field().type() != IPT_OUTPUT;
	compute_signature();

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
			else if (k == "sim") std::sscanf(v.c_str(), "%d:%d:%d:%d:%d", &m_sim_delay, &m_sim_jitter, &m_sim_loss, &m_sim_every, &m_sim_hold);
			else if (k == "cut" && !s_cut_done) { long long f; unsigned ms; if (std::sscanf(v.c_str(), "%lld:%u", &f, &ms) == 2) { m_cut_frame = f; m_cut_ms = ms; } }
			else if (k == "port") m_test_port = u16(std::stoi(v));
			else if (k == "audio") m_audio_delay_ms = u32(std::stoul(v));
		}
		if (!m_log_path.empty())
		{
			m_hash_log = std::fopen((m_log_path + ".hash.csv").c_str(), "w");
			m_trip_log = std::fopen((m_log_path + ".trip.csv").c_str(), "w");
		}
	}

	if (host)
	{
		const int most = std::min(system_players(machine.ioport().ports()), MAX_PLAYERS);
		if (most < 2)
			throw emu_fatalerror("Netplay: %s can't be played online: it has controls for one player only, or analog ones", machine.system().type.fullname());
		m_players = most;
		m_wait_for = machine.options().netplay_players() ? std::clamp(machine.options().netplay_players(), 2, most) : most;
		m_hash_every = std::clamp(machine.options().netplay_hash_every(), 1, 3600);
		m_session = u32(random_u64()) | 1;
		// another port if taken: the code finds it anyway, and the host writes to the players at home (sweep_lan)
		asio::error_code busy;
		m_socket.open(udp::v4());
		m_socket.bind(udp::endpoint(udp::v4(), u16(machine.options().netplay_host_port())), busy);
		if (busy)
			m_socket.bind(udp::endpoint(udp::v4(), 0));
		const int port = m_socket.local_endpoint().port();
		for (size_t i = 0; i < ROOM_LENGTH; i++)
			m_room += ROOM_LETTERS[random_u64() % (sizeof(ROOM_LETTERS) - 1)];
		note("room %s on UDP port %d: give the code to the others, then press Start (by itself with %d players)", room_name(m_room), port, m_wait_for);
	}
	else
	{
		const std::string target = join_target(machine.options().netplay_join());
		const auto colon = target.rfind(':');
		if (target.empty())
			throw emu_fatalerror("Netplay: -netplay_join needs a room code like K7Q-M3X, or host:port");
		if (colon == std::string::npos)
		{
			m_room = target;
			note("looking for room %s", room_name(m_room));
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
			note("joining by address");
		}
		// LAN_PORT if free: the host writes there (sweep_lan)
		asio::error_code busy;
		m_socket.open(udp::v4());
		m_socket.bind(udp::endpoint(udp::v4(), m_test_port ? m_test_port : LAN_PORT), busy);
		if (busy)
			m_socket.bind(udp::endpoint(udp::v4(), 0));
		m_nonce = random_u64() | 1;
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
	const util::sha1_t secret = room_secret(m_room);
	m_key = room_hash("key", secret);

	if (!m_room.empty() && *machine.options().netplay_trackers())
	{
		netplay_meet::hash hash;
		std::copy_n(room_hash("meet", secret).m_raw, hash.size(), hash.begin());
		m_meet = std::make_unique<netplay_meet>(m_socket, machine.options().netplay_trackers(), machine.options().netplay_stun(), hash);
	}

	if (screen_device *screen = screen_device_enumerator(machine.root_device()).first())
		m_frame_ms = float(screen->frame_period().as_double() * 1000.0);
	m_base_speed = machine.video().speed_factor();

	for (int p = 0; p < MAX_PLAYERS; p++)
	{
		m_input[p].assign(INPUT_RING, 0);
		m_used[p].assign(INPUT_RING, 0);
		m_known[p] = START_FRAME;
	}
}


netplay_session::~netplay_session()
{
	// the next system started from the menu plays alone, unless this one restarts to join again
	if (!s_rejoin)
	{
		m_machine.options().set_value(OPTION_NETPLAY_HOST, 0, OPTION_PRIORITY_CMDLINE);
		m_machine.options().set_value(OPTION_NETPLAY_JOIN, "", OPTION_PRIORITY_CMDLINE);
	}
	send_bye();
	if (m_phase != phase::ENDED && m_phase != phase::BOOT)
		note("left the game");
	write_notes();
	write_log();
	if (m_hash_log)
		std::fclose(m_hash_log);
	if (m_trip_log)
		std::fclose(m_trip_log);
	if (m_slowed)
		m_machine.video().set_speed_factor(m_base_speed);
	m_machine.sound().set_output_delay(attotime::zero);
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
		m_rom_hash = hash_words(m_rom_hash, region->base(), region->bytes());
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
	allocate_slots(WINDOW + 3);
	m_hashes.assign(64, hash_entry());

	// sound samples are floating point, which builds may round differently; the chips' state stays
	compute_hash_skip("", true);
	for (int p = 0; p < m_players; p++)
		m_known[p] = START_FRAME + m_delay; // nobody has inputs before the first boundary
}


std::string netplay_session::status() const
{
	if (m_room.empty() || m_phase == phase::ENDED)
		return std::string();
	if (m_phase != phase::RUNNING)
		return util::string_format("Online, room %s", room_name(m_room));
	return util::string_format("Online, room %s: player %d of %d", room_name(m_room), m_me + 1, m_players);
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
	predict(f);
	if (!m_resim)
		m_sample = m_bot_seed ? bot_word(m_bot_seed, m_me, f + m_delay) : 0;
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
	// the local player's controls are those of player 1, whatever seat they got
	if (!m_resim && !m_bot_seed)
		m_sample |= local_word(index, port.live().digital);
	port.live().digital = compose(index, f);
}

// frames of delay for every other player to redo at most what it can within a frame, and to have
// our inputs before its window ends even when lost once. Their trip: how many frames after they were
// sent the second slowest of our last LOAD_FRAMES to 2 x LOAD_FRAMES new inputs came, as they measure
// it on the frames, as WebRTC's jitter buffer does on the media timestamps: a phone stops sending for
// a while, one way only. An input slow alone is left to the rollbacks. A frame more, a choice: on a
// phone, as few stalls as by the round trip, with less delay and half its peaks
float netplay_session::delay_need() const
{
	float need = 0;
	for (int p = 0; p < m_players; p++)
	{
		const peer &pr = m_peers[p];
		if (p == m_me || !pr.connected)
			continue;
		const float trip = float(pr.late) + 1;
		need = std::max({ need, trip - pr.redo, trip + 1 - (WINDOW - 1) });
	}
	return need;
}

// the inputs read for this frame count for frame + m_delay. They go with their frame, so each player
// has a delay of its own: it grows by giving the frame skipped the inputs before, and shrinks by
// dropping inputs equal to those before, so nothing is lost
void netplay_session::publish_inputs(s64 frame)
{
	const s64 last = frame - 1 + m_delay;
	const u32 previous = m_input[m_me][last % INPUT_RING];
	const float need = delay_need();
	if (m_delay > MIN_DELAY && need <= m_delay - 1 && m_sample == previous)
	{
		m_delay--;
		m_delay_changes++;
	}
	else
	{
		// at most what a machine that redoes nothing needs far away
		if (need > m_delay && m_delay < FAR_RTT_MS / 2 / m_frame_ms)
		{
			m_input[m_me][(last + 1) % INPUT_RING] = previous;
			m_delay++;
			m_delay_changes++;
			m_delay_max = std::max(m_delay_max, m_delay);
		}
		m_input[m_me][(frame + m_delay) % INPUT_RING] = m_sample;
	}
	m_known[m_me] = frame + m_delay;
	m_delay_histogram[std::min<size_t>(m_delay, std::size(m_delay_histogram) - 1)]++;
}

// the frames this machine can redo within one: from the time a frame takes besides waiting and
// redoing, and from what a redone frame costs
void netplay_session::measure_load()
{
	const double mark = double(osd_ticks() - m_machine.video().idle_ticks() - m_stall_ticks) * 1000 / osd_ticks_per_second() - m_rollback_ms_total;
	if (m_load_mark)
		m_busy.push_back(float(mark - m_load_mark));
	m_load_mark = mark;
	if (m_busy.size() < LOAD_FRAMES)
		return;

	const auto high = m_busy.begin() + LOAD_FRAMES * 9 / 10;
	std::nth_element(m_busy.begin(), high, m_busy.end());
	const float busy = *high;
	m_busy.clear();
	if (m_resim_frames > m_load_redo_frames)
		m_redo_cost = float((m_rollback_ms_total - m_load_redo_ms) / (m_resim_frames - m_load_redo_frames));
	m_load_redo_ms = m_rollback_ms_total;
	m_load_redo_frames = m_resim_frames;
	// until something is redone, a frame redone costs as much as one played
	m_redo = std::clamp(int((m_frame_ms - busy) / std::max(m_redo_cost ? m_redo_cost : busy, 0.01f)), 0, WINDOW - 1);
}

void netplay_session::predict(s64 frame)
{
	for (int p = 0; p < m_players; p++)
		if (p != m_me)
			m_used[p][frame % INPUT_RING] = best(p, frame);
}

void netplay_session::apply_frame_to_ports(s64 frame)
{
	predict(frame);
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
	const s64 oldest = std::max(m_floor, boundary - WINDOW);
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
	meet_tick(now, m_host ? !m_all_joined : m_stage < stage::STARTED);
	int joined = 1;
	while (joined < m_players && m_peers[joined].connected)
		joined++;
	// host: of two players who have not reached each other, the last one who came
	int stuck = 0;
	for (int p = 1; p < joined && m_host && !stuck; p++)
		if (const int q = unmet(p); q >= 0)
			stuck = std::max(p, q);
	const bool all_met = !stuck;
	if ((m_host ? !m_all_joined : m_stage < stage::STARTED || m_back) && now - m_room_message_ms >= 1000)
	{
		// every second, to stay on screen; with the game's name, as the screen is black
		m_room_message_ms = now;
		const char *const game = m_machine.system().type.fullname();
		if (m_host && joined < 2)
			m_machine.popmessage("%s\nRoom %s: waiting for the other players", game, room_name(m_room));
		else if (m_host && !all_met)
			m_machine.popmessage("%s\nRoom %s, %d players: connecting them to each other", game, room_name(m_room), joined);
		else if (m_host)
			m_machine.popmessage("%s\nRoom %s, %d players: press Start to begin", game, room_name(m_room), joined);
		else if (m_back)
			m_machine.popmessage("%s\nRoom %s: catching up with the game", game, room_name(m_room));
		else if (m_stage != stage::LOOKING)
			m_machine.popmessage("%s\nRoom %s: waiting for the host to start", game, room_name(m_room));
		else if (m_rejoining)
			m_machine.popmessage("%s\nLost the connection: joining %s again", game, m_room.empty() ? "the host" : "room " + room_name(m_room));
		else if (m_room.empty())
			m_machine.popmessage("%s\nJoining the host", game);
		else
			m_machine.popmessage("%s\nLooking for room %s", game, room_name(m_room));
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
		if (!m_all_joined)
			punch_tick(now);
		tick_transfers();
		bool ready = m_all_joined;
		for (int p = 1; p < m_players; p++)
			ready = ready && (m_peers[p].session_ready || m_peers[p].left != NO_FRAME);
		if (ready)
			finish_lobby();
	}
	else
	{
		if (m_stage < stage::STARTED && now - m_last_hello_ms >= HELLO_MS)
		{
			m_last_hello_ms = now;
			packet_writer out(P_HELLO, 0, 0);
			out.put_string(emulator_info::get_build_version()); // first, in every version: it says which
			out.put_string(m_machine.system().name);
			out.put<u64>(m_layout_hash);
			out.put<u64>(m_rom_hash);
			out.put<u64>(m_input_hash);
			out.put<u64>(m_nonce);
			const std::optional<udp::endpoint> wan = m_meet ? m_meet->public_address() : std::nullopt;
			out.put<u32>(m_lan.to_uint());
			out.put<u16>(m_socket.local_endpoint().port());
			out.put<u32>(wan ? wan->address().to_v4().to_uint() : 0);
			out.put<u16>(wan ? wan->port() : 0);
			// the ones whose packets come, and who get ours
			std::vector<u64> met;
			for (const auto &m : m_met)
				if (m_heard_by.count(m.first) && met.size() < MAX_PLAYERS)
					met.push_back(m.first);
			out.put<u8>(u8(met.size()));
			for (u64 nonce : met)
				out.put<u64>(nonce);
			if (m_stage != stage::LOOKING || m_room.empty())
				send(m_host_endpoint, out);
			else
			{
				send(udp::endpoint(asio::ip::address_v4::broadcast(), LAN_PORT), out);
				if (now - m_sweep_ms >= SWEEP_MS)
				{
					m_sweep_ms = now;
					sweep_lan(out);
				}
				if ((m_punched || (m_meet && !m_meet->peers().empty() && may_write_first(now))) && !m_room_hello_ms)
					m_room_hello_ms = now;
				if (m_punched)
					send(*m_punched, out);
				else if (m_meet && may_write_first(now))
				{
					// only a router between homes shuts sockets; at home another port would lose the way in
					// that the host's firewall opens (sweep_lan)
					const auto me = m_meet->public_address();
					for (const udp::endpoint &endpoint : m_meet->peers())
					{
						if (!m_hello_ms && (!me || endpoint.address() != me->address()))
							m_hello_ms = now;
						send(endpoint, out);
					}
				}
			}
		}
		// not once the host's punches come: the socket isn't shut to it
		if (m_stage == stage::LOOKING && !m_other_socket && !m_punched && m_hello_ms && now - *m_hello_ms >= WELCOME_MS)
			other_socket();
		if (m_stage == stage::WELCOMED)
		{
			meet_others(now);
			punch_peers(now);
		}
		if (m_stage == stage::LOOKING && now - m_lobby_start_ms >= JOIN_TIMEOUT_MS)
		{
			end_session(m_rejoining ? "lost the connection, and could not join again" : m_room.empty() ? "no answer from the host" : room_not_found());
			return;
		}
		if (m_stage != stage::LOOKING && now - m_peers[0].last_rx_ms >= PEER_TIMEOUT_MS)
		{
			end_session("no packets from the host");
			return;
		}
		if (m_rx.ready)
		{
			// the machine is loaded when the session starts
			slot &s = slot_for(m_rx.frame);
			s.data = std::move(m_rx.raw);
			s.frame = m_rx.frame;
			s.valid = true;
			m_rx.ready = false;
			m_cur = m_rx.frame;
		}
		const bool loaded = m_rx.done_id != 0;
		// about a round trip (0.08 to 0.19 s measured): a lost one would hold the start
		if (m_stage >= stage::STARTED && now - m_last_hello_ms >= 100)
		{
			// until the host answers; a late joiner also says which inputs came, to get the next ones
			m_last_hello_ms = now;
			if (loaded)
			{
				m_send_ack = true;
				send_chunk_ack();
			}
			if (m_back)
				send_inputs();
		}
		// during the game, once the others' inputs are there up to where ours start
		bool caught_up = true;
		for (int p = 0; p < m_players; p++)
			caught_up = caught_up && (p == m_me || m_known[p] >= m_back - 1 - m_delay);
		if (loaded && (m_back ? m_stage >= stage::STARTED && caught_up : m_stage == stage::PLAYING))
			finish_lobby();
	}
	flush_simulated();
	if (now - m_notes_ms >= NOTES_WRITE_MS)
		write_notes();

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
	// a late joiner runs from the host's present up to where its inputs count
	const s64 from = m_cur;
	load_state(slot_for(from).data);
	// every frame drawn, so that the compared ones are (video_manager::set_frameskip)
	m_machine.video().set_frameskip(0);
	m_phase = phase::RUNNING;
	m_floor = m_confirmed = from;
	m_rollback_to = NO_FRAME;
	if (m_back)
		resimulate(from, m_back - 1 - m_delay, false);
	else
		apply_frame_to_ports(from);
	const u32 now = now_ms();
	for (int p = 0; p < m_players; p++)
	{
		m_peers[p].last_rx_ms = now;
		m_peers[p].frame_rx_ms = now;
	}
	m_session_start = osd_ticks();
	m_machine.sound().set_output_delay(attotime::from_msec(m_audio_delay_ms));
	m_machine.popmessage("Player %d of %d", m_me + 1, m_players);
	note("session started as player %d of %d", m_me + 1, m_players);
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
	measure_load();
	publish_inputs(b);
	const bool saved = save_slot(b);
	if (b == m_test_fault)
		corrupt_state(b);
	if (b == m_cut_frame && !s_cut_done)
	{
		s_cut_done = true;
		m_cut_until = now_ms() + m_cut_ms;
		osd_printf_info("Netplay test: no packets for %u ms\n", m_cut_ms);
	}

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
		if (b - slowest < WINDOW)
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
	if (m_host)
		join_tick();
	else
		client_game_tick(now_ms());

	if (now_ms() - m_notes_ms >= NOTES_WRITE_MS)
		write_notes();
	if (m_test_frames && b >= m_test_frames)
		end_session("test finished");
}

void netplay_session::stall_tick()
{
	const osd_ticks_t start = osd_ticks();
	osd_sleep(osd_ticks_per_second() / 1000);
	poll();
	send_chunk_ack();
	// as often as playing: this loop turns every millisecond, and sending at that pace filled a phone's
	// upload, whose queue then held the inputs for seconds
	if (now_ms() - m_inputs_ms >= u32(m_frame_ms))
		send_inputs();
	tick_transfers();
	flush_simulated();
	check_timeouts();
	if (m_host)
		join_tick();
	else
		client_game_tick(now_ms());
	m_machine.osd().update(true);
	m_stall_ticks += osd_ticks() - start;
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
	note("desync at frame %d", int(e.frame));
	request_resync(m_cur, e.frame);
}

void netplay_session::request_resync(s64 boundary, s64 frame)
{
	const u32 now = now_ms();
	if (m_resync_pending && now - m_resync_ms < RESYNC_RETRY_MS)
		return;
	m_resync_pending = true;
	m_resync_ms = now;
	m_machine.popmessage("Repairing a desync");
	packet_writer out(P_RESYNC, m_session, m_me);
	out.put<s32>(s32(boundary));
	out.put<s32>(s32(frame));
	send(m_peers[0].endpoint, out);
}

void netplay_session::apply_received_state(s64 boundary)
{
	const s64 frame = m_rx.frame;
	m_rx.ready = false;
	if (frame > boundary || boundary - frame >= INPUT_RING / 2)
		return; // too old to catch up from (the others' inputs may be gone): the next hash mismatch asks again

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
	m_machine.popmessage("Desync repaired");
	note("state of frame %d loaded at frame %d", int(frame), int(boundary));
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
	note("%d items differ: %s", count, names);
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
		const checked_state &c = m_checked[(pr.resync_check / m_hash_every) % 2];
		const s64 newest = newest_state(pr.resync_frame);
		if (c.frame == pr.resync_check && c.frame <= std::min(m_confirmed, pr.resync_frame))
		{
			pr.resync_wanted = false;
			start_transfer(p, c.frame, compress(c.data));
		}
		else if (newest >= 0)
		{
			pr.resync_wanted = false;
			start_transfer(p, newest, compress(slot_for(newest).data));
		}
	}
}

// the newest state kept, of a frame whose inputs are all known and no later than `newest`; -1 if none
s64 netplay_session::newest_state(s64 newest)
{
	newest = std::min(m_confirmed, newest);
	const s64 oldest = std::max<s64>(START_FRAME, newest - s64(slot_count()) + 1);
	for (s64 f = newest; f >= oldest; f--)
		if (slot_valid(f))
			return f;
	return -1;
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
		if (p == m_me || !pr.connected || boundary < pr.back)
			continue; // and one that joined, until it plays
		const float remote = float(pr.frame) + (float(now - pr.frame_rx_ms) + pr.rtt / 2) / m_frame_ms;
		pr.advantage += (float(boundary) - remote - pr.advantage) * 0.05f; // choices: smoothing, thresholds and the 2% slowdown
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
//  meeting: everybody announces itself on the
//  trackers under the room code, and writes to the
//  others: both sides writing opens the routers
//  (UDP hole punching). At home the players
//  broadcast, and write to every address to open
//  the firewalls
//-------------------------------------------------

void netplay_session::meet_tick(u32 now, bool poll)
{
	if (!m_meet)
		return;
	m_meet->tick(now, poll);
	if (m_host && !m_trackers_reported && m_meet->trackers_silent())
	{
		m_trackers_reported = true;
		note("cannot reach the trackers: players outside your network will not find this room; check the connection");
	}
	if (!m_nat_reported && m_meet->symmetric_nat())
	{
		m_nat_reported = true;
		note("your router uses a different port for every destination (symmetric NAT): players outside your network will probably not reach you; use a VPN such as Tailscale");
	}
}

// client: no welcome on the first socket, whose router may have shut it to the host (WELCOME_MS)
void netplay_session::other_socket()
{
	m_other_socket = true;
	note("no answer from the room's addresses: trying from another port");
	asio::error_code ec;
	m_socket.close(ec);
	m_socket.open(udp::v4(), ec);
	m_socket.bind(udp::endpoint(udp::v4(), 0), ec);
	m_socket.non_blocking(true, ec);
	m_socket.set_option(asio::socket_base::broadcast(true), ec);
	m_meet->restart();
	m_room_hello_ms.reset();
	// the hellos at once, before the host's punches can come: as soon as the trackers list this socket,
	// 0.24 s later
	m_last_hello_ms = now_ms() - HELLO_MS;
}

// client: where looking for the room stopped
const char *netplay_session::room_not_found() const
{
	if (!m_meet)
		return "room not found";
	if (!m_meet->tracker_answered())
		return "cannot reach the trackers, where rooms are found: check the connection";
	if (m_meet->peers().empty())
		return "room not found: nobody is announcing this code (is it right, and is the host waiting?)";
	return "found the room, but the host does not answer: a router or firewall in the way? Try Tailscale";
}

// client: to the other players, at home and through their routers
void netplay_session::meet_others(u32 now)
{
	if (now - m_meet_ms < PUNCH_MS)
		return;
	m_meet_ms = now;
	const bool first = may_write_first(now);
	for (const other &o : m_others)
	{
		packet_writer out(P_MEET, m_session, m_me);
		out.put<u64>(m_nonce);
		out.put<u8>(m_met.count(o.nonce) ? 1 : 0);
		for (const udp::endpoint *e : { &o.endpoint, &o.lan, &o.wan })
			if (e->port() && (e == &o.endpoint || *e != o.endpoint) && (first || home_address(e->address())))
				send(*e, out);
	}
}

//-------------------------------------------------
//  transport
//-------------------------------------------------

void netplay_session::send(const udp::endpoint &to, const packet_writer &packet, const util::sha1_t &key)
{
	std::vector<u8> data = packet.tagged(key);
	if (m_cut_until && s32(m_cut_until - now_ms()) > 0)
		return;
	if (m_sim_delay || m_sim_jitter || m_sim_loss || m_sim_every)
	{
		if (int(m_rng() % 100) < m_sim_loss)
			return;
		u32 time = now_ms();
		if (m_sim_every && int(time % m_sim_every) < m_sim_hold)
			time += m_sim_hold - time % m_sim_every;
		const u32 extra = m_sim_jitter ? m_rng() % (m_sim_jitter + 1) : 0;
		m_sim_queue.push_back({ time + m_sim_delay + extra, to, std::move(data) });
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
	// 0.13% lost on a phone, 0.82% on a wifi: three, or the others wait PEER_TIMEOUT_MS
	for (int i = 0; i < 3; i++)
		send_all(packet_writer(P_BYE, m_session, m_me));
	flush_simulated(true);
}

void netplay_session::poll()
{
	flush_simulated();
	u8 buffer[2048];
	for (int errors = 0; errors < 64; )
	{
		udp::endpoint from;
		asio::error_code ec;
		const size_t size = m_socket.receive_from(asio::buffer(buffer), from, 0, ec);
		if (ec == asio::error::would_block || ec == asio::error::try_again)
			return;
		if (ec)
		{
			errors++; // Windows: ICMP port unreachable from a peer that left; the bound, a choice, only ends this poll
			continue;
		}
		m_rx_bytes += size;
		if (m_cut_until && s32(m_cut_until - now_ms()) > 0)
			continue;
		if (!m_meet || !m_meet->handle(from, buffer, size))
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
		static const util::sha1_t open = room_hash("key", room_secret(std::string()));
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
		// there to open the sender's router; from the host, until the game, also to say that it's
		// there (in the game its inputs do: one dropped by the host gets only its punches)
		if (!m_host && m_stage != stage::LOOKING && m_phase != phase::RUNNING && from == m_host_endpoint)
			m_peers[0].last_rx_ms = now_ms();
		if (!m_host && m_stage == stage::LOOKING && sender == 0)
			m_punched = from; // the host has written first: the hellos can go to it
		return;
	}

	if (type == P_WELCOME || type == P_REJECT)
	{
		// until the welcome, the room's host may answer from any of its addresses
		if (m_host || m_stage >= stage::STARTED || (from != m_host_endpoint && (m_room.empty() || m_stage != stage::LOOKING)))
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
		const bool heard = in.get<u8>() != 0;
		if (m_host || !in.ok() || nonce == m_nonce)
			return;
		if (heard)
			m_heard_by.insert(nonce);
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
	if (m_phase == phase::RUNNING)
		m_silence_max_ms = std::max(m_silence_max_ms, now_ms() - pr.last_rx_ms);
	pr.last_rx_ms = now_ms();

	switch (type)
	{
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
			if (in.ok() && frame >= START_FRAME && boundary >= frame && boundary <= m_cur + INPUT_RING / 2)
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
	case P_ROSTER:
		if (!m_host && sender == 0)
			handle_roster(in);
		break;
	case P_NOTE:
		handle_note(sender, in);
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
	const std::string build = in.get_string();
	const std::string system = in.get_string();
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
	if (!in.ok() && build == emulator_info::get_build_version())
		return; // another version may write the rest otherwise: it's told that the version differs

	int player = -1;
	for (int p = 1; p < m_players && player < 0; p++)
		if ((m_peers[p].connected || m_peers[p].joining) && m_peers[p].endpoint == from)
			player = p;
	// a player already in, by another way: at home and through the router
	for (int p = 1; p < m_players; p++)
		if ((m_peers[p].connected || m_peers[p].joining) && m_peers[p].nonce == nonce && m_peers[p].endpoint != from)
			return;
	const bool running = m_phase != phase::LOBBY || m_all_joined;

	// the same address with another nonce: that player's Coinmates started again, having lost
	// the connection before the host noticed: the old one is gone
	if (player >= 0 && m_peers[player].nonce != nonce)
	{
		if (!running)
			remove_player(player, nullptr);
		else if (m_phase != phase::RUNNING)
			return; // the game is about to start: it's dropped then, and the next hello joins
		else if (m_peers[player].joining)
			m_peers[player].joining = false;
		else
			drop_player(player);
		player = -1;
	}

	std::string reason;
	if (build != emulator_info::get_build_version())
		reason = util::string_format("different Coinmates version: the host has %s, you have %s", emulator_info::get_build_version(), build);
	else if (system != m_machine.system().name)
		reason = util::string_format("different game: the host is playing %s", m_machine.system().type.fullname());
	else if (rom != m_rom_hash)
		reason = "different ROM contents";
	else if (layout != m_layout_hash)
		reason = "different save state layout";
	else if (inputs != m_input_hash)
		reason = "different inputs";

	if (reason.empty() && player < 0)
	{
		// a free place; during the game, the place of one who left, and if one is about to
		// leave, the next hello
		bool silent = false;
		for (int p = 1; p < m_players && player < 0; p++)
		{
			const peer &pr = m_peers[p];
			if (!running ? !pr.connected : (pr.left != NO_FRAME && !pr.joining && m_phase == phase::RUNNING))
				player = p;
			silent = silent || (running && pr.connected && now_ms() - pr.last_rx_ms >= SILENT_MS);
		}
		if (player < 0 && (silent || (running && m_phase != phase::RUNNING)))
			return; // the next hello, once a place is free or the game has started
		if (player < 0)
			reason = running ? "the game has already started, with every place taken" : "the room is full";
		else
		{
			peer &pr = m_peers[player];
			if (running)
				pr.joining = true; // connected once it has the state
			else
			{
				pr = peer();
				pr.connected = true;
			}
			pr.endpoint = from;
			pr.nonce = nonce;
			pr.joined_ms = now_ms();
			add_note(util::string_format(running ? "player %d joining" : "player %d joined", player + 1), &from);
		}
	}
	if (!reason.empty())
	{
		packet_writer out(P_REJECT, 0, 0);
		out.put_string(reason);
		send(from, out, key);
		add_note(util::string_format("refused a player: %s", reason), &from);
		return;
	}

	peer &pr = m_peers[player];
	pr.last_rx_ms = now_ms();
	pr.lan = udp::endpoint(asio::ip::address_v4(lan_address), lan_port);
	pr.wan = udp::endpoint(asio::ip::address_v4(wan_address), wan_port);
	pr.met = std::move(met);
	send_welcome(player, key);
	if (pr.connected && running)
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
	note("starting with %d players", players);
	for (int p = 1; p < m_players; p++)
	{
		send_start(p);
		start_transfer(p, START_FRAME, compress(slot_for(START_FRAME).data));
	}
}

// host: the first client playing or waiting that `player` says it hasn't reached yet, or -1
int netplay_session::unmet(int player) const
{
	const std::vector<u64> &met = m_peers[player].met;
	for (int q = 1; q < m_players; q++)
		if (q != player && m_peers[q].connected && std::find(met.begin(), met.end(), m_peers[q].nonce) == met.end())
			return q;
	return -1;
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
	note("player %d left the room%s%s", player + 1, reason ? ": " : "", reason ? reason : "");
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
	m_machine.popmessage("Player %d left\nThe room code is in the menu, to join again", player + 1);
	note("player %d left after frame %d", player + 1, int(last));
}

// host, during the game: a late joiner connects to the others, gets the state, then plays
void netplay_session::join_tick()
{
	const u32 now = now_ms();
	bool free = false;
	for (int p = 1; p < m_players; p++)
	{
		peer &pr = m_peers[p];
		if (pr.left == NO_FRAME)
			continue;
		free = true;
		if (!pr.joining)
			continue;
		if (now - pr.last_rx_ms >= PEER_TIMEOUT_MS)
		{
			pr.joining = false;
			note("player %d went away while joining", p + 1);
			continue;
		}
		if (const int q = unmet(p); q >= 0)
		{
			if (now - m_peers[q].last_rx_ms >= SILENT_MS)
				pr.joined_ms = now; // not its fault: wait for that one to come back, or to be dropped
			else if (now - pr.joined_ms >= MESH_TIMEOUT_MS)
			{
				packet_writer out(P_REJECT, 0, 0);
				out.put_string("cannot connect to the other players: try Tailscale");
				send(pr.endpoint, out);
				pr.joining = false;
				note("player %d cannot connect to the other players", p + 1);
			}
			continue;
		}
		// a state with the last inputs of whoever left, once the others know that they left
		const s64 from = newest_state(m_cur);
		bool settled = from >= 0;
		for (int q = 1; q < m_players; q++)
		{
			const peer &other = m_peers[q];
			settled = settled && (other.connected ? other.roster_acked == m_roster_version : from >= other.left);
		}
		if (!settled)
			continue;
		// it plays once the state has come, at the speed of the game's transfers, and it has caught up
		const auto data = compress(slot_for(from).data);
		const s64 transfer = data ? s64(data->size() / CHUNK_SIZE + 1) * MS_PER_CHUNK_PLAYING / s64(m_frame_ms) : 0;
		player_back(p, from, m_cur + std::min<s64>(BACK_FRAMES + transfer, INPUT_RING / 2), pr.endpoint, pr.nonce);
		send_start(p);
		start_transfer(p, from, data);
	}

	// as in the lobby: the room on the trackers, and the router open to its players
	if (free)
	{
		meet_tick(now, true);
		punch_tick(now);
	}
}

// host: to the room's addresses on the trackers and to the home network: this opens the router to their
// packets
void netplay_session::punch_tick(u32 now)
{
	if (now - m_sweep_ms >= SWEEP_MS)
	{
		m_sweep_ms = now;
		sweep_lan(packet_writer(P_PUNCH, 0, 0));
	}
	punch_peers(now);
}

// whoever is in the room writes first to the addresses on the trackers: at once to a new one, as its
// joiner waits for that (may_write_first)
void netplay_session::punch_peers(u32 now)
{
	if (m_meet && (m_meet->peers_changed() || now - m_punch_ms >= PUNCH_MS))
	{
		m_punch_ms = now;
		for (const udp::endpoint &endpoint : m_meet->peers())
			send(endpoint, packet_writer(P_PUNCH, 0, m_me));
	}
}

// client: on mobile data, nothing passes any more between a socket and an address that wrote to it
// first; so to an address outside home it writes once those in the room must have found it on the
// trackers and written; on another socket, its router is the one: it writes first
bool netplay_session::may_write_first(u32 now) const
{
	if (!m_meet || m_other_socket || m_meet->trackers_silent())
		return true;
	const std::optional<u32> announced = m_meet->announced_ms();
	return announced && now - *announced >= HOST_FOUND_MS;
}

// client, during the game: while a place is free, as in the lobby
void netplay_session::client_game_tick(u32 now)
{
	meet_others(now);
	for (int p = 1; p < m_players; p++)
		if (p != m_me && m_peers[p].left != NO_FRAME)
		{
			meet_tick(now, true);
			punch_peers(now);
			break;
		}
}

// its inputs are neutral up to `back`: from there on they are its own; it starts from the state of `from`
void netplay_session::player_back(int player, s64 from, s64 back, udp::endpoint endpoint, u64 nonce)
{
	peer &pr = m_peers[player];
	for (s64 f = std::max(pr.left + 1, back - INPUT_RING + 1); f < back; f++)
		m_input[player][f % INPUT_RING] = 0;
	m_known[player] = back - 1;
	if (!m_notes[player].empty())
		m_old_notes.emplace_back(player, std::move(m_notes[player]));
	m_notes[player].clear();
	pr = peer();
	pr.connected = true;
	pr.endpoint = endpoint;
	pr.nonce = nonce;
	pr.acked = from - 1;    // it needs our inputs from the frame of its state on
	pr.frame = from;
	pr.last_rx_ms = pr.frame_rx_ms = now_ms();
	pr.back_from = from;
	pr.back = back;
	m_machine.popmessage("Player %d joined", player + 1);
	note("player %d plays from frame %d, with the state of frame %d", player + 1, int(back), int(from));
}

// host: who plays, since when; who left, with its last inputs; who is joining. A place is taken
// again once all the clients have it (join_tick)
void netplay_session::send_roster()
{
	packet_writer places;
	for (int p = 1; p < m_players; p++)
	{
		const peer &pr = m_peers[p];
		places.put<u8>(pr.connected ? 0 : pr.joining ? 2 : 1);
		if (pr.connected)
		{
			put_player(places, p);
			places.put<s32>(s32(pr.back_from));
			places.put<s32>(s32(pr.back));
			continue;
		}
		const s64 first = std::max<s64>(START_FRAME, pr.left - MAX_INPUTS_PER_PACKET + 1);
		places.put<s32>(s32(pr.left));
		places.put<s32>(s32(first));
		for (s64 f = first; f <= pr.left; f++)
			places.put<u32>(m_input[p][f % INPUT_RING]);
		if (pr.joining)
			put_player(places, p);
	}
	if (places.data() != m_roster)
	{
		m_roster = places.data();
		m_roster_version++;
	}

	packet_writer out(P_ROSTER, m_session, m_me);
	out.put<u32>(m_roster_version);
	out.put_bytes(m_roster.data(), m_roster.size());
	for (int p = 1; p < m_players; p++)
		if (m_peers[p].connected && m_peers[p].roster_acked != m_roster_version)
			send(m_peers[p].endpoint, out);
}

void netplay_session::handle_roster(packet_reader &in)
{
	struct place
	{
		u8 state = 0;   // 0 plays, 1 left, 2 left and another is joining
		other who{};
		s64 from = 0, back = 0, left = 0, first = 0;
		u32 inputs[MAX_INPUTS_PER_PACKET];
	};
	const u32 version = in.get<u32>();
	if (version <= m_roster_version)
		return; // also an older one, come late
	place places[MAX_PLAYERS];
	for (int p = 1; p < m_players; p++)
	{
		place &pl = places[p];
		pl.state = in.get<u8>();
		if (pl.state == 0)
		{
			pl.who = get_player(in);
			pl.from = in.get<s32>();
			pl.back = in.get<s32>();
			if (pl.back && (pl.from < START_FRAME || pl.back <= pl.from))
				return;
			continue;
		}
		pl.left = in.get<s32>();
		pl.first = in.get<s32>();
		if (pl.state > 2 || pl.first < START_FRAME || pl.first > pl.left || pl.left - pl.first >= MAX_INPUTS_PER_PACKET)
			return;
		for (s64 f = pl.first; f <= pl.left; f++)
			pl.inputs[f - pl.first] = in.get<u32>();
		if (pl.state == 2)
			pl.who = get_player(in);
	}
	if (!in.ok())
		return;

	m_roster_version = version;
	m_others.clear();
	for (int p = 1; p < m_players; p++)
	{
		const place &pl = places[p];
		peer &pr = m_peers[p];
		if (p == m_me)
			continue;
		if (pl.state != 0 && pr.left == NO_FRAME)
		{
			const s64 from = std::min(m_known[p], pl.left) + 1;
			for (s64 f = pl.first; f <= pl.left; f++)
				if (f > m_known[p])
					m_input[p][f % INPUT_RING] = pl.inputs[f - pl.first];
			player_left(p, pl.left, from);
		}
		if (pl.state == 2)
			m_others.push_back(pl.who); // we connect to it, as in the lobby
		if (pl.state == 0 && pr.left != NO_FRAME)
		{
			const auto met = m_met.find(pl.who.nonce);
			player_back(p, pl.from, pl.back, met != m_met.end() ? met->second : pl.who.endpoint, pl.who.nonce);
		}
	}
}

void netplay_session::send_welcome(int player, const util::sha1_t &key)
{
	packet_writer out(P_WELCOME, m_session, 0);
	out.put<u8>(u8(player));
	out.put<u8>(u8(m_players));
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
		if (p != player && m_peers[p].connected)
			put_player(out, p);
	send(m_peers[player].endpoint, out, key);
}

void netplay_session::put_player(packet_writer &out, int player) const
{
	const peer &pr = m_peers[player];
	out.put<u64>(pr.nonce);
	for (const udp::endpoint &endpoint : { pr.endpoint, pr.lan, pr.wan })
	{
		out.put<u32>(endpoint.address().to_v4().to_uint());
		out.put<u16>(endpoint.port());
	}
}

netplay_session::other netplay_session::get_player(packet_reader &in)
{
	other o;
	o.nonce = in.get<u64>();
	for (udp::endpoint *endpoint : { &o.endpoint, &o.lan, &o.wan })
	{
		const u32 address = in.get<u32>();
		const u16 port = in.get<u16>();
		*endpoint = udp::endpoint(asio::ip::address_v4(address), port);
	}
	return o;
}

// again at every hello until the start: the place can change when somebody leaves
void netplay_session::handle_welcome(const udp::endpoint &from, packet_reader &in)
{
	const int player = in.get<u8>();
	const int players = in.get<u8>();
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
		o = get_player(in);
	if (!in.ok() || players < 2 || players > MAX_PLAYERS || player < 1 || player >= players || hash_every < 1 || others.size() >= MAX_PLAYERS)
		return;
	m_me = player;
	m_others = std::move(others);
	m_peers[0].last_rx_ms = now_ms();
	if (m_stage != stage::LOOKING)
		return;

	if (m_room.empty())
	{
		m_room = clean_room(room);
		m_key = room_hash("key", room_secret(m_room));
	}
	m_host_endpoint = from;
	m_players = players;
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
	m_stage = stage::WELCOMED;
	if (m_room_hello_ms)
		m_welcome_ms = s32(now_ms() - *m_room_hello_ms);
}

// a late joiner also gets the free places, and from when it plays
void netplay_session::send_start(int player)
{
	packet_writer out(P_START, m_session, 0);
	out.put<u8>(u8(m_players));
	out.put<s32>(s32(m_peers[player].back_from));
	out.put<s32>(s32(m_peers[player].back));
	for (int p = 1; p < m_players; p++)
	{
		out.put<u8>(m_peers[p].connected ? 1 : 0);
		if (m_peers[p].connected)
			put_player(out, p);
	}
	send(m_peers[player].endpoint, out);
}

void netplay_session::handle_start(packet_reader &in)
{
	const int players = in.get<u8>();
	const s64 from = in.get<s32>();
	const s64 back = in.get<s32>();
	if (m_stage >= stage::STARTED || players < 2 || players > MAX_PLAYERS || (back && (from < START_FRAME || back <= from)))
		return;
	u64 nonces[MAX_PLAYERS] = { };
	udp::endpoint endpoints[MAX_PLAYERS];
	for (int p = 1; p < players; p++)
	{
		if (!in.get<u8>())
			continue;
		const other o = get_player(in);
		const auto met = m_met.find(o.nonce);
		nonces[p] = o.nonce;
		endpoints[p] = met != m_met.end() ? met->second : o.endpoint;
	}
	// our place is the one with our nonce: the places may have moved since the last welcome
	const auto me = std::find(std::begin(nonces), std::begin(nonces) + players, m_nonce);
	if (!in.ok() || me == std::begin(nonces) + players)
		return;
	m_me = int(me - std::begin(nonces));
	m_players = players;
	m_back = back;
	m_others.clear(); // from now on the roster tells who joins
	for (int p = 0; p < m_players; p++)
	{
		peer &pr = m_peers[p];
		if (p == m_me)
			continue;
		if (p && !nonces[p])
		{
			pr.connected = false;
			pr.left = from;
			m_known[p] = NO_FRAME;
			continue;
		}
		if (p)
		{
			pr.connected = true;
			pr.endpoint = endpoints[p];
			pr.last_rx_ms = now_ms();
		}
		if (back)
		{
			// their inputs come from the frame of the state on; ours count from `back`
			m_known[p] = from - 1;
			pr.acked = back - 1;
			pr.frame = from;
		}
	}
	if (back)
		m_cur = from;
	m_stage = stage::STARTED;
}

void netplay_session::send_inputs()
{
	if (m_host)
		send_roster();
	const s64 newest = m_known[m_me];
	const u32 now = now_ms();
	m_inputs_ms = now;
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
		out.put<u8>(u8(m_redo));
		// before the delay grows, we stop a window ahead of their inputs and see them come no later than
		// that: at first, half the round trip, and the frame that a sample adds to it on average
		const s32 late = std::max(pr.late_slow[1], pr.late_full ? pr.late_before : s32(pr.rtt / 2 / m_frame_ms) + 1);
		out.put<u8>(u8(std::clamp(late, 0, 255)));
		out.put<u16>(u16(m_notes[p].size()));
		out.put<u32>(m_roster_version);
		out.put<s32>(s32(first));
		out.put<u8>(u8(count));
		for (int i = 0; i < count; i++)
			out.put<u32>(m_input[m_me][(first + i) % INPUT_RING]);
		send(pr.endpoint, out);

		if (pr.notes_acked < m_my_notes.size())
		{
			const note_line &n = m_my_notes[pr.notes_acked];
			packet_writer line(P_NOTE, m_session, m_me);
			line.put<u16>(u16(pr.notes_acked));
			line.put<s32>(s32(n.frame));
			line.put_string(std::string_view(n.text).substr(0, MAX_NOTE_SIZE));
			send(pr.endpoint, line);
		}
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
	const int redo = in.get<u8>();
	const int late = in.get<u8>();
	const size_t notes = in.get<u16>();
	const u32 roster = in.get<u32>();
	const s64 first = in.get<s32>();
	const int count = in.get<u8>();
	if (!in.ok() || in.remaining_size() < size_t(count) * 4)
		return;
	if (notes <= m_my_notes.size())
		pr.notes_acked = notes;
	if (m_host && roster <= m_roster_version)
		pr.roster_acked = std::max(pr.roster_acked, roster);

	const u32 now = now_ms();
	float sample = -1;
	if (frame >= pr.frame)
	{
		pr.frame = frame;
		pr.frame_rx_ms = now;
		pr.their_advantage = advantage;
		pr.redo = redo;
		pr.late = late;
		pr.echo_time = time;
		pr.echo_rx_ms = now;
		if (echo)
		{
			sample = float(now - echo - hold);
			if (sample >= 0 && sample < 10000)
			{
				pr.rtt = pr.rtt ? pr.rtt + (sample - pr.rtt) * 0.1f : sample;
			}
		}
	}
	if (acked <= m_known[m_me])
		pr.acked = std::max(pr.acked, acked);

	const s64 known = m_known[sender];
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

	// how late the oldest of the new inputs comes: its packet's frames on the way, and one for each input
	// past the first, whose packet was lost or overtaken. A packet sent again while they wait brings none,
	// and says nothing of the way
	const bool brought = m_known[sender] > known;
	const s32 came = s32(m_cur - frame) + s32(m_known[sender] - known - 1);
	if (brought)
	{
		if (!pr.late_samples)
			pr.late_slow[0] = pr.late_slow[1] = came;
		else if (came > pr.late_slow[0])
			pr.late_slow[1] = std::exchange(pr.late_slow[0], came);
		else
			pr.late_slow[1] = std::max(pr.late_slow[1], came);
		if (++pr.late_samples == LOAD_FRAMES)
		{
			pr.late_before = pr.late_slow[1];
			pr.late_samples = 0;
			pr.late_full = true;
		}
	}
	if (m_trip_log)
		std::fprintf(m_trip_log, "%u,%d,%d,%d,%d,%.0f\n", now, sender + 1, int(brought), int(s32(now - time)), int(came), sample);
	if (sender == 0 && m_stage == stage::STARTED)
		m_stage = stage::PLAYING;
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

std::shared_ptr<const std::vector<u8>> netplay_session::compress(const std::vector<u8> &state)
{
	auto data = std::make_shared<std::vector<u8>>(ZSTD_compressBound(state.size()));
	const size_t size = ZSTD_compress(data->data(), data->size(), state.data(), state.size(), 1);
	if (ZSTD_isError(size))
		return nullptr;
	data->resize(size);
	return data;
}

void netplay_session::start_transfer(int player, s64 frame, std::shared_ptr<const std::vector<u8>> data)
{
	if (!data)
	{
		end_session("cannot compress the state");
		return;
	}
	const size_t size = data->size();
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
		const u32 elapsed = now - tx.last_tick_ms;
		u32 budget = std::min((m_phase == phase::RUNNING) ? elapsed / MS_PER_CHUNK_PLAYING : elapsed * CHUNKS_PER_MS, CHUNK_WINDOW);
		if (budget)
			tx.last_tick_ms = now;
		for (u32 tried = 0; budget && tried < 2 * CHUNK_WINDOW; tried++)
		{
			const u32 limit = std::min(tx.total, tx.next_missing + CHUNK_WINDOW);
			if (tx.send_pos >= limit)
			{
				// after a round trip, once acknowledgements would have come; the far one until it is measured
				if (now - tx.last_pass_ms < (pr.rtt ? u32(pr.rtt) : FAR_RTT_MS))
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
	if (!in.ok() || frame < START_FRAME)
		return;
	m_send_ack = true;
	if (id <= m_rx.done_id)
		return; // done already, or a late one of an older transfer: the host numbers them in order

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
	if (done && m_stage < stage::STARTED)
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
	else if (done && pr.session_ready && m_phase != phase::RUNNING)
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
			lose_connection("no packets from the host");
		else if (silent >= PEER_TIMEOUT_MS + FAR_RTT_MS / 2 + u32(m_frame_ms)) // the host's word that it dropped them, maybe lost once
			lose_connection(util::string_format("no packets from player %d", p + 1).c_str());
	}
}

void netplay_session::end_session(const char *reason, bool rejoin)
{
	if (m_phase == phase::ENDED)
		return;
	if (m_phase == phase::RUNNING)
		note("frames of input delay: %d, at most %d; frames this machine redoes within one: %d", m_delay, m_delay_max, m_redo);
	note("session over: %s%s", reason, rejoin ? ": joining again" : "");
	s_end_message = rejoin ? std::string() : util::string_format("%s: %s", m_machine.system().type.fullname(), reason);
	send_bye();
	m_end_reason = reason;
	m_phase = phase::ENDED;
	write_notes();
	write_log();
	s_rejoin = rejoin;
	if (rejoin)
		m_machine.schedule_hard_reset();
	else
		m_machine.schedule_exit();
}

// a client, once playing: its place stays free on the host, to take again
void netplay_session::lose_connection(const char *reason)
{
	end_session(reason, !m_host && m_phase == phase::RUNNING);
}


void netplay_session::add_note(std::string text, const udp::endpoint *from)
{
	osd_printf_info("Netplay: %s%s\n", text, from ? " (" + from->address().to_string() + ")" : std::string());
	if (m_my_notes.size() < MAX_NOTES)
		m_my_notes.push_back({ m_phase == phase::RUNNING ? m_cur : 0, std::move(text) });
}

void netplay_session::handle_note(int sender, packet_reader &in)
{
	const size_t index = in.get<u16>();
	const s64 frame = in.get<s32>();
	std::string text = in.get_string().substr(0, MAX_NOTE_SIZE);
	if (!in.ok() || index != m_notes[sender].size() || index >= MAX_NOTES)
		return;
	// another player's text, in our log: one line, so it can't pass for lines of ours
	for (char &c : text)
		if (u8(c) < 0x20 || c == 0x7f)
			c = ' ';
	m_notes[sender].push_back({ frame, std::move(text) });
}

void netplay_session::write_notes()
{
	m_notes_ms = now_ms();
	if (!*m_machine.options().netplay_logs() || (m_room.empty() && m_phase != phase::ENDED))
		return;
	if (m_notes_path.empty())
	{
		char date[32];
		const std::time_t t = std::time(nullptr);
		std::strftime(date, sizeof(date), "%Y%m%d-%H%M%S", std::localtime(&t));
		m_notes_path = util::string_format("%s-%s-%s.txt", date, m_machine.system().name, m_room.empty() ? "noroom" : m_room);
		// with the new one, the newest LOGS_KEPT
		const std::string dir = m_machine.options().netplay_logs();
		const std::vector<std::string> logs = room_logs(dir);
		for (size_t i = 0; i + LOGS_KEPT <= logs.size(); i++)
			osd_file::remove(dir + PATH_SEPARATOR + logs[i]);
	}
	emu_file file(m_machine.options().netplay_logs(), OPEN_FLAG_WRITE | OPEN_FLAG_CREATE | OPEN_FLAG_CREATE_PATHS);
	if (file.open(m_notes_path))
		return;
	std::string text = util::string_format("Coinmates %s, %s (%s), room %s\nThe frame numbers are the same for every player.\n",
			emulator_info::get_build_version(), m_machine.system().type.fullname(), m_machine.system().name, m_room.empty() ? "-" : room_name(m_room));
	const auto lines = [&text] (const std::vector<note_line> &notes)
	{
		for (const note_line &n : notes)
			text += util::string_format("  frame %d: %s\n", int(n.frame), n.text);
	};
	text += util::string_format("\nPlayer %d, this computer:\n", m_me + 1);
	lines(m_my_notes);
	for (int p = 0; p < m_players; p++)
	{
		if (p != m_me && !m_notes[p].empty())
		{
			text += util::string_format("\nPlayer %d:\n", p + 1);
			lines(m_notes[p]);
		}
	}
	for (const auto &[player, notes] : m_old_notes)
	{
		text += util::string_format("\nPlayer %d, before another took the place:\n", player + 1);
		lines(notes);
	}
	file.write(text.data(), text.size());
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
			"\"hash_compared\":%llu,\"desyncs\":%llu,\"resyncs\":%llu,\"stall_ms\":%llu,\"slow_frames\":%llu,\"delay_changes\":%d,\"tx_bytes\":%llu,\"rx_bytes\":%llu,\"send_failed\":%u,\"silence_ms_max\":%u,\"welcome_ms\":%d,\"depth\":[",
			m_machine.system().name, m_me + 1, m_players, (long long)m_cur, seconds, m_end_reason.c_str(), rtt,
			(unsigned long long)m_rollbacks, (unsigned long long)m_resim_frames, m_rollback_ms_max, m_rollbacks ? m_rollback_ms_total / m_rollbacks : 0.0,
			(unsigned long long)m_unrecoverable, (unsigned long long)m_saves_failed,
			(unsigned long long)m_hash_compared, (unsigned long long)m_desyncs, (unsigned long long)m_resyncs,
			(unsigned long long)(m_stall_ticks * 1000 / osd_ticks_per_second()), (unsigned long long)m_slow_frames, m_delay_changes, (unsigned long long)m_tx_bytes, (unsigned long long)m_rx_bytes, m_send_failed, m_silence_max_ms, m_welcome_ms);
	for (size_t i = 0; i < std::size(m_depth_histogram); i++)
		std::fprintf(f, "%s%llu", i ? "," : "", (unsigned long long)m_depth_histogram[i]);
	std::fprintf(f, "],\"delay\":[");
	for (size_t i = 0; i < std::size(m_delay_histogram); i++)
		std::fprintf(f, "%s%llu", i ? "," : "", (unsigned long long)m_delay_histogram[i]);
	std::fprintf(f, "]}\n");
	std::fclose(f);
}

} // anonymous namespace


std::unique_ptr<rollback_session> create_netplay_session(running_machine &machine, bool host)
{
	return std::make_unique<netplay_session>(machine, host);
}


// what was typed to join: a room code (any case, with or without the hyphen), or an address,
// with the host's port if missing; empty if neither
std::string rollback_session::join_target(std::string_view typed)
{
	const std::string text = clean_address(std::string(typed));
	if (text.find_first_of(".:") != std::string::npos)
		return text.find(':') != std::string::npos ? text : text + ':' + std::to_string(LAN_PORT);
	const std::string room = clean_room(text);
	return room.size() == ROOM_LENGTH ? room : std::string();
}


//-------------------------------------------------
//  report_problem: a Google Form, as it needs no
//  account, filled in with the newest room log
//-------------------------------------------------

void rollback_session::report_problem(const emu_options &options, const game_driver &driver)
{
	const std::string dir = options.netplay_logs();
	const std::string middle = util::string_format("-%s-", driver.name);
	std::string newest;
	for (const std::string &name : room_logs(dir))
		if (std::string_view(name).substr(LOG_DATE_SIZE).starts_with(middle))
			newest = name;
	std::string log;
	std::vector<u8> data;
	if (!newest.empty() && !util::core_file::load(dir + PATH_SEPARATOR + newest, data))
		log.assign(data.begin(), data.end());

#if defined(_WIN32)
	const char *const system = "Windows";
#elif defined(__APPLE__)
	const char *const system = "macOS";
#else
	const char *const system = std::getenv("SteamDeck") ? "Steam Deck" : "Linux"; // set by Steam on the Deck
#endif
	// the form's prefilled link, and its fields by their numbers there
	std::string url = "https://docs.google.com/forms/d/e/1FAIpQLSe4jwBgvunTNEIuVGZNSf0-RxrT-rzQ240Gx8aaFRXhoDXgtQ/viewform?usp=pp_url";
	constexpr char GAME[] = "1250517475", VERSION[] = "1725171489", SYSTEM[] = "1126036203", LOG[] = "857459388";
	const auto encoded = [] (std::string_view text)
	{
		std::string out;
		for (const char c : text)
		{
			// what the query of a link holds as it is, and spaces as '+': a third of the length
			if (c && (std::isalnum(u8(c)) || std::strchr("-._~:/,()@!*'", c)))
				out += c;
			else
				out += (c == ' ') ? std::string("+") : util::string_format("%%%02X", u8(c));
		}
		return out;
	};
	const auto add = [&] (std::string_view field, std::string_view text) { url += util::string_format("&entry.%s=%s", field, encoded(text)); };
	add(GAME, util::string_format("%s (%s)", driver.type.fullname(), driver.name));
	add(VERSION, emulator_info::get_build_version());
	add(SYSTEM, system);

	// Google refuses links over ~8190 characters: the same number of last lines, where the problem
	// is, of each player; not the header, which the other fields say, nor "frame" on every line
	constexpr size_t MAX_URL = 7500;
	std::vector<std::vector<std::string_view>> parts(1); // the header, then each player's, title first
	for (size_t pos = 0; pos < log.size(); )
	{
		const size_t end = std::min(log.find('\n', pos), log.size());
		const std::string_view line = std::string_view(log).substr(pos, end - pos);
		if (line.empty())
			parts.emplace_back();
		else
			parts.back().push_back(line);
		pos = end + 1;
	}
	const auto shown = [&] (size_t keep)
	{
		std::string text;
		for (size_t p = (parts.size() > 1) ? 1 : 0; p < parts.size(); p++)
		{
			const std::vector<std::string_view> &part = parts[p];
			const size_t from = (part.size() > keep + 1) ? part.size() - keep : 1;
			text += text.empty() ? "" : "\n";
			for (size_t i = 0; i < part.size(); i++)
			{
				if (i == from && from > 1)
					text += util::string_format("[%d earlier lines]\n", int(from - 1));
				if (!i || i >= from)
				{
					std::string_view line = part[i];
					if (line.starts_with("  frame "))
						line.remove_prefix(8);
					text += line;
					text += '\n';
				}
			}
		}
		return encoded(text);
	};
	size_t keep = 0;
	for (size_t step = std::bit_ceil(MAX_NOTES); step; step /= 2)
		if (url.size() + shown(keep + step).size() <= MAX_URL)
			keep += step;
	url += util::string_format("&entry.%s=%s", LOG, log.empty() ? encoded("No room log of this game.\n") : shown(keep));

	osd_printf_info("Report a problem: %s\n", url);
#if defined(_WIN32)
	ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
#if defined(__APPLE__)
	char opener[] = "open";
#else
	char opener[] = "xdg-open";
#endif
	char *argv[] = { opener, url.data(), nullptr };
	pid_t pid;
	if (!posix_spawnp(&pid, opener, nullptr, nullptr, argv, environ))
		std::thread([pid] { waitpid(pid, nullptr, 0); }).detach();
#endif
}
