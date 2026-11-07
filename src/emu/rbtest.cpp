// license:BSD-3-Clause
/***************************************************************************

    rbtest.cpp

    Rollback sync test, enabled by the MAME_RBTEST environment variable, e.g.
      MAME_RBTEST="check=8,frames=3600,players=2,out=/out/kod"

    A bot plays. Every frame the state of `check` frames ago is loaded and those
    frames run again with the same inputs: a state that differs from the first
    pass depends on something the save state does not hold. Options:
      seed=<n>          another bot
      out=<path>        <path>.json with the results, <path>.layout.tsv with the
                        save state items, <path>.synctest.txt with the mismatches
      exclude=<a;b>     items computed while drawing, left out of the hash
      draw=1            draw while resimulating: state computed while drawing then stops differing
      dump_after=<n>    write the first mismatch from frame n on as <path>.stA/stB.<frame>.state
      tapframe=<n>      log the accesses of a CPU during frame n, per pass, to <path>.tap<pass>.txt
      tapcpu=<tag>      that CPU, by default maincpu

***************************************************************************/

#include "emu.h"
#include "rollback.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>


namespace {

using clk = std::chrono::steady_clock;
double us_since(clk::time_point t) { return std::chrono::duration<double, std::micro>(clk::now() - t).count(); }

struct stats
{
	std::vector<float> v;

	void add(double x) { v.push_back(float(x)); }

	std::string json(const char *name)
	{
		std::sort(v.begin(), v.end());
		auto const pct = [this] (double q) { return v.empty() ? 0.0 : double(v[std::min(v.size() - 1, size_t(q * (v.size() - 1) + 0.5))]); };
		return util::string_format("\"%s\":{\"n\":%u,\"p50\":%.2f,\"p90\":%.2f,\"p99\":%.2f,\"max\":%.2f}",
				name, unsigned(v.size()), pct(0.5), pct(0.9), pct(0.99), pct(1.0));
	}
};


class sync_test : public rollback_session
{
public:
	sync_test(running_machine &machine, const char *config);
	~sync_test() { finish(); }

	void frame_inputs_begin() override { if (!m_done) m_frame_ended = true; }
	void frame_inputs_port(ioport_port &port) override;
	void post_timeslice() override;

private:
	u32 frame_word(int player, s64 frame) const override { return bot_word(m_seed, player, frame); }
	void resim_frame_done() override;

	bool timed_save(s64 frame);
	void check_frame(s64 boundary);
	void dump_state(const std::vector<u8> &state, const char *pass, s64 frame) const;
	void finish();

	template <typename T> void install_tap(address_space &space);
	void tap_log(char rw, offs_t offset, u32 data, u32 mask);
	void tap_frame_start();

	int m_check = 4;
	u32 m_seed = 1;
	s64 m_frames = 3600;
	s64 m_dump_after = 0;
	std::string m_out;
	bool m_done = false;

	std::vector<std::pair<s64, u64>> m_first_hash;  // per slot: the frame and the hash of its first pass
	std::vector<u8> m_first_pass;                   // the state a resimulated frame replaced
	s64 m_reload = 0;
	s64 m_boundary = 0;
	u64 m_checked = 0, m_mismatches = 0;
	bool m_dumped = false;
	FILE *m_log = nullptr;

	s64 m_tapframe = -1;
	cpu_device *m_tapcpu = nullptr;
	int m_tappass = 0;
	FILE *m_tapfile = nullptr;
	std::vector<memory_passthrough_handler> m_taps;

	stats m_save_us, m_load_us, m_resim_frame_us, m_frame_us, m_rollback_us;
	clk::time_point m_frame_start, m_run_start;
};


sync_test::sync_test(running_machine &machine, const char *config)
	: rollback_session(machine)
{
	std::string exclude, tapcpu = "maincpu";
	std::istringstream options(config);
	for (std::string kv; std::getline(options, kv, ','); )
	{
		const auto eq = kv.find('=');
		if (eq == std::string::npos)
			continue;
		const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
		if (k == "check") m_check = std::max(1, std::stoi(v));
		else if (k == "draw") m_resim_draw = std::stoi(v) != 0;
		else if (k == "tapframe") m_tapframe = std::stoll(v);
		else if (k == "tapcpu") tapcpu = v;
		else if (k == "players") m_players = std::clamp(std::stoi(v), 1, MAX_PLAYERS);
		else if (k == "seed") m_seed = u32(std::stoul(v));
		else if (k == "frames") m_frames = std::stoll(v);
		else if (k == "out") m_out = v;
		else if (k == "dump_after") m_dump_after = std::stoll(v);
		else if (k == "exclude") exclude = v;
	}

	allocate_slots(m_check + 3);
	m_first_hash.assign(m_check + 3, { -1, 0 });
	m_first_pass.resize(m_state_size);
	compute_hash_skip(exclude, false);

	if (!m_out.empty())
	{
		if (FILE *const layout = std::fopen((m_out + ".layout.tsv").c_str(), "w"))
		{
			for_each_item([layout] (const char *name, size_t offset, size_t size) { std::fprintf(layout, "%zu\t%zu\t%s\n", offset, size, name); });
			std::fclose(layout);
		}
		m_log = std::fopen((m_out + ".synctest.txt").c_str(), "w");
	}

	if (m_tapframe >= 0)
	{
		m_tapcpu = machine.root_device().subdevice<cpu_device>(tapcpu.c_str());
		if (!m_tapcpu)
			throw emu_fatalerror("Sync test: no CPU %s", tapcpu);
		address_space &space = m_tapcpu->space(AS_PROGRAM);
		switch (space.data_width())
		{
		case 8: install_tap<u8>(space); break;
		case 16: install_tap<u16>(space); break;
		case 32: install_tap<u32>(space); break;
		case 64: install_tap<u64>(space); break;
		}
	}

	osd_printf_info("Sync test: check=%d players=%d frames=%d state=%u bytes\n", m_check, m_players, int(m_frames), unsigned(m_state_size));
	m_run_start = m_frame_start = clk::now();
}


void sync_test::frame_inputs_port(ioport_port &port)
{
	if (!m_done)
		port.live().digital = compose(m_port_index[&port], m_cur + 1);
}

bool sync_test::timed_save(s64 frame)
{
	const auto start = clk::now();
	const bool saved = save_slot(frame);
	if (saved)
		m_save_us.add(us_since(start));
	return saved;
}

void sync_test::dump_state(const std::vector<u8> &state, const char *pass, s64 frame) const
{
	if (FILE *const f = std::fopen(util::string_format("%s.st%s.%d.state", m_out, pass, int(frame)).c_str(), "wb"))
	{
		std::fwrite(state.data(), 1, state.size(), f);
		std::fclose(f);
	}
}

void sync_test::post_timeslice()
{
	if (!m_frame_ended || m_done)
		return;
	m_frame_ended = false;
	m_frame_us.add(us_since(m_frame_start));

	const s64 b = ++m_cur;
	if (timed_save(b))
		check_frame(b);

	if (b >= m_frames)
	{
		finish();
		m_machine.schedule_exit();
	}
	tap_frame_start();
	m_frame_start = clk::now();
}

void sync_test::check_frame(s64 boundary)
{
	m_first_hash[boundary % m_first_hash.size()] = { boundary, state_hash(slot_for(boundary).data) };
	m_boundary = boundary;
	m_reload = boundary - m_check;
	if (m_reload < 1 || !slot_valid(m_reload))
		return;

	const auto start = clk::now();
	load_state(slot_for(m_reload).data);
	apply_frame_to_ports(m_reload);
	m_load_us.add(us_since(start));
	m_cur = m_reload;
	tap_frame_start();
	m_frame_start = clk::now();
	run_frames(m_reload, boundary);
	m_rollback_us.add(us_since(start));
}

void sync_test::resim_frame_done()
{
	const s64 frame = m_cur;
	slot &s = slot_for(frame);
	std::swap(m_first_pass, s.data);
	const bool saved = timed_save(frame);
	m_resim_frame_us.add(us_since(m_frame_start));

	auto &first = m_first_hash[frame % m_first_hash.size()];
	if (saved && first.first == frame)
	{
		m_checked++;
		const u64 hash = state_hash(s.data);
		if (hash != first.second)
		{
			m_mismatches++;
			if (m_log)
				std::fprintf(m_log, "frame %d differs after reload of %d (boundary %d)\n", int(frame), int(m_reload), int(m_boundary));
			if (!m_dumped && frame >= m_dump_after && !m_out.empty())
			{
				m_dumped = true;
				dump_state(m_first_pass, "A", frame);
				dump_state(s.data, "B", frame);
			}
			first.second = hash;
		}
	}
	if (frame < m_boundary)
		tap_frame_start();
	m_frame_start = clk::now();
}

template <typename T> void sync_test::install_tap(address_space &space)
{
	m_taps.push_back(space.install_readwrite_tap(0, space.addrmask(), "rbtest",
			[this] (offs_t offset, T &data, T mask) { tap_log('r', offset, u32(data), u32(mask)); },
			[this] (offs_t offset, T &data, T mask) { tap_log('w', offset, u32(data), u32(mask)); }));
}

void sync_test::tap_log(char rw, offs_t offset, u32 data, u32 mask)
{
	if (m_tapfile)
		std::fprintf(m_tapfile, "%s %c %06x %04x %04x pc=%06x\n", m_machine.time().as_string(9), rw, offset, data, mask, unsigned(m_tapcpu->pc()));
}

void sync_test::tap_frame_start()
{
	if (m_tapfile)
	{
		std::fclose(m_tapfile);
		m_tapfile = nullptr;
	}
	if (m_cur == m_tapframe && !m_out.empty())
		m_tapfile = std::fopen(util::string_format("%s.tap%d.txt", m_out, m_tappass++).c_str(), "w");
}

void sync_test::finish()
{
	if (m_done)
		return;
	m_done = true;
	if (m_log)
		std::fclose(m_log);
	if (m_out.empty())
		return;
	FILE *const f = std::fopen((m_out + ".json").c_str(), "w");
	if (!f)
		return;
	std::fprintf(f, "{\"system\":\"%s\",\"check\":%d,\"players\":%d,\"seed\":%u,\"frames\":%d,\"state_size\":%u,\"wall_s\":%.2f,"
			"\"synctest_checked\":%llu,\"synctest_mismatch\":%llu,\"saves_failed\":%llu,\n",
			m_machine.system().name, m_check, m_players, m_seed, int(m_cur), unsigned(m_state_size), us_since(m_run_start) / 1e6,
			(unsigned long long)m_checked, (unsigned long long)m_mismatches, (unsigned long long)m_saves_failed);
	std::fprintf(f, "%s,\n%s,\n%s,\n%s,\n%s}\n",
			m_save_us.json("save_us").c_str(), m_load_us.json("load_us").c_str(), m_resim_frame_us.json("resim_frame_us").c_str(),
			m_frame_us.json("frame_us").c_str(), m_rollback_us.json("rollback_us").c_str());
	std::fclose(f);
}

} // anonymous namespace


std::unique_ptr<rollback_session> create_sync_test(running_machine &machine, const char *config)
{
	return std::make_unique<sync_test>(machine, config);
}
