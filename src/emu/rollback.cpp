// license:BSD-3-Clause
/***************************************************************************

    rollback.cpp

    What a rollback session needs from the machine, see rollback.h.

***************************************************************************/

#include "emu.h"
#include "rollback.h"

#include "emuopts.h"
#include "sound.h"
#include "video.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <string>


namespace {

constexpr const char *SYNC_TEST_VARIABLE = "MAME_RBTEST";

inline u64 mix64(u64 x)
{
	x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
	x ^= x >> 27; x *= 0x94d049bb133111ebULL;
	x ^= x >> 31;
	return x;
}

} // anonymous namespace


bool rollback_session::requested(const emu_options &options)
{
	return options.netplay_host_port() || *options.netplay_join() || std::getenv(SYNC_TEST_VARIABLE);
}

std::unique_ptr<rollback_session> rollback_session::create(running_machine &machine)
{
	const emu_options &options = machine.options();
	const bool host = options.netplay_host_port() != 0;
	if (host && *options.netplay_join())
		throw emu_fatalerror("Netplay: -netplay_host and -netplay_join exclude each other");
	if (host || *options.netplay_join())
		return create_netplay_session(machine, host);
	if (const char *const config = std::getenv(SYNC_TEST_VARIABLE))
		return create_sync_test(machine, config);
	return nullptr;
}

rollback_session::rollback_session(running_machine &machine)
	: m_machine(machine)
	, m_state_size(ram_state::get_size(machine.save()))
{
	map_inputs();
}


//-------------------------------------------------
//  inputs
//-------------------------------------------------

// every player has the same inputs, one bit per input type
void rollback_session::map_inputs()
{
	for (auto &p : m_machine.ioport().ports())
	{
		m_port_index[p.second.get()] = int(m_ports.size());
		m_ports.push_back(p.second.get());
		std::vector<port_field> &fields = m_port_fields.emplace_back();
		for (ioport_field &field : p.second->fields())
		{
			const ioport_type t = field.type();
			ioport_type type = t;
			int player = field.player();
			if (t >= IPT_COIN1 && t <= IPT_COIN12) { type = IPT_COIN1; player = t - IPT_COIN1; }
			else if (t >= IPT_START1 && t <= IPT_START10) { type = IPT_START1; player = t - IPT_START1; }
			else if (!field.is_digital_joystick() && !(t >= IPT_BUTTON1 && t <= IPT_BUTTON16))
				continue; // service, tilt...: nobody's, and always off in a session
			if (player >= MAX_PLAYERS)
				continue;

			auto it = std::find(m_bit_type.begin(), m_bit_type.end(), type);
			if (it == m_bit_type.end())
			{
				if (m_bit_type.size() == 32)
					throw emu_fatalerror("Rollback: too many different inputs");
				it = m_bit_type.insert(m_bit_type.end(), type);
			}
			fields.push_back({ u8(it - m_bit_type.begin()), u8(player), field.mask() });
		}
	}
}

ioport_value rollback_session::compose(int port, s64 frame) const
{
	ioport_value value = 0;
	for (const port_field &f : m_port_fields[port])
		if (f.player < m_players && BIT(frame_word(f.player, frame), f.bit))
			value |= f.mask;
	return value;
}

// the OS sample of a port as a word: the controls of player 1
u32 rollback_session::local_word(int port, ioport_value digital) const
{
	u32 word = 0;
	for (const port_field &f : m_port_fields[port])
		if (f.player == 0 && (digital & f.mask))
			word |= u32(1) << f.bit;
	return word;
}

// a load leaves the live port values of the present: put back those of the
// loaded frame. Custom port handlers may have side effects (s1945's
// mcu_status_r toggles on every read), and this read is not the machine's
void rollback_session::apply_frame_to_ports(s64 frame)
{
	auto dis = m_machine.disable_side_effects();
	for (size_t i = 0; i < m_ports.size(); i++)
	{
		ioport_port &port = *m_ports[i];
		port.live().digital = compose(int(i), frame);
		const ioport_value value = port.read();
		for (auto &dyn : port.live().writelist)
			if (dyn.field().type() != IPT_OUTPUT)
				dyn.write(value);
	}
}

// a deterministic player: a coin and start every 30 seconds, held directions, mashed buttons
u32 rollback_session::bot_word(u32 seed, int player, s64 frame) const
{
	static const u8 directions[9] = { 0, 1, 9, 8, 10, 2, 6, 4, 5 }; // bits: up, down, left, right
	auto const pick = [seed, player, frame] (int what, s64 period)
	{
		return mix64(mix64(u64(seed) * 0x9e3779b97f4a7c15ULL + player * 131 + what) ^ u64(frame / period));
	};

	const s64 phase = frame % 1800;
	u32 word = 0;
	for (size_t bit = 0; bit < m_bit_type.size(); bit++)
	{
		const ioport_type t = m_bit_type[bit];
		bool on;
		if (t == IPT_COIN1)
			on = phase >= 120 + 20 * player && phase < 126 + 20 * player;
		else if (t == IPT_START1)
			on = phase >= 300 + 20 * player && phase < 306 + 20 * player;
		else if (t >= IPT_BUTTON1)
		{
			const int button = t - IPT_BUTTON1;
			on = button == 0 ? pick(100, 3) % 2 == 0 : button == 1 ? pick(101, 4) % 10 == 0 : pick(100 + button, 8) % 30 == 0;
		}
		else
			on = BIT(directions[pick((t - IPT_JOYSTICK_UP) / 4, 10) % 9], (t - IPT_JOYSTICK_UP) % 4);
		if (on)
			word |= u32(1) << bit;
	}
	return word;
}


//-------------------------------------------------
//  states
//-------------------------------------------------

void rollback_session::allocate_slots(size_t count)
{
	m_slots.resize(count);
	for (slot &s : m_slots)
		s.data.resize(m_state_size);
}

bool rollback_session::save_slot(s64 frame)
{
	slot &s = slot_for(frame);
	s.frame = frame;
	s.valid = false;
	if (!m_machine.scheduler().can_save())
	{
		m_saves_failed++;
		return false;
	}
	s.valid = m_machine.save().write_buffer(s.data.data(), s.data.size()) == STATERR_NONE;
	return s.valid;
}

// items the hash ignores: state derived from drawing, which depends on whether
// a frame was drawn or resimulated (plus the ;-separated names in exclude),
// frontend state, and optionally the sound samples
void rollback_session::compute_hash_skip(std::string_view exclude, bool skip_samples)
{
	std::vector<std::string> names;
	std::istringstream list{ std::string(exclude) };
	for (std::string name; std::getline(list, name, ';'); )
		if (!name.empty())
			names.push_back(name);

	m_hash_skip.clear();
	for_each_item([&] (std::string_view name, size_t offset, size_t size)
	{
		bool skip = name.substr(0, 8) == "tilemap/" || name.substr(0, 13) == "Video Screen/" || name.find("lua_engine") != std::string_view::npos;
		skip = skip || (skip_samples && name.substr(0, 7) == "stream.");
		for (const std::string &x : names)
			skip = skip || name.find(x) != std::string_view::npos;
		if (skip)
			m_hash_skip.emplace_back(offset, size);
	});
}

u64 rollback_session::state_hash(const std::vector<u8> &state) const
{
	u64 h = 0x84222325cbf29ce4ULL;
	size_t pos = 0;
	auto const hash_to = [&] (size_t end)
	{
		for (; pos + 8 <= end; pos += 8)
		{
			u64 word;
			memcpy(&word, &state[pos], 8);
			h ^= word * 0x9e3779b97f4a7c15ULL;
			h = ((h << 31) | (h >> 33)) * 0xc2b2ae3d27d4eb4fULL;
		}
		for (; pos < end; pos++)
			h = (h ^ state[pos]) * 0x100000001b3ULL;
	};
	for (const auto &skip : m_hash_skip)
	{
		hash_to(skip.first);
		pos = skip.first + skip.second;
	}
	hash_to(state.size());
	return mix64(h);
}


//-------------------------------------------------
//  resimulation
//-------------------------------------------------

void rollback_session::run_frames(s64 from, s64 to)
{
	m_resim = true;
	m_machine.sound().set_discard_output(true);
	for (m_cur = from; m_cur < to && !m_machine.exit_pending(); )
	{
		m_machine.video().set_resimulating(true, resim_draws(m_cur + 1));
		m_frame_ended = false;
		while (!m_frame_ended && !m_machine.exit_pending() && !m_machine.hard_reset_pending())
			m_machine.scheduler().timeslice();
		m_cur++;
		resim_frame_done();
	}
	m_frame_ended = false;
	m_machine.sound().set_discard_output(false);
	m_machine.video().set_resimulating(false);
	m_resim = false;
}
