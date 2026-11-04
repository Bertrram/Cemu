#pragma once

#include <mutex>

#include "nsyshid.h"
#include "Backend.h"

#include "Common/FileStream.h"

namespace nsyshid
{
	class DimensionsToypadDevice final : public Device
	{
	  public:
		DimensionsToypadDevice();
		~DimensionsToypadDevice() = default;

		bool Open() override;

		void Close() override;

		bool IsOpened() override;

		ReadResult Read(ReadMessage* message) override;

		WriteResult Write(WriteMessage* message) override;

		bool GetDescriptor(uint8 descType,
						   uint8 descIndex,
						   uint16 lang,
						   uint8* output,
						   uint32 outputMaxLength) override;

		bool SetIdle(uint8 ifIndex,
					 uint8 reportId,
					 uint8 duration) override;

		bool SetProtocol(uint8 ifIndex, uint8 protocol) override;

		bool SetReport(ReportMessage* message) override;

	  private:
		bool m_IsOpened;
	};

	// a tag is 0x2D pages of 4 bytes, and the Toy Pad has seven places, numbered as the Emulated USB Devices window numbers them
	constexpr uint16 DIM_FIGURE_SIZE = 0x2D * 0x04;
	constexpr uint8 DIM_MAX_FIGURES = 7;

	class DimensionsUSB
	{
	  public:
		struct DimensionsMini final
		{
			std::unique_ptr<FileStream> dimFile;
			std::array<uint8, 0x2D * 0x04> data{};
			uint8 index = 255;
			uint8 pad = 255;
			uint32 id = 0;
			void Save();
		};

		// The light the game last asked of a pad, as it asked it, kept so that the portal server can show the player
		// which pad the game lit. Nothing here changes what the game is answered
		struct PadLight final
		{
			enum class Mode : uint8
			{
				None, // nothing asked yet
				Solid,
				Fade,
				Flash,
				Random, // a fade to colours the pad picks itself, which the game never learns
			};
			Mode mode = Mode::None;
			std::array<uint8, 3> color{}; // as the game asked it; a Random fade leaves it as it was
			std::array<uint8, 3> from{};  // what the pad showed when the game asked for this
			uint8 time = 0;				  // a fade's ticks for each step, a flash's ticks on
			uint8 off = 0;				  // a flash's ticks off
			uint8 count = 0;			  // a fade's steps, 0 for ever; a flash's flashes, 0xFF for ever
			uint32 changes = 0;			  // one more for every light command the game sends this pad
		};

		void SendCommand(std::span<const uint8, 32> buf);
		std::array<uint8, 32> GetStatus();

		void GenerateRandomNumber(std::span<const uint8, 8> buf, uint8 sequence,
								  std::array<uint8, 32>& replyBuf);
		void InitializeRNG(uint32 seed);
		void GetChallengeResponse(std::span<const uint8, 8> buf, uint8 sequence,
								  std::array<uint8, 32>& replyBuf);
		void QueryBlock(uint8 index, uint8 page, std::array<uint8, 32>& replyBuf,
						uint8 sequence);
		void WriteBlock(uint8 index, uint8 page, std::span<const uint8, 4> toWriteBuf, std::array<uint8, 32>& replyBuf,
						uint8 sequence);
		void GetModel(std::span<const uint8, 8> buf, uint8 sequence,
					  std::array<uint8, 32>& replyBuf);

		bool RemoveFigure(uint8 pad, uint8 index, bool fullRemove);
		bool TempRemove(uint8 index);
		bool CancelRemove(uint8 index);
		uint32 LoadFigure(const std::array<uint8, 0x2D * 0x04>& buf, std::unique_ptr<FileStream> file, uint8 pad, uint8 index);
		bool CreateFigure(fs::path pathName, uint32 id);
		bool MoveFigure(uint8 pad, uint8 index, uint8 oldPad, uint8 oldIndex);
		static std::map<const uint32, const char*> GetListMinifigs();
		static std::map<const uint32, const char*> GetListTokens();
		std::string FindFigure(uint32 figNum);
		// for the portal server (SkylanderPortalIPC.cpp), which has no window to keep its own record of the places
		bool GetLoadedFigure(uint8 index, uint32& id);
		bool HasFigureWithUid(const std::array<uint8, DIM_FIGURE_SIZE>& data);
		std::array<PadLight, 3> GetLights(); // the centre, left and right pad

	  protected:
		std::mutex m_dimensionsMutex;
		std::array<DimensionsMini, DIM_MAX_FIGURES> m_figures{};

	  private:
		void NoteLight(std::span<const uint8, 32> buf);
		void RandomUID(std::array<uint8, 0x2D * 0x04>& uidBuffer);
		uint8 GenerateChecksum(const std::array<uint8, 32>& data,
							   int numOfBytes) const;
		std::array<uint8, 8> Decrypt(std::span<const uint8, 8> buf, std::optional<std::array<uint8, 16>> key);
		std::array<uint8, 8> Encrypt(std::span<const uint8, 8> buf, std::optional<std::array<uint8, 16>> key);
		std::array<uint8, 16> GenerateFigureKey(const std::array<uint8, 0x2D * 0x04>& uid);
		std::array<uint8, 4> PWDGenerate(const std::array<uint8, 0x2D * 0x04>& uid);
		std::array<uint8, 4> DimensionsRandomize(const std::vector<uint8> key, uint8 count);
		uint32 GetFigureId(const std::array<uint8, 0x2D * 0x04>& buf);
		uint32 Scramble(const std::array<uint8, 7>& uid, uint8 count);
		uint32 GetNext();
		DimensionsMini& GetFigureByIndex(uint8 index);

		uint32 m_randomA;
		uint32 m_randomB;
		uint32 m_randomC;
		uint32 m_randomD;

		bool m_isAwake = false;

		std::queue<std::array<uint8, 32>> m_figureAddedRemovedResponses;
		std::queue<std::array<uint8, 32>> m_queries;
		// guarded by m_dimensionsMutex, as each of the game's commands comes on a thread of its own (export_HIDWrite)
		std::array<PadLight, 3> m_lights{};
	};
	extern DimensionsUSB g_dimensionstoypad;

} // namespace nsyshid