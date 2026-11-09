// license:BSD-3-Clause
/***************************************************************************

    rollback.h

    What a rollback session needs from the machine: a ring of save states,
    a hash of them, inputs that come from the session instead of the OS, and
    running frames again without video or sound. Two sessions use it: netplay
    (-netplay_host, -netplay_join) and the sync test (MAME_RBTEST).

    Frame numbering: frame b starts at boundary b, which is where its inputs
    are applied and its state is saved. m_cur is the frame being emulated.

***************************************************************************/

#ifndef MAME_EMU_ROLLBACK_H
#define MAME_EMU_ROLLBACK_H

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>


class rollback_session
{
public:
	// whether the options and the environment ask for a session: machine
	// features that depend on history must then be off (see allow_drc)
	static bool requested(const emu_options &options);
	static std::unique_ptr<rollback_session> create(running_machine &machine);

	// the players a system has controls for (at most MAX_PLAYERS count in a session)
	static int system_players(const ioport_list &ports);
	static constexpr int MAX_PLAYERS = 4;

	// why the last session ended, for the system list to show once; empty if it wasn't told
	static std::string take_end_message() { return std::exchange(s_end_message, std::string()); }

	// frames of input delay of a system, unless the host chooses: more for the heaviest
	static int default_delay(const game_driver &system);
	static constexpr int MIN_DELAY = 1, MAX_DELAY = 4;   // what the host can choose in the menu

	// what was typed to join, as -netplay_join takes it: a room code, or host:port; empty if neither
	static std::string join_target(std::string_view typed);

	// opens the problem form in the browser, filled in with this system's newest room log
	static void report_problem(const emu_options &options, const game_driver &driver);

	virtual ~rollback_session() = default;

	// true while others share this timeline: pausing, saving and resetting are off
	virtual bool in_session() const { return false; }

	// for the menu, e.g. the room code to give to whoever joins again; empty if nothing to say
	virtual std::string status() const { return std::string(); }

	virtual void frame_inputs_begin() = 0;                 // ioport frame_update start (end of the current frame)
	virtual void frame_inputs_port(ioport_port &port) = 0; // per port, after the OS sample
	virtual void post_timeslice() = 0;                     // main loop, after every timeslice

protected:
	static inline std::string s_end_message;

	struct slot
	{
		s64 frame = -1;
		bool valid = false;
		std::vector<u8> data;
	};

	rollback_session(running_machine &machine);

	// inputs: one bit per input type in each player's word for a frame
	virtual u32 frame_word(int player, s64 frame) const = 0;
	virtual void apply_frame_to_ports(s64 frame);
	ioport_value compose(int port, s64 frame) const;
	u32 local_word(int port, ioport_value digital) const;
	u32 bot_word(u32 seed, int player, s64 frame) const;

	void allocate_slots(size_t count);
	size_t slot_count() const { return m_slots.size(); }
	slot &slot_for(s64 frame) { return m_slots[frame % m_slots.size()]; }
	bool slot_valid(s64 frame) { const slot &s = slot_for(frame); return s.frame == frame && s.valid; }
	bool save_slot(s64 frame);
	void load_state(const std::vector<u8> &state) { m_machine.save().read_buffer(state.data(), state.size()); }
	void compute_hash_skip(std::string_view exclude, bool skip_samples);
	u64 state_hash(const std::vector<u8> &state) const;

	// calls f(name, offset, size) for every item of a save state
	template <typename F> void for_each_item(F &&f) const
	{
		size_t offset = 32; // HEADER_SIZE in save.cpp
		void *base;
		u32 valsize, valcount, blockcount, stride;
		for (int i = 0; ; i++)
		{
			const char *const name = m_machine.save().indexed_item(i, base, valsize, valcount, blockcount, stride);
			if (!name)
				break;
			const size_t size = size_t(valsize) * valcount * blockcount;
			f(name, offset, size);
			offset += size;
		}
	}

	// run the frames from..to-1 again, in one host frame: no video, sound or
	// throttle; resim_frame_done() is called after each of them
	void run_frames(s64 from, s64 to);
	virtual void resim_frame_done() = 0;
	// the screens of this frame are drawn (not shown) while resimulating: the
	// state computed while drawing is then the same as in a frame played normally
	virtual bool resim_draws(s64 frame) const { return m_resim_draw; }

	running_machine &m_machine;
	int m_players = 2;
	s64 m_cur = 0;
	bool m_frame_ended = false;
	bool m_resim = false;
	bool m_resim_draw = false;      // draw every frame while resimulating
	size_t m_state_size = 0;
	std::vector<ioport_port *> m_ports;
	std::unordered_map<ioport_port *, int> m_port_index;
	std::vector<ioport_type> m_bit_type;
	std::vector<std::pair<size_t, size_t>> m_hash_skip; // (offset, size) of items left out of the hash, in order
	u64 m_saves_failed = 0;

private:
	// one input of one player in one port
	struct port_field
	{
		u8 bit;
		u8 player;
		ioport_value mask;
	};

	void map_inputs();

	std::vector<std::vector<port_field>> m_port_fields;
	std::vector<slot> m_slots;
};


std::unique_ptr<rollback_session> create_netplay_session(running_machine &machine, bool host);
std::unique_ptr<rollback_session> create_sync_test(running_machine &machine, const char *config);

#endif // MAME_EMU_ROLLBACK_H
