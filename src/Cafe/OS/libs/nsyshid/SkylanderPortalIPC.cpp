/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "SkylanderPortalIPC.h"
#include "Dimensions.h"
#include "Infinity.h"
#include "Skylander.h"

#include "config/CemuConfig.h"
#include "Common/FileStream.h"
#include "Common/socket.h"
#include "util/helpers/helpers.h"

#include <charconv>

#if !BOOST_OS_WINDOWS
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>
#endif

// The protocol is the one RPCS3's Skylanders portal server speaks (rpcs3/Emu/Io/SkylanderPortalIPC.cpp,
// added in RPCS3 commit dfc486f), so that a launcher drives both emulators the same way:
// one line per connection, one reply, then the connection is closed. "hello", "create" and "hush" are Cemu's own,
// and so are the Infinity Base's commands and the Dimensions Toypad's, which all start with "infinity" or "toypad"
// so that none can be taken for the portal's.

namespace nsyshid::SkylanderPortalIPC
{
	constexpr size_t MAX_COMMAND_SIZE = 4096;
	constexpr sint64 COMMAND_TIMEOUT_MS = 1000;
	constexpr sint32 POLL_INTERVAL_MS = 100;

#ifdef MSG_NOSIGNAL
	// a client that has gone away must not end Cemu with SIGPIPE
	constexpr sint32 SEND_FLAGS = MSG_NOSIGNAL;
#else
	constexpr sint32 SEND_FLAGS = 0;
#endif

	static std::atomic_bool s_inputHushed = false;
	// the emulated devices the running title started with, as Start() read them from the settings
	static std::atomic_bool s_portalEmulated = false;
	static std::atomic_bool s_baseEmulated = false;
	static std::atomic_bool s_toypadEmulated = false;

	static bool WasInterrupted()
	{
#if BOOST_OS_WINDOWS
		return WSAGetLastError() == WSAEINTR;
#else
		return errno == EINTR;
#endif
	}

	static bool SetBlocking(SOCKET s, bool blocking)
	{
#if BOOST_OS_WINDOWS
		u_long nonBlocking = blocking ? 0 : 1;
		return ioctlsocket(s, FIONBIO, &nonBlocking) == 0;
#else
		const int flags = fcntl(s, F_GETFL, 0);
		if (flags == -1)
			return false;
		return fcntl(s, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK)) == 0;
#endif
	}

	// greater than zero when the socket can be read, zero on timeout and below zero on error
	static sint32 WaitForData(SOCKET s, sint32 timeoutMs)
	{
		pollfd pfd{};
		pfd.fd = s;
		pfd.events = POLLIN;
#if BOOST_OS_WINDOWS
		return WSAPoll(&pfd, 1, timeoutMs);
#else
		return poll(&pfd, 1, timeoutMs);
#endif
	}

	static bool IsAbsoluteLocalPath(std::string_view path)
	{
#if BOOST_OS_WINDOWS
		// requiring a drive letter also rules out network (\\server\share) and device (\\.\) paths
		const bool hasDriveLetter = path.size() >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':';
		return hasDriveLetter && (path[2] == '\\' || path[2] == '/');
#else
		return path.starts_with('/');
#endif
	}

	static bool ParseNumber(std::string_view text, sint64& value)
	{
		const char* end = text.data() + text.size();
		const auto result = std::from_chars(text.data(), end, value);
		return result.ec == std::errc() && result.ptr == end;
	}

	static std::string HandleStatus()
	{
		std::string reply;
		for (uint32 slot = 0; slot < MAX_SKYLANDERS; slot++)
		{
			uint16 skyId, skyVar;
			if (g_skyportal.GetLoadedSkylander((uint8)slot, skyId, skyVar))
				reply += fmt::format("slot{} loaded {} {}\n", slot, skyId, skyVar);
			else
				reply += fmt::format("slot{} empty\n", slot);
		}
		reply += "ok\n";
		return reply;
	}

	static std::string HandleLoad(sint64 slot, std::string_view path)
	{
		if (slot < -1 || slot >= MAX_SKYLANDERS)
			return "error invalid slot\n";

		if (!IsAbsoluteLocalPath(path))
			return "error path must be absolute\n";

		// checked before the file is opened, so that a device or a pipe is never opened
		const fs::path figurePath = _utf8ToPath(path);
		std::error_code ec;
		if (!fs::is_regular_file(figurePath, ec) || fs::file_size(figurePath, ec) != SKY_FIGURE_SIZE)
			return "error not a skylander file\n";

		// opened for writing as the Emulated USB Devices window opens it, so the game can save to the figure
		std::unique_ptr<FileStream> figureFile(FileStream::openFile2(figurePath, true));
		if (!figureFile)
			return "error cannot open file\n";

		std::array<uint8, SKY_FIGURE_SIZE> data;
		if (figureFile->readData(data.data(), data.size()) != data.size())
			return "error file too small\n";

		const uint16 storedCrc = uint16(data[0x1F]) << 8 | uint16(data[0x1E]);
		if (storedCrc != g_skyportal.SkylanderCRC16(0xFFFF, data.data(), 0x1E))
			return "error not a skylander file\n";

		const uint8 portalSlot = g_skyportal.LoadSkylander(data.data(), std::move(figureFile), (sint32)slot);
		if (portalSlot == 0xFF)
			return "error no free slot\n";

		return fmt::format("ok {}\n", (uint32)portalSlot);
	}

	static std::string HandleRemove(sint64 slot)
	{
		if (slot < 0 || slot >= MAX_SKYLANDERS)
			return "error invalid slot\n";

		if (!g_skyportal.RemoveSkylander((uint8)slot))
			return "error slot empty\n";

		return "ok\n";
	}

	static std::string HandleClear()
	{
		for (uint8 slot = 0; slot < MAX_SKYLANDERS; slot++)
			g_skyportal.RemoveSkylander(slot);
		return "ok\n";
	}

	static std::string HandleCreate(uint16 skyId, uint16 skyVar, std::string_view path)
	{
		if (!IsAbsoluteLocalPath(path))
			return "error path must be absolute\n";

		// the figure maker in the Emulated USB Devices window asks before it replaces a file, and nobody can be asked here
		const fs::path figurePath = _utf8ToPath(path);
		std::error_code ec;
		if (fs::exists(fs::symlink_status(figurePath, ec)))
			return "error file exists\n";

		if (!g_skyportal.CreateSkylander(figurePath, skyId, skyVar))
			return "error cannot create file\n";

		return "ok\n";
	}

	static std::string HandleHush(std::string_view argument)
	{
		if (argument == "on")
			s_inputHushed = true;
		else if (argument == "off")
			s_inputHushed = false;
		else
			return "error invalid argument\n";
		return "ok\n";
	}

	struct Words
	{
		std::string_view first;
		std::string_view rest;
		bool hasRest = false;
	};

	// splits a line at its first space, as ProcessCommand does
	static Words SplitFirstWord(std::string_view line)
	{
		const size_t space = line.find(' ');
		if (space == std::string_view::npos)
			return {line, {}, false};
		return {line.substr(0, space), line.substr(space + 1), true};
	}

	// the places are numbered as the Emulated USB Devices window lists them, from 0 for the hexagon's play set or power disc
	// to 8 for Player Two's second ability, which is the order InfinityUSB keeps them in. LoadFigure and RemoveFigure don't
	// check a place themselves
	static bool ParseInfinityPlace(std::string_view text, uint8& place)
	{
		sint64 value;
		if (!ParseNumber(text, value) || value < 0 || value >= MAX_FIGURES)
			return false;
		place = (uint8)value;
		return true;
	}

	static std::string HandleInfinityStatus()
	{
		std::string reply;
		for (uint8 place = 0; place < MAX_FIGURES; place++)
		{
			// the series is the one the figure maker's list gives the number, 0 for a number it doesn't have
			uint32 number;
			if (g_infinitybase.GetLoadedFigure(place, number))
				reply += fmt::format("place{} loaded {} {}\n", (uint32)place, number, (uint32)g_infinitybase.FindFigure(number).first);
			else
				reply += fmt::format("place{} empty\n", (uint32)place);
		}
		reply += "ok\n";
		return reply;
	}

	static std::string HandleInfinityLoad(uint8 place, std::string_view path)
	{
		if (!IsAbsoluteLocalPath(path))
			return "error path must be absolute\n";

		// checked before the file is opened, so that a device or a pipe is never opened
		const fs::path figurePath = _utf8ToPath(path);
		std::error_code ec;
		if (!fs::is_regular_file(figurePath, ec) || fs::file_size(figurePath, ec) != INF_FIGURE_SIZE)
			return "error not an infinity file\n";

		// opened for writing as the Emulated USB Devices window opens it, so the game can save to the figure
		std::unique_ptr<FileStream> figureFile(FileStream::openFile2(figurePath, true));
		if (!figureFile)
			return "error cannot open file\n";

		std::array<uint8, INF_FIGURE_SIZE> data;
		if (figureFile->readData(data.data(), data.size()) != data.size())
			return "error file too small\n";

		if (g_infinitybase.HasFigureWithUid(data, place))
			return "error already on the base\n";

		// LoadFigure would drop what the place holds without saving it or telling the game, so that comes off first, as
		// the window takes it off (EmulatedUSBDeviceFrame::LoadFigurePath), and only once the new file has been read
		g_infinitybase.RemoveFigure(place);
		const uint32 number = g_infinitybase.LoadFigure(data, std::move(figureFile), place);
		return fmt::format("ok {} {}\n", number, (uint32)g_infinitybase.FindFigure(number).first);
	}

	static std::string HandleInfinityRemove(uint8 place)
	{
		if (!g_infinitybase.RemoveFigure(place))
			return "error place empty\n";
		return "ok\n";
	}

	static std::string HandleInfinityClear()
	{
		for (uint8 place = 0; place < MAX_FIGURES; place++)
			g_infinitybase.RemoveFigure(place);
		return "ok\n";
	}

	static std::string HandleInfinityCreate(uint32 number, uint8 series, std::string_view path)
	{
		if (!IsAbsoluteLocalPath(path))
			return "error path must be absolute\n";

		// the figure maker in the Emulated USB Devices window asks before it replaces a file, and nobody can be asked here
		const fs::path figurePath = _utf8ToPath(path);
		std::error_code ec;
		if (fs::exists(fs::symlink_status(figurePath, ec)))
			return "error file exists\n";

		if (!g_infinitybase.CreateFigure(figurePath, number, series))
			return "error cannot create file\n";

		return "ok\n";
	}

	// every figure the figure maker knows, in the order of their numbers. The name comes last, so its spaces need no quoting
	static std::string HandleInfinityList()
	{
		std::string reply;
		for (const auto& [number, figure] : InfinityUSB::GetFigureList())
			reply += fmt::format("figure {} {} {}\n", number, (uint32)figure.first, figure.second);
		reply += "ok\n";
		return reply;
	}

	// These make the calls the Emulated USB Devices window makes from the GUI thread, and ThreadFunc runs one at a time,
	// so they are as safe beside the title's own reads and writes of the base as the window is
	static std::string HandleInfinity(std::string_view line)
	{
		if (!s_baseEmulated)
			return "error base not emulated\n";

		const Words words = SplitFirstWord(line);
		const std::string_view command = words.first;

		if (command == "status")
			return HandleInfinityStatus();

		if (command == "list")
			return HandleInfinityList();

		if (command == "clear")
			return HandleInfinityClear();

		if (command == "remove")
		{
			if (!words.hasRest)
				return "error missing place\n";
			uint8 place;
			if (!ParseInfinityPlace(words.rest, place))
				return "error invalid place\n";
			return HandleInfinityRemove(place);
		}

		if (command == "load")
		{
			if (!words.hasRest)
				return "error missing arguments\n";
			const Words placeAndPath = SplitFirstWord(words.rest);
			if (!placeAndPath.hasRest)
				return "error missing path\n";
			uint8 place;
			if (!ParseInfinityPlace(placeAndPath.first, place))
				return "error invalid place\n";
			return HandleInfinityLoad(place, placeAndPath.rest);
		}

		if (command == "create")
		{
			if (!words.hasRest)
				return "error missing arguments\n";
			const Words numberAndRest = SplitFirstWord(words.rest);
			const Words seriesAndPath = SplitFirstWord(numberAndRest.rest);
			if (!numberAndRest.hasRest || !seriesAndPath.hasRest)
				return "error missing path\n";
			// CreateFigure turns down a number below 0x10000 only after it has made the file, which it leaves empty and open
			sint64 number, series;
			if (!ParseNumber(numberAndRest.first, number) || number < 0x10000 || number > 0xFFFFFF)
				return "error invalid number\n";
			// the series dates the figure, and only 1, 2 and 3 have a date (GenerateBlankFigureData)
			if (!ParseNumber(seriesAndPath.first, series) || series < 1 || series > 3)
				return "error invalid series\n";
			return HandleInfinityCreate((uint32)number, (uint8)series, seriesAndPath.rest);
		}

		return "error unknown command\n";
	}

	// the places are numbered as DimensionsUSB keeps them, 1 on the centre pad, 0, 3 and 4 on the left one and 2, 5 and 6
	// on the right one, as the Emulated USB Devices window lays them out (EmulatedUSBDeviceFrame::AddDimensionsPage) and
	// RPCS3's Figure Mover too (rpcs3qt/dimensions_dialog.cpp, minifig_move_dialog::add_minifig_position, RPCS3 b2e52d6).
	// The game is told a tag's pad as well as its place, and DimensionsUSB takes the pad from its caller unchecked, so the
	// server works it out from the place and never takes one from the client
	static uint8 ToypadPadOf(uint8 place)
	{
		if (place == 1)
			return 1;
		if (place == 0 || place == 3 || place == 4)
			return 2;
		return 3;
	}

	// LoadFigure, RemoveFigure, TempRemove and MoveFigure don't check a place themselves (GetFigureByIndex)
	static bool ParseToypadPlace(std::string_view text, uint8& place)
	{
		sint64 value;
		if (!ParseNumber(text, value) || value < 0 || value >= DIM_MAX_FIGURES)
			return false;
		place = (uint8)value;
		return true;
	}

	static std::string_view LightModeName(DimensionsUSB::PadLight::Mode mode)
	{
		using Mode = DimensionsUSB::PadLight::Mode;
		switch (mode)
		{
		case Mode::Solid:
			return "solid";
		case Mode::Fade:
			return "fade";
		case Mode::Flash:
			return "flash";
		case Mode::Random:
			return "random";
		default:
			return "none";
		}
	}

	// The id is the one the game reads from the tag, and a Blank Tag's is 0 until the game writes a vehicle or gadget to
	// it. After the places come the lights the game last asked of the centre, left and right pad, numbered 1 to 3 as the
	// game numbers them
	static std::string HandleToypadStatus()
	{
		std::string reply;
		for (uint8 place = 0; place < DIM_MAX_FIGURES; place++)
		{
			uint32 id;
			if (g_dimensionstoypad.GetLoadedFigure(place, id))
				reply += fmt::format("place{} loaded {}\n", (uint32)place, id);
			else
				reply += fmt::format("place{} empty\n", (uint32)place);
		}

		const std::array<DimensionsUSB::PadLight, 3> lights = g_dimensionstoypad.GetLights();
		for (size_t pad = 0; pad < lights.size(); pad++)
		{
			const DimensionsUSB::PadLight& light = lights[pad];
			reply += fmt::format("light{} {} {} {} {} {} {} {} {} {} {} {}\n", pad + 1, LightModeName(light.mode),
								 (uint32)light.color[0], (uint32)light.color[1], (uint32)light.color[2],
								 (uint32)light.from[0], (uint32)light.from[1], (uint32)light.from[2],
								 (uint32)light.time, (uint32)light.off, (uint32)light.count, light.changes);
		}
		reply += "ok\n";
		return reply;
	}

	static std::string HandleToypadLoad(uint8 place, std::string_view path)
	{
		if (!IsAbsoluteLocalPath(path))
			return "error path must be absolute\n";

		// checked before the file is opened, so that a device or a pipe is never opened
		const fs::path tagPath = _utf8ToPath(path);
		std::error_code ec;
		if (!fs::is_regular_file(tagPath, ec) || fs::file_size(tagPath, ec) != DIM_FIGURE_SIZE)
			return "error not a toypad file\n";

		// opened for writing as the Emulated USB Devices window opens it, so the game can write a vehicle to a Blank Tag
		std::unique_ptr<FileStream> tagFile(FileStream::openFile2(tagPath, true));
		if (!tagFile)
			return "error cannot open file\n";

		std::array<uint8, DIM_FIGURE_SIZE> data;
		if (tagFile->readData(data.data(), data.size()) != data.size())
			return "error file too small\n";

		if (g_dimensionstoypad.HasFigureWithUid(data))
			return "error already on the toy pad\n";

		// LoadFigure would drop what the place holds without saving it or telling the game. The window takes it off first
		// (EmulatedUSBDeviceFrame::LoadMinifigPath), and here the client does, so that it knows what came off. Checked
		// last, once the new tag has been read: the client takes the old tag off only on this answer, so a tag that could
		// never go on never costs the place the tag it holds
		uint32 held;
		if (g_dimensionstoypad.GetLoadedFigure(place, held))
			return "error place taken\n";

		const uint32 id = g_dimensionstoypad.LoadFigure(data, std::move(tagFile), ToypadPadOf(place), place);
		return fmt::format("ok {}\n", id);
	}

	static std::string HandleToypadRemove(uint8 place)
	{
		// saves the tag to its file, closes the file and tells the game the tag was lifted
		if (!g_dimensionstoypad.RemoveFigure(ToypadPadOf(place), place, true))
			return "error place empty\n";
		return "ok\n";
	}

	static std::string HandleToypadClear()
	{
		for (uint8 place = 0; place < DIM_MAX_FIGURES; place++)
			g_dimensionstoypad.RemoveFigure(ToypadPadOf(place), place, true);
		return "ok\n";
	}

	// GetStatus looks for news every 100 ms while the game waits for some, so this keeps at least one of its looks
	// between the lift and the tag coming down again
	constexpr sint64 TOYPAD_LIFT_MS = 200;

	// This moves a tag as the window's Move does (EmulatedUSBDeviceFrame::MoveMinifig): the game is told the tag was
	// lifted, and then that it came down where it went, or where it was for a move to its own place (MoveFigure, as
	// RPCS3's dimensions_toypad::move_figure does it). There the pause between is the player's time in the Figure Mover.
	// MoveFigure takes off whatever the new place holds without a word to the client, and from an empty place it puts
	// down a tag with no file, so both places are checked first, and again after the pause, in which Cemu's own window
	// may have changed either. Each of these calls takes DimensionsUSB's lock itself, and the lock isn't recursive
	static std::string HandleToypadMove(uint8 from, uint8 to)
	{
		uint32 id;
		if (!g_dimensionstoypad.GetLoadedFigure(from, id))
			return "error place empty\n";
		if (to != from && g_dimensionstoypad.GetLoadedFigure(to, id))
			return "error place taken\n";

		g_dimensionstoypad.TempRemove(from);
		std::this_thread::sleep_for(std::chrono::milliseconds(TOYPAD_LIFT_MS));

		if (!g_dimensionstoypad.GetLoadedFigure(from, id))
			return "error place empty\n";
		if (to != from && g_dimensionstoypad.GetLoadedFigure(to, id))
		{
			g_dimensionstoypad.CancelRemove(from);
			return "error place taken\n";
		}

		g_dimensionstoypad.MoveFigure(ToypadPadOf(to), to, ToypadPadOf(from), from);
		return "ok\n";
	}

	// CreateFigure encrypts any id but 0 into the tag as a character's, a vehicle's or gadget's too, so the window's maker
	// offers only the characters and id 0, a Blank Tag, which the game fills in when a vehicle or gadget is built on it
	// (CreateDimensionFigureDialog; CreateFigure's own comment)
	static std::string HandleToypadCreate(uint32 id, std::string_view path)
	{
		if (!IsAbsoluteLocalPath(path))
			return "error path must be absolute\n";

		// the figure maker in the Emulated USB Devices window asks before it replaces a file, and nobody can be asked here
		const fs::path tagPath = _utf8ToPath(path);
		std::error_code ec;
		if (fs::exists(fs::symlink_status(tagPath, ec)))
			return "error file exists\n";

		if (!g_dimensionstoypad.CreateFigure(tagPath, id))
			return "error cannot create file\n";

		return "ok\n";
	}

	// every character, or every vehicle and gadget, the figure maker knows, in the order of their ids. The name comes last,
	// so its spaces need no quoting
	static std::string HandleToypadList(std::string_view which)
	{
		const bool minis = which == "minis";
		if (!minis && which != "tokens")
			return "error unknown command\n";

		const auto list = minis ? DimensionsUSB::GetListMinifigs() : DimensionsUSB::GetListTokens();
		std::string reply;
		for (const auto& [id, name] : list)
			reply += fmt::format("{} {}\n", id, name);
		reply += "ok\n";
		return reply;
	}

	// These make the calls the Emulated USB Devices window makes from the GUI thread, and ThreadFunc runs one at a time,
	// so they are as safe beside the title's own reads and writes of the Toy Pad as the window is
	static std::string HandleToypad(std::string_view line)
	{
		if (!s_toypadEmulated)
			return "error toypad not emulated\n";

		const Words words = SplitFirstWord(line);
		const std::string_view command = words.first;

		if (command == "status")
			return HandleToypadStatus();

		if (command == "list")
			return HandleToypadList(words.rest);

		if (command == "clear")
			return HandleToypadClear();

		if (command == "remove")
		{
			if (!words.hasRest)
				return "error missing place\n";
			uint8 place;
			if (!ParseToypadPlace(words.rest, place))
				return "error invalid place\n";
			return HandleToypadRemove(place);
		}

		if (command == "load")
		{
			if (!words.hasRest)
				return "error missing arguments\n";
			const Words placeAndPath = SplitFirstWord(words.rest);
			if (!placeAndPath.hasRest)
				return "error missing path\n";
			uint8 place;
			if (!ParseToypadPlace(placeAndPath.first, place))
				return "error invalid place\n";
			return HandleToypadLoad(place, placeAndPath.rest);
		}

		if (command == "move")
		{
			if (!words.hasRest)
				return "error missing arguments\n";
			const Words fromAndTo = SplitFirstWord(words.rest);
			if (!fromAndTo.hasRest)
				return "error missing place\n";
			uint8 from, to;
			if (!ParseToypadPlace(fromAndTo.first, from) || !ParseToypadPlace(fromAndTo.rest, to))
				return "error invalid place\n";
			return HandleToypadMove(from, to);
		}

		if (command == "create")
		{
			if (!words.hasRest)
				return "error missing arguments\n";
			const Words idAndPath = SplitFirstWord(words.rest);
			if (!idAndPath.hasRest)
				return "error missing path\n";
			// the window's figure maker takes any number up to 0xFFFF (CreateDimensionFigureDialog)
			sint64 id;
			if (!ParseNumber(idAndPath.first, id) || id < 0 || id > 0xFFFF)
				return "error invalid id\n";
			return HandleToypadCreate((uint32)id, idAndPath.rest);
		}

		return "error unknown command\n";
	}

	static std::string ProcessCommand(std::string_view line)
	{
		const size_t firstSpace = line.find(' ');
		const std::string_view command = line.substr(0, firstSpace);
		const bool hasArguments = firstSpace != std::string_view::npos;
		const std::string_view arguments = hasArguments ? line.substr(firstSpace + 1) : std::string_view();

		if (command == "hello")
			return "ok cemu-portal 1\n";

		// the server also runs for a title with only the Infinity Base or the Dimensions Toypad, which can't see the portal
		const bool forThePortal = command == "status" || command == "clear" || command == "remove" || command == "load" || command == "create";
		if (forThePortal && !s_portalEmulated)
			return "error portal not emulated\n";

		if (command == "status")
			return HandleStatus();

		if (command == "clear")
			return HandleClear();

		if (command == "remove")
		{
			if (!hasArguments)
				return "error missing slot\n";
			sint64 slot;
			if (!ParseNumber(arguments, slot))
				return "error invalid slot\n";
			return HandleRemove(slot);
		}

		if (command == "load")
		{
			if (!hasArguments)
				return "error missing arguments\n";
			const size_t secondSpace = arguments.find(' ');
			if (secondSpace == std::string_view::npos)
				return "error missing path\n";
			sint64 slot;
			if (!ParseNumber(arguments.substr(0, secondSpace), slot))
				return "error invalid arguments\n";
			return HandleLoad(slot, arguments.substr(secondSpace + 1));
		}

		if (command == "create")
		{
			if (!hasArguments)
				return "error missing arguments\n";
			const size_t idEnd = arguments.find(' ');
			const size_t varEnd = idEnd == std::string_view::npos ? idEnd : arguments.find(' ', idEnd + 1);
			if (varEnd == std::string_view::npos)
				return "error missing path\n";
			sint64 skyId, skyVar;
			if (!ParseNumber(arguments.substr(0, idEnd), skyId) || skyId < 0 || skyId > 0xFFFF)
				return "error invalid id\n";
			if (!ParseNumber(arguments.substr(idEnd + 1, varEnd - idEnd - 1), skyVar) || skyVar < 0 || skyVar > 0xFFFF)
				return "error invalid variant\n";
			return HandleCreate((uint16)skyId, (uint16)skyVar, arguments.substr(varEnd + 1));
		}

		if (command == "hush")
		{
			if (!hasArguments)
				return "error missing argument\n";
			return HandleHush(arguments);
		}

		if (command == "infinity")
			return HandleInfinity(arguments);

		if (command == "toypad")
			return HandleToypad(arguments);

		return "error unknown command\n";
	}

	// reads one line, ended by \n, \r or the end of the stream, which has to arrive whole within COMMAND_TIMEOUT_MS
	static bool ReadCommand(SOCKET s, const std::atomic_bool& stopRequested, std::string& line)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(COMMAND_TIMEOUT_MS);
		while (line.size() < MAX_COMMAND_SIZE)
		{
			const sint64 remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
			if (remainingMs <= 0 || stopRequested)
				return false;

			const sint32 ready = WaitForData(s, (sint32)std::min<sint64>(remainingMs, POLL_INTERVAL_MS));
			if (ready == 0 || (ready < 0 && WasInterrupted()))
				continue;
			if (ready < 0)
				return false;

			char ch;
			const sint32 received = (sint32)recv(s, &ch, 1, 0);
			if (received == 0)
				return true;
			if (received < 0)
			{
				if (WasInterrupted())
					continue;
				return false;
			}

			if (ch == '\r')
			{
				// the \n of a \r\n is taken too, as closing a connection that still has unread data resets it and can lose the reply
				if (WaitForData(s, 0) > 0)
					recv(s, &ch, 1, 0);
				return true;
			}
			if (ch == '\n')
				return true;

			line.push_back(ch);
		}
		return false;
	}

	static bool SendAll(SOCKET s, std::string_view data)
	{
		while (!data.empty())
		{
			const sint32 sent = (sint32)send(s, data.data(), (sint32)data.size(), SEND_FLAGS);
			if (sent > 0)
				data.remove_prefix(sent);
			else if (sent < 0 && WasInterrupted())
				continue;
			else
				return false;
		}
		return true;
	}

	static void HandleConnection(SOCKET client, const std::atomic_bool& stopRequested)
	{
		// on Windows an accepted socket inherits the listening socket's non-blocking mode
		if (!SetBlocking(client, true))
			return;
#if BOOST_OS_MACOS
		int noSigPipe = 1;
		setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe));
#endif

		std::string line;
		if (!ReadCommand(client, stopRequested, line) || line.empty())
			return;

		const std::string reply = ProcessCommand(line);
		// every reply ends in a line that starts with "ok" or "error"
		std::string_view outcome(reply);
		outcome.remove_suffix(1);
		outcome = outcome.substr(outcome.rfind('\n') + 1);
		cemuLog_log(LogType::InputAPI, "Skylanders portal IPC: {} -> {}", line, outcome);

		if (!SendAll(client, reply))
			cemuLog_log(LogType::InputAPI, "Skylanders portal IPC: could not send the reply");
	}

	static SOCKET OpenListenSocket(uint16 port)
	{
		SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (listenSocket == INVALID_SOCKET)
		{
			cemuLog_log(LogType::Force, "Skylanders portal IPC: cannot create a socket");
			return INVALID_SOCKET;
		}

		// on Windows no other program may bind the port while Cemu listens on it. Elsewhere SO_REUSEADDR only
		// lets Cemu listen again while connections it closed are still waiting out TIME_WAIT
		int option = 1;
#if BOOST_OS_WINDOWS
		const sint32 optionName = SO_EXCLUSIVEADDRUSE;
#else
		const sint32 optionName = SO_REUSEADDR;
#endif

		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons(port);

		// not blocking, so that a connection which is gone again before accept() can't hold up Stop()
		if (setsockopt(listenSocket, SOL_SOCKET, optionName, (const char*)&option, sizeof(option)) == SOCKET_ERROR ||
			!SetBlocking(listenSocket, false) ||
			bind(listenSocket, (sockaddr*)&address, sizeof(address)) == SOCKET_ERROR ||
			listen(listenSocket, SOMAXCONN) == SOCKET_ERROR)
		{
			cemuLog_log(LogType::Force, "Skylanders portal IPC: cannot listen on 127.0.0.1:{}, the port may be in use. Carrying on without it", port);
			closesocket(listenSocket);
			return INVALID_SOCKET;
		}
		return listenSocket;
	}

	class Server
	{
	  public:
		Server(SOCKET listenSocket)
			: m_listenSocket(listenSocket)
		{
			m_thread = std::thread(&Server::ThreadFunc, this);
		}

		~Server()
		{
			m_stopRequested = true;
			m_thread.join();
			closesocket(m_listenSocket);
#if BOOST_OS_WINDOWS
			WSACleanup();
#endif
		}

	  private:
		void ThreadFunc()
		{
			SetThreadName("SkylanderIPC");
			while (!m_stopRequested)
			{
				const sint32 ready = WaitForData(m_listenSocket, POLL_INTERVAL_MS);
				if (ready < 0 && !WasInterrupted())
				{
					cemuLog_log(LogType::Force, "Skylanders portal IPC: waiting for connections failed, stopping");
					return;
				}
				if (ready <= 0)
					continue;

				SOCKET client = accept(m_listenSocket, nullptr, nullptr);
				if (client == INVALID_SOCKET)
					continue;
				HandleConnection(client, m_stopRequested);
				closesocket(client);
			}
		}

		SOCKET m_listenSocket;
		std::atomic_bool m_stopRequested = false;
		std::thread m_thread;
	};

	static std::unique_ptr<Server> s_server;

	void Start()
	{
		if (s_server)
			return;
		const auto& config = GetConfig();
		// nsyshid attaches the emulated devices from these settings as the title starts, right before this, and not again until
		// the next title (BackendEmulated::AttachVisibleDevices). The server runs for any of them: hush serves every title
		// with one, and a client tells a build without a device's commands by its "error unknown command"
		const bool portal = config.emulated_usb_devices.emulate_skylander_portal.GetValue();
		const bool base = config.emulated_usb_devices.emulate_infinity_base.GetValue();
		const bool toypad = config.emulated_usb_devices.emulate_dimensions_toypad.GetValue();
		if (!config.skylanders_ipc.enabled || !(portal || base || toypad))
			return;
		s_portalEmulated = portal;
		s_baseEmulated = base;
		s_toypadEmulated = toypad;
		const uint16 port = config.skylanders_ipc.port.GetValue();

#if BOOST_OS_WINDOWS
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		{
			cemuLog_log(LogType::Force, "Skylanders portal IPC: cannot initialise Winsock");
			return;
		}
#endif
		SOCKET listenSocket = OpenListenSocket(port);
		if (listenSocket == INVALID_SOCKET)
		{
#if BOOST_OS_WINDOWS
			WSACleanup();
#endif
			return;
		}

		s_inputHushed = false;
		s_server = std::make_unique<Server>(listenSocket);
		cemuLog_log(LogType::Force, "Skylanders portal IPC: listening on 127.0.0.1:{}", port);
	}

	void Stop()
	{
		if (!s_server)
			return;
		s_server.reset();
		s_inputHushed = false;
		cemuLog_log(LogType::Force, "Skylanders portal IPC: stopped");
	}

	bool IsInputHushed()
	{
		return s_inputHushed;
	}
} // namespace nsyshid::SkylanderPortalIPC
