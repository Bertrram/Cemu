/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "SkylanderPortalIPC.h"
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
// one line per connection, one reply, then the connection is closed. "hello", "create" and "hush" are Cemu's own.

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

	static std::string ProcessCommand(std::string_view line)
	{
		const size_t firstSpace = line.find(' ');
		const std::string_view command = line.substr(0, firstSpace);
		const bool hasArguments = firstSpace != std::string_view::npos;
		const std::string_view arguments = hasArguments ? line.substr(firstSpace + 1) : std::string_view();

		if (command == "hello")
			return "ok cemu-portal 1\n";

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
		if (!config.skylanders_ipc.enabled || !config.emulated_usb_devices.emulate_skylander_portal)
			return;
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
