#include "IAudioAPI.h"

#if BOOST_OS_WINDOWS
#include "XAudio2API.h"
#include "XAudio27API.h"
#include "DirectSoundAPI.h"
#endif
#include "config/CemuConfig.h"
#if HAS_CUBEB
#include "CubebAPI.h"
#endif
#include <chrono>
#include <deque>
#include <mutex>

std::shared_mutex g_audioMutex;
AudioAPIPtr g_tvAudio;
AudioAPIPtr g_padAudio;
std::atomic_int32_t g_padVolume = 0;

// Trap Team plays its trap and villain sounds on the Traptanium Portal's own
// speaker. It sends them as 64 byte interrupt writes, each 32 samples of
// signed 16 bit little endian mono PCM at 8000 Hz, one every 4 ms in real
// time (Cemu pull request 1478, commit 4f4412b, nsyshid/Skylander.cpp
// SkylanderPortalDevice::Write; Dolphin pull request 11644,
// AudioCommon/Mixer.cpp lines 292 to 306 at 3eb30c7; read 27 October 2026).
// They are kept here and mixed into the TV's sound, as Dolphin mixes them
// into its own output, so they play on Cemu's output at its volume with no
// device or setting of their own. Upstream Cemu opens a device for them
// instead, which plays nothing until its settings name one and is reported
// glitchy on Windows 11 (Cemu issue 1705).
static std::mutex s_portalMutex;
static std::deque<sint16> s_portalSamples;
// The last sample mixed in, which the next one ramps from
static sint16 s_portalLastSample = 0;
// Whether the speaker is playing, or waiting for a lead to build up
static bool s_portalPlaying = false;
static std::chrono::steady_clock::time_point s_portalLastWrite;
// Half a second: a game that writes faster than the speaker plays loses its
// oldest samples rather than falling further and further behind
constexpr size_t kPortalMaxSamples = 4000;
constexpr uint32 kPortalSampleRate = 8000;
// 20 ms, five writes: the speaker starts only with this much waiting, so a
// write that lands just after a mix of the TV's 12 ms of sound doesn't leave
// a hole, Dolphin's own buffering being longer still (32 ms granules,
// AudioCommon/Mixer.h at 3eb30c7). A sound shorter than that plays once no
// write has come for as long
constexpr size_t kPortalLead = 160;
constexpr auto kPortalQuiet = std::chrono::milliseconds(20);

void PortalSpeaker_PushSamples(const sint16* samples, size_t count)
{
	std::lock_guard lock(s_portalMutex);
	s_portalSamples.insert(s_portalSamples.end(), samples, samples + count);
	while (s_portalSamples.size() > kPortalMaxSamples)
		s_portalSamples.pop_front();
	s_portalLastWrite = std::chrono::steady_clock::now();
}

void PortalSpeaker_MixInto(sint16* interleaved, size_t frames, uint32 channels, uint32 sampleRate)
{
	if (channels == 0 || sampleRate % kPortalSampleRate != 0)
		return;
	const size_t step = sampleRate / kPortalSampleRate;
	const size_t wanted = frames / step;
	std::vector<sint16> taken;
	sint16 previous;
	{
		std::lock_guard lock(s_portalMutex);
		if (!s_portalPlaying)
		{
			const bool lead = s_portalSamples.size() >= kPortalLead;
			const bool ended = !s_portalSamples.empty() && std::chrono::steady_clock::now() - s_portalLastWrite >= kPortalQuiet;
			s_portalPlaying = lead || ended;
		}
		// Nothing to play and nothing still sounding: the TV's sound is left
		// exactly as it is
		if (!s_portalPlaying && s_portalLastSample == 0)
			return;
		if (s_portalPlaying)
		{
			const size_t count = std::min(wanted, s_portalSamples.size());
			taken.assign(s_portalSamples.begin(), s_portalSamples.begin() + count);
			s_portalSamples.erase(s_portalSamples.begin(), s_portalSamples.begin() + count);
			// Run dry: the next sound waits for a lead again
			if (count < wanted)
				s_portalPlaying = false;
		}
		previous = s_portalLastSample;
	}
	// Each speaker sample spans `step` frames of the TV's sound, ramping from
	// the one before it so the sound isn't stepped. It goes on the front left
	// and right, or the one channel there is
	const uint32 mixedChannels = std::min<uint32>(channels, 2);
	const auto add = [&](size_t frame, sint32 value)
	{
		sint16* at = interleaved + frame * channels;
		for (uint32 c = 0; c < mixedChannels; c++)
			at[c] = (sint16)std::clamp<sint32>(at[c] + value, -32768, 32767);
	};
	size_t frame = 0;
	for (const sint16 next : taken)
	{
		for (size_t j = 0; j < step; j++)
			add(frame++, previous + (sint32)(next - previous) * (sint32)(j + 1) / (sint32)step);
		previous = next;
	}
	// Run short: down to silence over about a millisecond, not at once, which
	// would click
	if (taken.size() < wanted && previous != 0)
	{
		const size_t fade = std::min<size_t>(frames - frame, sampleRate / 1000);
		for (size_t j = 0; j < fade; j++)
			add(frame++, (sint32)previous * (sint32)(fade - j - 1) / (sint32)fade);
		previous = 0;
	}
	std::lock_guard lock(s_portalMutex);
	s_portalLastSample = previous;
}

void PortalSpeaker_Clear()
{
	std::lock_guard lock(s_portalMutex);
	s_portalSamples.clear();
	s_portalLastSample = 0;
	s_portalPlaying = false;
}

uint32 IAudioAPI::s_audioDelay = 2;
std::array<bool, IAudioAPI::AudioAPIEnd> IAudioAPI::s_availableApis{};

IAudioAPI::IAudioAPI(uint32 samplerate, uint32 channels, uint32 samples_per_block, uint32 bits_per_sample)
	: m_samplerate(samplerate), m_channels(channels), m_samplesPerBlock(samples_per_block), m_bitsPerSample(bits_per_sample) 
{
	m_bytesPerBlock = samples_per_block * channels * (bits_per_sample / 8);
	InitWFX(m_samplerate, m_channels, m_bitsPerSample);
}

void IAudioAPI::PrintLogging()
{
	cemuLog_log(LogType::Force, "------- Init Audio backend -------");
	cemuLog_log(LogType::Force, "DirectSound: {}", s_availableApis[DirectSound] ? "available" : "not supported");
	cemuLog_log(LogType::Force, "XAudio 2.8: {}", s_availableApis[XAudio2] ? "available" : "not supported");
	if (!s_availableApis[XAudio2])
	{
		cemuLog_log(LogType::Force, "XAudio 2.7: {}", s_availableApis[XAudio27] ? "available" : "not supported");
	}

	cemuLog_log(LogType::Force, "Cubeb: {}", s_availableApis[Cubeb] ? "available" : "not supported");
}

void IAudioAPI::InitWFX(sint32 samplerate, sint32 channels, sint32 bits_per_sample)
{
#if BOOST_OS_WINDOWS
	// move this to Windows-specific audio API implementations and use a cross-platform format here
	m_wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	m_wfx.Format.nChannels = channels;
	m_wfx.Format.nSamplesPerSec = samplerate;
	m_wfx.Format.wBitsPerSample = bits_per_sample;
	m_wfx.Format.nBlockAlign = (m_wfx.Format.nChannels * m_wfx.Format.wBitsPerSample) / 8; // must equal (nChannels � wBitsPerSample) / 8
	m_wfx.Format.nAvgBytesPerSec = m_wfx.Format.nSamplesPerSec * m_wfx.Format.nBlockAlign; // must equal nSamplesPerSec � nBlockAlign.
	m_wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);

	m_wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
	m_wfx.Samples.wValidBitsPerSample = bits_per_sample;
	switch (channels)
	{
	case 8:
		m_wfx.dwChannelMask |= (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT | SPEAKER_FRONT_LEFT_OF_CENTER | SPEAKER_FRONT_RIGHT_OF_CENTER);
		break;
	case 6:
		m_wfx.dwChannelMask |= (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT);
		break;
	case 4:
		m_wfx.dwChannelMask |= (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT);
		break;
	case 2:
		m_wfx.dwChannelMask |= (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
		break;
	default:
		m_wfx.dwChannelMask = 0;
		break;
	}
#endif
}

void IAudioAPI::InitializeStatic()
{
	s_audioDelay = GetConfig().audio_delay;

#if BOOST_OS_WINDOWS
	s_availableApis[DirectSound] = true;
	s_availableApis[XAudio2] = XAudio2API::InitializeStatic();
	if(!s_availableApis[XAudio2]) // don't try to initialize the older lib if the newer version is available
		s_availableApis[XAudio27] = XAudio27API::InitializeStatic();
#endif
#if HAS_CUBEB
	s_availableApis[Cubeb] = CubebAPI::InitializeStatic();
#endif
}

bool IAudioAPI::IsAudioAPIAvailable(AudioAPI api)
{
	if ((size_t)api < s_availableApis.size())
		return s_availableApis[api];

	cemu_assert_debug(false);
	return false;
}

AudioAPIPtr IAudioAPI::CreateDeviceFromConfig(bool TV, sint32 rate, sint32 samples_per_block, sint32 bits_per_sample)
{
	auto& config = GetConfig();
	sint32 channels = CemuConfig::AudioChannelsToNChannels(TV ? config.tv_channels : config.pad_channels);
	return CreateDeviceFromConfig(TV, rate, channels, samples_per_block, bits_per_sample);
}

AudioAPIPtr IAudioAPI::CreateDeviceFromConfig(bool TV, sint32 rate, sint32 channels, sint32 samples_per_block, sint32 bits_per_sample)
{
	AudioAPIPtr audioAPIDev;

	auto& config = GetConfig();

	const auto audio_api = (IAudioAPI::AudioAPI)config.audio_api;
	auto& selectedDevice = TV ? config.tv_device : config.pad_device;

	if(selectedDevice.empty())
		return {};

	IAudioAPI::DeviceDescriptionPtr device_description;
	if (IAudioAPI::IsAudioAPIAvailable(audio_api))
	{
		auto devices = IAudioAPI::GetDevices(audio_api);
		const auto it = std::find_if(devices.begin(), devices.end(), [&selectedDevice](const auto& d) {return d->GetIdentifier() == selectedDevice; });
		if (it != devices.end())
			device_description = *it;
	}
	if (!device_description)
		throw std::runtime_error("failed to find selected device while trying to create audio device");

	audioAPIDev = CreateDevice(audio_api, device_description, rate, channels, samples_per_block, bits_per_sample);
	audioAPIDev->SetVolume(TV ? config.tv_volume : config.pad_volume);
	return audioAPIDev;
}

AudioAPIPtr IAudioAPI::CreateDevice(AudioAPI api, const DeviceDescriptionPtr& device, sint32 samplerate, sint32 channels, sint32 samples_per_block, sint32 bits_per_sample)
{
	if (!IsAudioAPIAvailable(api))
		return {};

	switch(api)
	{
#if BOOST_OS_WINDOWS
	case DirectSound:
	{
		const auto tmp = std::dynamic_pointer_cast<DirectSoundAPI::DirectSoundDeviceDescription>(device);
		return std::make_unique<DirectSoundAPI>(tmp->GetGUID(), samplerate, channels, samples_per_block, bits_per_sample);
	}
	case XAudio27:
	{
		const auto tmp = std::dynamic_pointer_cast<XAudio27API::XAudio27DeviceDescription>(device);
		return std::make_unique<XAudio27API>(tmp->GetDeviceId(), samplerate, channels, samples_per_block, bits_per_sample);
	}
	case XAudio2:
	{
		const auto tmp = std::dynamic_pointer_cast<XAudio2API::XAudio2DeviceDescription>(device);
		return std::make_unique<XAudio2API>(tmp->GetDeviceId(), samplerate, channels, samples_per_block, bits_per_sample);
	}
#endif
#if HAS_CUBEB
    case Cubeb:
    {
        const auto tmp = std::dynamic_pointer_cast<CubebAPI::CubebDeviceDescription>(device);
        return std::make_unique<CubebAPI>(tmp->GetDeviceId(), samplerate, channels, samples_per_block, bits_per_sample);
    }
#endif
	default:
		throw std::runtime_error(fmt::format("invalid audio api: {}", api));
	}
}

std::vector<IAudioAPI::DeviceDescriptionPtr> IAudioAPI::GetDevices(AudioAPI api)
{
	if (!IsAudioAPIAvailable(api))
		return {};
	
	switch(api)
	{
#if BOOST_OS_WINDOWS
	case DirectSound:
	{
		return DirectSoundAPI::GetDevices();
	}
	case XAudio27:
	{
		return XAudio27API::GetDevices();
	}
	case XAudio2:
	{
		return XAudio2API::GetDevices();
	}
#endif
#if HAS_CUBEB
	case Cubeb:
	{
		return CubebAPI::GetDevices();
	}
#endif
	default:
		throw std::runtime_error(fmt::format("invalid audio api: {}", api));
	}
}

void IAudioAPI::SetAudioDelayOverride(uint32 delay)
{
	m_audioDelayOverride = delay;
}

uint32 IAudioAPI::GetAudioDelay() const
{
	return m_audioDelayOverride > 0 ? m_audioDelayOverride : s_audioDelay;
}
