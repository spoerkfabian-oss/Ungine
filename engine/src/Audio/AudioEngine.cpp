#include "Engine/Audio/AudioEngine.h"
#include "Engine/Core/Log.h"

#include <miniaudio.h>
#include <verblib.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <numbers>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace Engine {

const char* ToString(AudioBus bus)
{
    switch (bus) {
    case AudioBus::Master:  return "Master";
    case AudioBus::World:   return "World";
    case AudioBus::Music:   return "Music";
    case AudioBus::Ui:      return "UI";
    case AudioBus::Ambient: return "Ambient";
    case AudioBus::Count:   break;
    }
    return "World";
}

namespace {
bool EqualsNoCase(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}
} // namespace

std::optional<AudioBus> AudioBusFromString(std::string_view name)
{
    for (std::size_t i = 0; i < kAudioBusCount; ++i)
        if (EqualsNoCase(ToString(static_cast<AudioBus>(i)), name))
            return static_cast<AudioBus>(i);
    return std::nullopt;
}

const char* ToString(Attenuation attenuation)
{
    switch (attenuation) {
    case Attenuation::Inverse:     return "Inverse";
    case Attenuation::Linear:      return "Linear";
    case Attenuation::Exponential: return "Exponential";
    }
    return "Inverse";
}

std::optional<Attenuation> AttenuationFromString(std::string_view name)
{
    for (Attenuation a : {Attenuation::Inverse, Attenuation::Linear, Attenuation::Exponential})
        if (EqualsNoCase(ToString(a), name))
            return a;
    return std::nullopt;
}

namespace {

constexpr float kOccludedCutoffHz = 800.0f;
constexpr float kOpenCutoffHz     = 20000.0f;
constexpr float kOccludedGainDb   = -12.0f;
constexpr float kSmoothSeconds    = 0.03f; // occlusion changes (raycasts at frame rate)

// Per-voice processing between the sound and its bus: volume ramps (SetVolume with fade, fade in)
// and the occlusion filter (one-pole low-pass + gain). Parameters are handed to the audio thread
// through atomics; everything else is touched by the audio thread only.
struct VoiceNode {
    ma_node_base base; // must be first
    std::uint32_t channels   = 2;
    float         sampleRate = 48000.0f;

    // Main thread → audio thread.
    std::atomic<float>         targetVolume{1.0f};
    std::atomic<std::uint32_t> rampFrames{0};
    std::atomic<std::uint32_t> volumeVersion{0};
    std::atomic<float>         targetOcclusion{0.0f};

    // Audio thread.
    std::uint32_t      seenVersion = 0;
    float              volume      = 1.0f;
    float              volumeStep  = 0.0f;
    std::uint32_t      stepsLeft   = 0;
    float              occlusion   = 0.0f; // smoothed
    float              smooth      = 0.0f; // per-sample smoothing factor
    float              coefficientOcclusion = 0.0f; // occlusion the filter coefficients were computed for
    float              coefficient          = 1.0f; // low-pass
    float              occlusionGain        = 1.0f;
    std::array<float, 8> lowpass{};        // filter state per channel

    static void Process(ma_node* node, const float** in, ma_uint32* frameCountIn, float** out, ma_uint32* frameCountOut)
    {
        auto&           self   = *static_cast<VoiceNode*>(node);
        const ma_uint32 frames = std::min(*frameCountIn, *frameCountOut);
        *frameCountIn = *frameCountOut = frames;
        const std::uint32_t version = self.volumeVersion.load(std::memory_order_acquire);
        if (version != self.seenVersion) {
            self.seenVersion       = version;
            const float         to = self.targetVolume.load(std::memory_order_relaxed);
            const std::uint32_t n  = self.rampFrames.load(std::memory_order_relaxed);
            self.stepsLeft  = n;
            self.volumeStep = n ? (to - self.volume) / static_cast<float>(n) : 0.0f;
            if (!n)
                self.volume = to;
        }
        const float occlusionTarget = self.targetOcclusion.load(std::memory_order_relaxed);
        const float* src = in[0];
        float*       dst = out[0];
        const std::uint32_t channels = std::min<std::uint32_t>(self.channels, static_cast<std::uint32_t>(self.lowpass.size()));
        if (self.stepsLeft == 0 && occlusionTarget == 0.0f && self.occlusion == 0.0f) { // no filter, constant gain
            const std::size_t count = static_cast<std::size_t>(frames) * self.channels;
            if (self.volume == 1.0f)
                std::memcpy(dst, src, sizeof(float) * count);
            else
                for (std::size_t i = 0; i < count; ++i)
                    dst[i] = src[i] * self.volume;
            if (frames > 0) // the filter continues from the signal when occlusion starts
                for (std::uint32_t c = 0; c < channels; ++c)
                    self.lowpass[c] = src[(frames - 1) * self.channels + c];
            return;
        }
        for (ma_uint32 f = 0; f < frames; ++f) {
            if (self.stepsLeft) {
                self.volume += self.volumeStep;
                if (--self.stepsLeft == 0)
                    self.volume = self.targetVolume.load(std::memory_order_relaxed);
            }
            self.occlusion += (occlusionTarget - self.occlusion) * self.smooth;
            if (std::abs(self.occlusion - occlusionTarget) < 1e-4f)
                self.occlusion = occlusionTarget;
            // Filter coefficients follow the smoothed occlusion every 16 samples (and once settled).
            if (self.occlusion != self.coefficientOcclusion && ((f & 15u) == 0 || self.occlusion == occlusionTarget)) {
                self.coefficientOcclusion = self.occlusion;
                self.coefficient          = 1.0f;
                self.occlusionGain        = 1.0f;
                if (self.occlusion > 0.0f) {
                    const float cutoff = kOpenCutoffHz * std::pow(kOccludedCutoffHz / kOpenCutoffHz, self.occlusion);
                    self.coefficient   = 1.0f - std::exp(-2.0f * std::numbers::pi_v<float> * cutoff / self.sampleRate);
                    self.occlusionGain = std::pow(10.0f, kOccludedGainDb * self.occlusion / 20.0f);
                }
            }
            const float coefficient = self.coefficient;
            const float gain        = self.volume * self.occlusionGain;
            for (std::uint32_t c = 0; c < self.channels; ++c) {
                const float x = src[f * self.channels + c];
                float       y = x;
                if (c < channels) {
                    float& state = self.lowpass[c];
                    state += coefficient * (x - state);
                    y = state;
                }
                dst[f * self.channels + c] = y * gain;
            }
        }
    }
};

ma_node_vtable g_VoiceNodeVTable = {VoiceNode::Process, nullptr, 1, 1, 0};

// A VoiceNode in the graph (voices, bus gains). The node must not move afterwards.
ma_result InitVoiceNode(ma_engine& engine, VoiceNode& node, float volume, float rampSeconds)
{
    const ma_uint32 channels   = ma_engine_get_channels(&engine);
    ma_node_config  config     = ma_node_config_init();
    config.vtable              = &g_VoiceNodeVTable;
    config.pInputChannels      = &channels;
    config.pOutputChannels     = &channels;
    node.channels   = channels;
    node.sampleRate = static_cast<float>(ma_engine_get_sample_rate(&engine));
    node.smooth     = 1.0f - std::exp(-1.0f / (kSmoothSeconds * node.sampleRate));
    node.volume     = rampSeconds > 0.0f ? 0.0f : volume;
    node.targetVolume.store(volume);
    node.rampFrames.store(static_cast<std::uint32_t>(rampSeconds * node.sampleRate));
    node.volumeVersion.store(rampSeconds > 0.0f ? 1u : 0u);
    return ma_node_init(ma_engine_get_node_graph(&engine), &config, nullptr, &node);
}

// Lock-free for the audio thread (miniaudio's smoothed ma_sound volume is not).
void SetNodeVolume(VoiceNode& node, float volume, float rampSeconds)
{
    node.targetVolume.store(std::max(volume, 0.0f), std::memory_order_relaxed);
    node.rampFrames.store(static_cast<std::uint32_t>(std::max(rampSeconds, 0.0f) * node.sampleRate), std::memory_order_relaxed);
    node.volumeVersion.fetch_add(1, std::memory_order_release);
}

// Freeverb (verblib) on the World bus. Keeps running without input so the tail decays.
struct ReverbNode {
    ma_node_base  base; // must be first
    std::uint32_t channels = 2;
    bool          active   = false; // verblib supports 1 or 2 channels
    verblib       verb{};

    std::atomic<float>         roomSize{0.5f}, damping{0.5f}, wet{0.0f}, width{1.0f};
    std::atomic<std::uint32_t> version{1};
    std::uint32_t              seenVersion = 0;
    std::array<float, 1024>    silence{}; // input while nothing plays on the bus (tail)

    static void Process(ma_node* node, const float** in, ma_uint32* frameCountIn, float** out, ma_uint32* frameCountOut)
    {
        auto&           self   = *static_cast<ReverbNode*>(node);
        const ma_uint32 frames = *frameCountOut;
        const float*    src    = (in && in[0]) ? in[0] : nullptr;
        if (frameCountIn)
            *frameCountIn = frames;
        if (!self.active) {
            if (src)
                std::memcpy(out[0], src, sizeof(float) * frames * self.channels);
            else
                std::memset(out[0], 0, sizeof(float) * frames * self.channels);
            return;
        }
        const std::uint32_t v = self.version.load(std::memory_order_acquire);
        if (v != self.seenVersion) {
            self.seenVersion = v;
            verblib_set_room_size(&self.verb, self.roomSize.load(std::memory_order_relaxed));
            verblib_set_damping(&self.verb, self.damping.load(std::memory_order_relaxed));
            verblib_set_width(&self.verb, self.width.load(std::memory_order_relaxed));
            // wet 1 = Freeverb's default full mix (1/3 before its internal scaling); dry stays unity.
            verblib_set_wet(&self.verb, self.wet.load(std::memory_order_relaxed) / 3.0f);
            verblib_set_dry(&self.verb, 0.5f);
        }
        if (src) {
            verblib_process(&self.verb, src, out[0], frames);
            return;
        }
        const ma_uint32 chunk = static_cast<ma_uint32>(self.silence.size() / self.channels);
        for (ma_uint32 done = 0; done < frames; done += chunk)
            verblib_process(&self.verb, self.silence.data(), out[0] + done * self.channels, std::min(chunk, frames - done));
    }
};

ma_node_vtable g_ReverbNodeVTable = {ReverbNode::Process, nullptr, 1, 1,
                                     MA_NODE_FLAG_CONTINUOUS_PROCESSING | MA_NODE_FLAG_ALLOW_NULL_INPUT};

ma_attenuation_model ToMa(Attenuation a)
{
    switch (a) {
    case Attenuation::Linear:      return ma_attenuation_model_linear;
    case Attenuation::Exponential: return ma_attenuation_model_exponential;
    case Attenuation::Inverse:     break;
    }
    return ma_attenuation_model_inverse;
}

} // namespace

struct AudioEngine::Impl {
    struct Voice {
        ma_sound                         sound{};
        VoiceNode                        node;
        ma_audio_buffer_ref              buffer{};
        ma_decoder                       decoder{};
        bool                             soundInit = false, nodeInit = false, bufferInit = false, decoderInit = false;
        std::shared_ptr<const SoundData> data;
        bool                             stopping = false;
        bool                             paused   = false;
        float                            occlusion = 0.0f;

        ~Voice()
        {
            if (soundInit)
                ma_sound_uninit(&sound);
            if (nodeInit)
                ma_node_uninit(&node, nullptr);
            if (decoderInit)
                ma_decoder_uninit(&decoder);
            if (bufferInit)
                ma_audio_buffer_ref_uninit(&buffer);
        }
    };

    AudioEngineDesc desc;
    ma_engine       engine{};
    bool            engineInit = false;
    bool            hasDevice  = false;
    std::string     deviceName;
    // Bus i: group (voices attach here) -> gain node (volume / mute) -> parent.
    std::array<ma_sound_group, kAudioBusCount> groups{};
    std::array<bool, kAudioBusCount>           groupInit{};
    std::array<VoiceNode, kAudioBusCount>      busNodes;
    std::array<bool, kAudioBusCount>           busNodeInit{};
    std::array<float, kAudioBusCount>          busVolume{};
    std::array<bool, kAudioBusCount>           busMuted{};
    ReverbNode      reverb;
    bool            reverbInit = false;
    ReverbParams    reverbParams;
    std::unordered_map<VoiceId, std::unique_ptr<Voice>> voices;
    VoiceId         nextId = 1;
    AudioStats      stats;

    ~Impl()
    {
        voices.clear();
        // Children before parents: buses -> reverb -> Master.
        for (std::size_t i = kAudioBusCount; i-- > 1;) {
            if (groupInit[i])
                ma_sound_group_uninit(&groups[i]);
            if (busNodeInit[i])
                ma_node_uninit(&busNodes[i], nullptr);
        }
        if (reverbInit)
            ma_node_uninit(&reverb, nullptr);
        if (groupInit[0])
            ma_sound_group_uninit(&groups[0]);
        if (busNodeInit[0])
            ma_node_uninit(&busNodes[0], nullptr);
        if (engineInit)
            ma_engine_uninit(&engine);
    }

    Voice* Find(VoiceId id) const
    {
        const auto it = voices.find(id);
        return it == voices.end() ? nullptr : it->second.get();
    }

    ma_sound_group& Group(AudioBus bus) { return groups[static_cast<std::size_t>(bus)]; }

    void ApplyBusVolume(AudioBus bus)
    {
        const auto i = static_cast<std::size_t>(bus);
        if (busNodeInit[i]) // 10 ms ramp: no clicks
            SetNodeVolume(busNodes[i], busMuted[i] ? 0.0f : busVolume[i], 0.01f);
    }
};

AudioEngine::AudioEngine(const AudioEngineDesc& desc) : m_Impl(std::make_unique<Impl>())
{
    Impl& s = *m_Impl;
    s.desc  = desc;
    s.desc.channels   = std::clamp<std::uint32_t>(desc.channels, 1, 8);
    s.desc.sampleRate = std::clamp<std::uint32_t>(desc.sampleRate, 8000, 192000);
    s.busVolume.fill(1.0f);

    ma_engine_config config = ma_engine_config_init();
    config.channels         = s.desc.channels;
    config.sampleRate       = s.desc.sampleRate;
    config.listenerCount    = 1;
    config.noAutoStart      = MA_TRUE; // started once the graph is complete
    ma_result result        = MA_ERROR;
    if (desc.device) {
        result = ma_engine_init(&config, &s.engine);
        if (result != MA_SUCCESS)
            ENGINE_WARN("Audio: no output device ({}), mixing offline", ma_result_description(result));
    }
    if (result != MA_SUCCESS) {
        config.noDevice = MA_TRUE;
        result          = ma_engine_init(&config, &s.engine);
        if (result != MA_SUCCESS)
            throw std::runtime_error(std::string("Audio: engine init failed: ") + ma_result_description(result));
    } else {
        s.hasDevice = true;
    }
    s.engineInit = true;
    if (s.hasDevice) {
        char   name[256] = {};
        size_t length    = 0;
        if (ma_device_get_name(ma_engine_get_device(&s.engine), ma_device_type_playback, name, sizeof(name), &length) == MA_SUCCESS)
            s.deviceName.assign(name, length);
        ENGINE_INFO("Audio: '{}', {} Hz, {} channels", s.deviceName, ma_engine_get_sample_rate(&s.engine),
                    ma_engine_get_channels(&s.engine));
    }

    const ma_uint32 channels = ma_engine_get_channels(&s.engine);
    const auto      check    = [](ma_result r, const char* what) {
        if (r != MA_SUCCESS)
            throw std::runtime_error(std::string("Audio: ") + what + ": " + ma_result_description(r));
    };

    // Master: group -> gain -> endpoint.
    for (std::size_t i = 0; i < kAudioBusCount; ++i) {
        check(InitVoiceNode(s.engine, s.busNodes[i], 1.0f, 0.0f), "bus gain");
        s.busNodeInit[i] = true;
    }
    ma_node_attach_output_bus(&s.busNodes[0], 0, ma_engine_get_endpoint(&s.engine), 0);
    ma_sound_group_config groupConfig = ma_sound_group_config_init_2(&s.engine);
    groupConfig.pInitialAttachment    = &s.busNodes[0];
    check(ma_sound_group_init_ex(&s.engine, &groupConfig, &s.groups[0]), "master bus");
    s.groupInit[0] = true;

    // Reverb → Master; World → reverb.
    ma_node_config nodeConfig  = ma_node_config_init();
    nodeConfig.vtable          = &g_ReverbNodeVTable;
    nodeConfig.pInputChannels  = &channels;
    nodeConfig.pOutputChannels = &channels;
    s.reverb.channels          = channels;
    s.reverb.active            = (channels == 1 || channels == 2) &&
                      verblib_initialize(&s.reverb.verb, ma_engine_get_sample_rate(&s.engine), channels) != 0;
    check(ma_node_init(ma_engine_get_node_graph(&s.engine), &nodeConfig, nullptr, &s.reverb), "reverb");
    s.reverbInit = true;
    ma_node_attach_output_bus(&s.reverb, 0, &s.groups[0], 0); // parameters: ReverbNode defaults

    // World: group -> gain -> reverb; the others: group -> gain -> Master.
    for (std::size_t i = 1; i < kAudioBusCount; ++i) {
        ma_node* parent = static_cast<AudioBus>(i) == AudioBus::World ? static_cast<ma_node*>(&s.reverb)
                                                                       : static_cast<ma_node*>(&s.groups[0]);
        ma_node_attach_output_bus(&s.busNodes[i], 0, parent, 0);
        ma_sound_group_config config2 = groupConfig;
        config2.pInitialAttachment    = &s.busNodes[i];
        check(ma_sound_group_init_ex(&s.engine, &config2, &s.groups[i]), "bus");
        s.groupInit[i] = true;
    }
    if (s.hasDevice)
        check(ma_engine_start(&s.engine), "start");
}

AudioEngine::~AudioEngine() = default;

bool               AudioEngine::HasDevice() const { return m_Impl->hasDevice; }
const std::string& AudioEngine::DeviceName() const { return m_Impl->deviceName; }
std::uint32_t      AudioEngine::SampleRate() const { return ma_engine_get_sample_rate(&m_Impl->engine); }
std::uint32_t      AudioEngine::Channels() const { return ma_engine_get_channels(&m_Impl->engine); }

VoiceId AudioEngine::Play(std::shared_ptr<const SoundData> sound, const VoiceDesc& desc)
{
    Impl& s = *m_Impl;
    if (!sound || (!sound->Streamed() && (sound->samples.empty() || sound->channels == 0)))
        return 0;
    if (s.voices.size() >= s.desc.maxVoices) {
        ++s.stats.dropped;
        return 0;
    }
    auto voice  = std::make_unique<Impl::Voice>();
    Impl::Voice& v = *voice;
    v.data      = std::move(sound);

    ma_data_source* source = nullptr;
    if (v.data->Streamed()) {
        const ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
#ifdef _WIN32
        const ma_result result = ma_decoder_init_file_w(v.data->streamFile.c_str(), &config, &v.decoder);
#else
        const ma_result result = ma_decoder_init_file(v.data->streamFile.c_str(), &config, &v.decoder);
#endif
        if (result != MA_SUCCESS) {
            ENGINE_WARN("Audio: cannot stream '{}' ({})", v.data->streamFile.string(), ma_result_description(result));
            return 0;
        }
        v.decoderInit = true;
        source        = &v.decoder;
    } else {
        if (ma_audio_buffer_ref_init(ma_format_f32, v.data->channels, v.data->samples.data(), v.data->frames, &v.buffer) != MA_SUCCESS)
            return 0;
        v.buffer.sampleRate = v.data->sampleRate;
        v.bufferInit        = true;
        source              = &v.buffer;
    }

    if (InitVoiceNode(s.engine, v.node, std::max(desc.volume, 0.0f), std::max(desc.fadeInSeconds, 0.0f)) != MA_SUCCESS)
        return 0;
    v.nodeInit = true;
    const AudioBus bus = desc.bus == AudioBus::Master || desc.bus == AudioBus::Count ? AudioBus::World : desc.bus;
    ma_node_attach_output_bus(&v.node, 0, &s.Group(bus), 0);

    ma_sound_config config    = ma_sound_config_init_2(&s.engine);
    config.pDataSource        = source;
    config.pInitialAttachment = &v.node;
    config.flags              = (desc.spatial ? 0u : static_cast<ma_uint32>(MA_SOUND_FLAG_NO_SPATIALIZATION)) |
                   (desc.loop ? static_cast<ma_uint32>(MA_SOUND_FLAG_LOOPING) : 0u);
    if (desc.startSeconds > 0.0) // not attached yet: seek synchronously
        ma_data_source_seek_to_pcm_frame(source, static_cast<ma_uint64>(desc.startSeconds * v.data->sampleRate));
    if (ma_sound_init_ex(&s.engine, &config, &v.sound) != MA_SUCCESS)
        return 0;
    v.soundInit = true;
    ma_sound_set_pitch(&v.sound, std::max(desc.pitch, 0.01f));
    if (desc.spatial) {
        ma_sound_set_attenuation_model(&v.sound, ToMa(desc.attenuation));
        ma_sound_set_min_distance(&v.sound, std::max(desc.minDistance, 0.01f));
        ma_sound_set_max_distance(&v.sound, std::max(desc.maxDistance, desc.minDistance));
        ma_sound_set_rolloff(&v.sound, std::max(desc.rolloff, 0.0f));
        ma_sound_set_doppler_factor(&v.sound, std::max(desc.doppler, 0.0f));
        ma_sound_set_position(&v.sound, desc.position.x, desc.position.y, desc.position.z);
        ma_sound_set_velocity(&v.sound, desc.velocity.x, desc.velocity.y, desc.velocity.z);
    }
    v.paused = desc.paused;
    if (!desc.paused)
        ma_sound_start(&v.sound);

    const VoiceId id = s.nextId++;
    s.voices.emplace(id, std::move(voice));
    return id;
}

void AudioEngine::Stop(VoiceId id, float fadeOutSeconds)
{
    Impl&        s = *m_Impl;
    Impl::Voice* v = s.Find(id);
    if (!v)
        return;
    if (fadeOutSeconds <= 0.0f || v->paused || !ma_sound_is_playing(&v->sound)) {
        s.voices.erase(id);
        return;
    }
    v->stopping = true;
    ma_sound_stop_with_fade_in_milliseconds(&v->sound, static_cast<ma_uint64>(fadeOutSeconds * 1000.0f));
}

void AudioEngine::StopAll(float fadeOutSeconds)
{
    std::vector<VoiceId> ids;
    ids.reserve(m_Impl->voices.size());
    for (const auto& [id, voice] : m_Impl->voices)
        ids.push_back(id);
    for (VoiceId id : ids)
        Stop(id, fadeOutSeconds);
}

bool AudioEngine::IsPlaying(VoiceId id) const
{
    const Impl::Voice* v = m_Impl->Find(id);
    return v && !v->stopping && !ma_sound_at_end(&v->sound);
}

void AudioEngine::SetVolume(VoiceId id, float volume, float fadeSeconds)
{
    if (Impl::Voice* v = m_Impl->Find(id))
        SetNodeVolume(v->node, volume, fadeSeconds);
}

void AudioEngine::SetPitch(VoiceId id, float pitch)
{
    if (Impl::Voice* v = m_Impl->Find(id))
        ma_sound_set_pitch(&v->sound, std::max(pitch, 0.01f));
}

void AudioEngine::SetPaused(VoiceId id, bool paused)
{
    Impl::Voice* v = m_Impl->Find(id);
    if (!v || v->stopping || v->paused == paused)
        return;
    v->paused = paused;
    if (paused)
        ma_sound_stop(&v->sound);
    else
        ma_sound_start(&v->sound);
}

void AudioEngine::SetLooping(VoiceId id, bool loop)
{
    if (Impl::Voice* v = m_Impl->Find(id))
        ma_sound_set_looping(&v->sound, loop ? MA_TRUE : MA_FALSE);
}

void AudioEngine::SetTransform(VoiceId id, const glm::vec3& position, const glm::vec3& velocity)
{
    if (Impl::Voice* v = m_Impl->Find(id)) {
        ma_sound_set_position(&v->sound, position.x, position.y, position.z);
        ma_sound_set_velocity(&v->sound, velocity.x, velocity.y, velocity.z);
    }
}

void AudioEngine::SetSpatialRange(VoiceId id, float minDistance, float maxDistance, float rolloff)
{
    if (Impl::Voice* v = m_Impl->Find(id)) {
        ma_sound_set_min_distance(&v->sound, std::max(minDistance, 0.01f));
        ma_sound_set_max_distance(&v->sound, std::max(maxDistance, minDistance));
        ma_sound_set_rolloff(&v->sound, std::max(rolloff, 0.0f));
    }
}

void AudioEngine::SetOcclusion(VoiceId id, float occlusion)
{
    if (Impl::Voice* v = m_Impl->Find(id)) {
        v->occlusion = std::clamp(occlusion, 0.0f, 1.0f);
        v->node.targetOcclusion.store(v->occlusion, std::memory_order_relaxed);
    }
}

double AudioEngine::Cursor(VoiceId id) const
{
    Impl::Voice* v = m_Impl->Find(id);
    if (!v)
        return 0.0;
    float seconds = 0.0f;
    ma_sound_get_cursor_in_seconds(&v->sound, &seconds);
    return seconds;
}

void AudioEngine::SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up, const glm::vec3& velocity)
{
    // miniaudio's listener world-up is not synchronized with the mixer: it stays +Y. Looking
    // straight up / down, the camera's up decides which way is "right" (tilted forward).
    glm::vec3 f = glm::length(forward) > 1e-6f ? glm::normalize(forward) : glm::vec3(0.0f, 0.0f, -1.0f);
    if (std::abs(f.y) > 0.999f && glm::length(up) > 1e-6f)
        f = glm::normalize(f + glm::normalize(up) * 0.02f);
    ma_engine& e = m_Impl->engine;
    ma_engine_listener_set_position(&e, 0, position.x, position.y, position.z);
    ma_engine_listener_set_direction(&e, 0, f.x, f.y, f.z);
    ma_engine_listener_set_velocity(&e, 0, velocity.x, velocity.y, velocity.z);
}

void AudioEngine::SetBusVolume(AudioBus bus, float volume)
{
    if (bus == AudioBus::Count)
        return;
    m_Impl->busVolume[static_cast<std::size_t>(bus)] = std::max(volume, 0.0f);
    m_Impl->ApplyBusVolume(bus);
}

float AudioEngine::BusVolume(AudioBus bus) const
{
    return bus == AudioBus::Count ? 0.0f : m_Impl->busVolume[static_cast<std::size_t>(bus)];
}

void AudioEngine::SetBusMuted(AudioBus bus, bool muted)
{
    if (bus == AudioBus::Count)
        return;
    m_Impl->busMuted[static_cast<std::size_t>(bus)] = muted;
    m_Impl->ApplyBusVolume(bus);
}

bool AudioEngine::BusMuted(AudioBus bus) const
{
    return bus != AudioBus::Count && m_Impl->busMuted[static_cast<std::size_t>(bus)];
}

void AudioEngine::SetReverb(const ReverbParams& params)
{
    Impl& s = *m_Impl;
    ReverbParams p;
    p.roomSize = std::clamp(params.roomSize, 0.0f, 1.0f);
    p.damping  = std::clamp(params.damping, 0.0f, 1.0f);
    p.wet      = std::clamp(params.wet, 0.0f, 1.0f);
    p.width    = std::clamp(params.width, 0.0f, 1.0f);
    if (p == s.reverbParams)
        return;
    s.reverbParams = p;
    s.reverb.roomSize.store(p.roomSize, std::memory_order_relaxed);
    s.reverb.damping.store(p.damping, std::memory_order_relaxed);
    s.reverb.wet.store(p.wet, std::memory_order_relaxed);
    s.reverb.width.store(p.width, std::memory_order_relaxed);
    s.reverb.version.fetch_add(1, std::memory_order_release);
}

const ReverbParams& AudioEngine::Reverb() const { return m_Impl->reverbParams; }

void AudioEngine::Update()
{
    Impl& s = *m_Impl;
    s.stats.voices = s.stats.streamed = s.stats.occluded = 0;
    for (auto it = s.voices.begin(); it != s.voices.end();) {
        Impl::Voice& v = *it->second;
        const bool finished = ma_sound_at_end(&v.sound) || (v.stopping && !ma_sound_is_playing(&v.sound));
        if (finished) {
            it = s.voices.erase(it);
            continue;
        }
        ++s.stats.voices;
        s.stats.streamed += v.data->Streamed() ? 1u : 0u;
        s.stats.occluded += v.occlusion > 0.0f ? 1u : 0u;
        ++it;
    }
}

void AudioEngine::Render(std::span<float> out)
{
    Impl&           s        = *m_Impl;
    const ma_uint32 channels = ma_engine_get_channels(&s.engine);
    const ma_uint64 frames   = out.size() / channels;
    if (s.hasDevice) { // the device thread mixes
        std::fill(out.begin(), out.end(), 0.0f);
        return;
    }
    ma_uint64 read = 0;
    ma_engine_read_pcm_frames(&s.engine, out.data(), frames, &read);
    std::fill(out.begin() + static_cast<std::ptrdiff_t>(read * channels), out.end(), 0.0f);
    s.stats.framesMixed += frames;
}

const AudioStats& AudioEngine::Stats() const { return m_Impl->stats; }

} // namespace Engine
