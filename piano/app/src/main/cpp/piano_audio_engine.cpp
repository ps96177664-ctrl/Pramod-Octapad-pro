#include <jni.h>

#include <oboe/Oboe.h>
#include <fluidsynth.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

class PianoEngine final : public oboe::AudioStreamDataCallback {
public:
    struct SampleData {
        std::vector<int16_t> pcm;
        int sampleRate = 0;
        int channels = 0;
    };

    struct SampleVoice {
        std::shared_ptr<const SampleData> sample;
        int note = -1;
        int channel = 0;
        double position = 0.0;
        float gain = 0.0f;
        float release = 1.0f;
        bool releasing = false;
        bool active = false;
    };

    bool start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (synth_) return true;
        pitchBend_.fill(8192);

        settings_ = new_fluid_settings();
        if (settings_ == nullptr) return false;
        fluid_settings_setnum(settings_, "synth.gain", 0.7);
        fluid_settings_setint(settings_, "synth.polyphony", 256);
        fluid_settings_setint(settings_, "synth.reverb.active", 1);
        fluid_settings_setint(settings_, "synth.chorus.active", 1);

        oboe::AudioStreamBuilder builder;
        builder.setDirection(oboe::Direction::Output)
                ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
                ->setSharingMode(oboe::SharingMode::Shared)
                ->setFormat(oboe::AudioFormat::Float)
                ->setChannelCount(oboe::ChannelCount::Stereo)
                ->setUsage(oboe::Usage::Game)
                ->setContentType(oboe::ContentType::Music)
                ->setDataCallback(this);
        const oboe::Result result = builder.openStream(stream_);
        if (result != oboe::Result::OK || !stream_) {
            delete_fluid_settings(settings_);
            settings_ = nullptr;
            return false;
        }

        fluid_settings_setnum(settings_, "synth.sample-rate", stream_->getSampleRate());
        synth_ = new_fluid_synth(settings_);
        if (synth_ == nullptr) {
            stream_->close();
            stream_.reset();
            delete_fluid_settings(settings_);
            settings_ = nullptr;
            return false;
        }

        if (stream_->requestStart() == oboe::Result::OK) return true;

        stream_->close();
        stream_.reset();
        delete_fluid_synth(synth_);
        delete_fluid_settings(settings_);
        synth_ = nullptr;
        settings_ = nullptr;
        return false;
    }

    void stop() {
        if (stream_) {
            stream_->requestStop();
            stream_->close();
            stream_.reset();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (synth_) delete_fluid_synth(synth_);
        if (settings_) delete_fluid_settings(settings_);
        synth_ = nullptr;
        settings_ = nullptr;
    }

    bool loadSoundFont(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!synth_) return false;
        const int id = fluid_synth_sfload(synth_, path.c_str(), 1);
        if (id < 0) return false;
        soundFonts_.push_back(id);
        const bool selected = fluid_synth_program_select(synth_, 0, id, 0, 0) == FLUID_OK;
        if (selected) std::atomic_store(&activeSample_, std::shared_ptr<const SampleData>{});
        return selected;
    }

    bool selectPreset(int bank, int preset) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!synth_ || soundFonts_.empty()) return false;
        const bool selected = fluid_synth_program_select(
                synth_, 0, soundFonts_.back(), bank, preset) == FLUID_OK;
        if (selected) std::atomic_store(&activeSample_, std::shared_ptr<const SampleData>{});
        return selected;
    }

    void setSampleCacheLimit(size_t bytes) {
        std::lock_guard<std::mutex> lock(sampleCacheMutex_);
        sampleCacheLimitBytes_ = std::max<size_t>(bytes, 1);
        trimSampleCache();
    }

    bool cacheSample(const std::string& key, std::vector<int16_t>&& pcm,
                     int sampleRate, int channels) {
        if (pcm.empty() || sampleRate <= 0 || (channels != 1 && channels != 2)
                || pcm.size() % static_cast<size_t>(channels) != 0) {
            return false;
        }
        auto sample = std::make_shared<SampleData>();
        sample->pcm = std::move(pcm);
        sample->sampleRate = sampleRate;
        sample->channels = channels;
        const size_t sampleBytes = sample->pcm.size() * sizeof(int16_t);
        std::vector<std::shared_ptr<const SampleData>> evictedSamples;
        std::lock_guard<std::mutex> lock(sampleCacheMutex_);
        if (sampleBytes > sampleCacheLimitBytes_) return false;
        auto existing = sampleCache_.find(key);
        if (existing != sampleCache_.end()) {
            sampleCacheBytes_ -= existing->second.sample->pcm.size() * sizeof(int16_t);
            evictedSamples.push_back(existing->second.sample);
            sampleCacheOrder_.erase(existing->second.position);
            sampleCache_.erase(existing);
        }
        while (sampleCacheBytes_ + sampleBytes > sampleCacheLimitBytes_
                && !sampleCacheOrder_.empty()) {
            const std::string oldest = sampleCacheOrder_.back();
            auto cached = sampleCache_.find(oldest);
            if (cached != sampleCache_.end()) {
                sampleCacheBytes_ -= cached->second.sample->pcm.size() * sizeof(int16_t);
                evictedSamples.push_back(cached->second.sample);
                sampleCache_.erase(cached);
            }
            sampleCacheOrder_.pop_back();
        }
        sampleCacheOrder_.push_front(key);
        sampleCache_.emplace(key, CachedSample{std::move(sample), sampleCacheOrder_.begin()});
        sampleCacheBytes_ += sampleBytes;
        return true;
    }

    bool selectCachedSample(const std::string& key) {
        std::shared_ptr<const SampleData> selected;
        {
            std::lock_guard<std::mutex> lock(sampleCacheMutex_);
            auto cached = sampleCache_.find(key);
            if (cached == sampleCache_.end()) return false;
            sampleCacheOrder_.splice(sampleCacheOrder_.begin(), sampleCacheOrder_,
                                     cached->second.position);
            selected = cached->second.sample;
        }
        std::atomic_store(&activeSample_, std::move(selected));
        return true;
    }

    bool hasCachedSample(const std::string& key) {
        std::lock_guard<std::mutex> lock(sampleCacheMutex_);
        return sampleCache_.find(key) != sampleCache_.end();
    }

    void noteOn(int channel, int note, int velocity) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto sample = std::atomic_load(&activeSample_);
        if (sample) {
            auto voice = std::find_if(sampleVoices_.begin(), sampleVoices_.end(),
                                      [](const SampleVoice& item) { return !item.active; });
            if (voice == sampleVoices_.end()) {
                voice = std::min_element(sampleVoices_.begin(), sampleVoices_.end(),
                        [](const SampleVoice& left, const SampleVoice& right) {
                            return left.position > right.position;
                        });
            }
            *voice = SampleVoice{};
            voice->sample = std::move(sample);
            voice->note = note;
            voice->channel = std::clamp(channel, 0, static_cast<int>(pitchBend_.size()) - 1);
            voice->gain = std::clamp(velocity / 127.0f, 0.0f, 1.0f) * 0.8f;
            voice->active = true;
        } else if (synth_) {
            fluid_synth_noteon(synth_, channel, note, velocity);
        }
    }

    void noteOff(int channel, int note) {
        std::lock_guard<std::mutex> lock(mutex_);
        channel = std::clamp(channel, 0, static_cast<int>(pitchBend_.size()) - 1);
        for (auto& voice : sampleVoices_) {
            if (voice.active && voice.channel == channel && voice.note == note) {
                voice.releasing = true;
            }
        }
        if (synth_ && !std::atomic_load(&activeSample_)) {
            fluid_synth_noteoff(synth_, channel, note);
        }
    }

    void pitchBend(int channel, int value) {
        std::lock_guard<std::mutex> lock(mutex_);
        channel = std::clamp(channel, 0, static_cast<int>(pitchBend_.size()) - 1);
        value = std::clamp(value, 0, 16383);
        pitchBend_[static_cast<size_t>(channel)] = value;
        if (synth_ && !std::atomic_load(&activeSample_)) {
            fluid_synth_pitch_bend(synth_, channel, value);
        }
    }

    void setMasterVolume(float volume) {
        std::lock_guard<std::mutex> lock(mutex_);
        targetMasterVolume_ = std::clamp(volume, 0.0f, 1.0f);
    }

    void setEqualizer(float lowDb, float midDb, float highDb) {
        std::lock_guard<std::mutex> lock(mutex_);
        constexpr float dbScale = 0.11512925465f;
        targetEqGains_[0] = std::pow(10.0f, std::clamp(lowDb, -12.0f, 12.0f) * dbScale);
        targetEqGains_[1] = std::pow(10.0f, std::clamp(midDb, -12.0f, 12.0f) * dbScale);
        targetEqGains_[2] = std::pow(10.0f, std::clamp(highDb, -12.0f, 12.0f) * dbScale);
    }

    oboe::DataCallbackResult onAudioReady(
            oboe::AudioStream* stream, void* audioData, int32_t frames) override {
        auto* output = static_cast<float*>(audioData);
        std::lock_guard<std::mutex> lock(mutex_);
        const auto activeSample = std::atomic_load(&activeSample_);
        const float outputRate = static_cast<float>(stream->getSampleRate());
        if (!synth_ || activeSample) {
            std::fill(output, output + frames * 2, 0.0f);
        } else {
            left_.resize(static_cast<size_t>(frames));
            right_.resize(static_cast<size_t>(frames));
            if (fluid_synth_write_float(synth_, frames, left_.data(), 0, 1,
                                        right_.data(), 0, 1) != FLUID_OK) {
                std::fill(output, output + frames * 2, 0.0f);
            } else {
                for (int32_t i = 0; i < frames; ++i) {
                    output[i * 2] = left_[static_cast<size_t>(i)];
                    output[i * 2 + 1] = right_[static_cast<size_t>(i)];
                }
            }
        }

        if (activeSample || std::any_of(sampleVoices_.begin(), sampleVoices_.end(),
                [](const SampleVoice& voice) { return voice.active && voice.sample; })) {
            const double releaseStep = 1.0 / (outputRate * 0.015);
            for (auto& voice : sampleVoices_) {
                if (!voice.active || !voice.sample) continue;
                const auto& sample = *voice.sample;
                const size_t frameCount = sample.pcm.size()
                        / static_cast<size_t>(sample.channels);
                const double bendSemitones =
                        (pitchBend_[static_cast<size_t>(voice.channel)] - 8192)
                        * (2.0 / 8192.0);
                const double rate = (sample.sampleRate / outputRate)
                        * std::pow(2.0, (voice.note - 60 + bendSemitones) / 12.0);
                for (int32_t i = 0; i < frames; ++i) {
                    const size_t sourceFrame = static_cast<size_t>(voice.position);
                    if (sourceFrame >= frameCount) {
                        voice.active = false;
                        break;
                    }
                    const size_t nextFrame = std::min(sourceFrame + 1, frameCount - 1);
                    const float fraction = static_cast<float>(voice.position - sourceFrame);
                    const size_t first = sourceFrame * static_cast<size_t>(sample.channels);
                    const size_t second = nextFrame * static_cast<size_t>(sample.channels);
                    const float sampleLeft = (sample.pcm[first] * (1.0f - fraction)
                            + sample.pcm[second] * fraction) / 32768.0f;
                    const float sampleRight = sample.channels == 1 ? sampleLeft
                            : (sample.pcm[first + 1] * (1.0f - fraction)
                            + sample.pcm[second + 1] * fraction) / 32768.0f;
                    const float envelope = voice.gain * voice.release;
                    output[i * 2] += sampleLeft * envelope;
                    output[i * 2 + 1] += sampleRight * envelope;
                    voice.position += rate;
                    if (voice.releasing) {
                        voice.release -= static_cast<float>(releaseStep);
                        if (voice.release <= 0.0f) {
                            voice.active = false;
                            break;
                        }
                    }
                }
            }
        }

        constexpr float pi = 3.14159265358979323846f;
        const float lowAlpha = 1.0f - std::exp(-2.0f * pi
                * 250.0f / outputRate);
        const float highAlpha = 1.0f - std::exp(-2.0f * pi
                * 4000.0f / outputRate);
        constexpr float controlSmoothing = 0.002f;
        for (int32_t i = 0; i < frames; ++i) {
            masterVolume_ += (targetMasterVolume_ - masterVolume_) * controlSmoothing;
            for (size_t band = 0; band < eqGains_.size(); ++band) {
                eqGains_[band] += (targetEqGains_[band] - eqGains_[band])
                        * controlSmoothing;
            }
            for (size_t channel = 0; channel < 2; ++channel) {
                const size_t sampleIndex = static_cast<size_t>(i) * 2 + channel;
                const float input = output[sampleIndex];
                lowPassState_[channel] += lowAlpha * (input - lowPassState_[channel]);
                highPassState_[channel] += highAlpha * (input - highPassState_[channel]);
                const float low = lowPassState_[channel];
                const float mid = highPassState_[channel] - low;
                const float high = input - highPassState_[channel];
                const float equalized = low * eqGains_[0] + mid * eqGains_[1]
                        + high * eqGains_[2];
                output[sampleIndex] = std::clamp(equalized * masterVolume_, -1.0f, 1.0f);
            }
        }
        return oboe::DataCallbackResult::Continue;
    }

private:
    struct CachedSample {
        std::shared_ptr<const SampleData> sample;
        std::list<std::string>::iterator position;
    };

    void trimSampleCache() {
        while (sampleCacheBytes_ > sampleCacheLimitBytes_ && !sampleCacheOrder_.empty()) {
            const std::string oldest = sampleCacheOrder_.back();
            auto cached = sampleCache_.find(oldest);
            if (cached != sampleCache_.end()) {
                sampleCacheBytes_ -= cached->second.sample->pcm.size() * sizeof(int16_t);
                sampleCache_.erase(cached);
            }
            sampleCacheOrder_.pop_back();
        }
    }

    std::mutex mutex_;
    std::mutex sampleCacheMutex_;
    fluid_settings_t* settings_ = nullptr;
    fluid_synth_t* synth_ = nullptr;
    std::shared_ptr<oboe::AudioStream> stream_;
    std::vector<int> soundFonts_;
    std::vector<float> left_;
    std::vector<float> right_;
    std::array<SampleVoice, 64> sampleVoices_{};
    std::shared_ptr<const SampleData> activeSample_;
    std::unordered_map<std::string, CachedSample> sampleCache_;
    std::list<std::string> sampleCacheOrder_;
    size_t sampleCacheBytes_ = 0;
    size_t sampleCacheLimitBytes_ = 64 * 1024 * 1024;
    std::array<int, 16> pitchBend_{};
    std::array<float, 2> lowPassState_{};
    std::array<float, 2> highPassState_{};
    std::array<float, 3> targetEqGains_{1.0f, 1.0f, 1.0f};
    std::array<float, 3> eqGains_{1.0f, 1.0f, 1.0f};
    float targetMasterVolume_ = 0.8f;
    float masterVolume_ = 0.8f;
};

std::mutex gEngineMutex;
std::unique_ptr<PianoEngine> gEngine;

std::string toString(JNIEnv* env, jstring value) {
    if (value == nullptr) return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) return {};
    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    return result;
}

}  // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_com_pramod_piano_MainActivity_nativeCreate(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> lock(gEngineMutex);
    gEngine = std::make_unique<PianoEngine>();
    return gEngine->start() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_pramod_piano_MainActivity_nativeDestroy(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> lock(gEngineMutex);
    if (gEngine) gEngine->stop();
    gEngine.reset();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_pramod_piano_MainActivity_nativeLoadSoundFont(
        JNIEnv* env, jobject, jstring path) {
    const std::string file = toString(env, path);
    std::lock_guard<std::mutex> lock(gEngineMutex);
    return gEngine && gEngine->loadSoundFont(file) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_pramod_piano_MainActivity_nativeSetSampleCacheLimit(
        JNIEnv*, jobject, jlong bytes) {
    std::lock_guard<std::mutex> lock(gEngineMutex);
    if (!gEngine || bytes <= 0) return JNI_FALSE;
    gEngine->setSampleCacheLimit(static_cast<size_t>(bytes));
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_pramod_piano_MainActivity_nativeCacheSample(
        JNIEnv* env, jobject, jstring key, jshortArray pcm, jint sampleRate, jint channels) {
    if (key == nullptr || pcm == nullptr) return JNI_FALSE;
    const std::string cacheKey = toString(env, key);
    const jsize length = env->GetArrayLength(pcm);
    if (length <= 0) return JNI_FALSE;
    std::vector<int16_t> samples(static_cast<size_t>(length));
    env->GetShortArrayRegion(pcm, 0, length, reinterpret_cast<jshort*>(samples.data()));
    if (env->ExceptionCheck()) return JNI_FALSE;
    std::lock_guard<std::mutex> lock(gEngineMutex);
    return gEngine && gEngine->cacheSample(cacheKey, std::move(samples), sampleRate, channels)
            ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_pramod_piano_MainActivity_nativeSelectCachedSample(
        JNIEnv* env, jobject, jstring key) {
    if (key == nullptr) return JNI_FALSE;
    const std::string cacheKey = toString(env, key);
    std::lock_guard<std::mutex> lock(gEngineMutex);
    return gEngine && gEngine->selectCachedSample(cacheKey) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_pramod_piano_MainActivity_nativeHasCachedSample(
        JNIEnv* env, jobject, jstring key) {
    if (key == nullptr) return JNI_FALSE;
    const std::string cacheKey = toString(env, key);
    std::lock_guard<std::mutex> lock(gEngineMutex);
    return gEngine && gEngine->hasCachedSample(cacheKey) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_pramod_piano_MainActivity_nativeSelectPreset(
        JNIEnv*, jobject, jint bank, jint preset) {
    std::lock_guard<std::mutex> lock(gEngineMutex);
    return gEngine && gEngine->selectPreset(bank, preset) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_pramod_piano_MainActivity_nativeNoteOn(
        JNIEnv*, jobject, jint channel, jint note, jint velocity) {
    std::lock_guard<std::mutex> lock(gEngineMutex);
    if (gEngine) gEngine->noteOn(channel, note, velocity);
}

extern "C" JNIEXPORT void JNICALL
Java_com_pramod_piano_MainActivity_nativeNoteOff(
        JNIEnv*, jobject, jint channel, jint note) {
    std::lock_guard<std::mutex> lock(gEngineMutex);
    if (gEngine) gEngine->noteOff(channel, note);
}

extern "C" JNIEXPORT void JNICALL
Java_com_pramod_piano_MainActivity_nativePitchBend(
        JNIEnv*, jobject, jint channel, jint value) {
    std::lock_guard<std::mutex> lock(gEngineMutex);
    if (gEngine) gEngine->pitchBend(channel, value);
}

extern "C" JNIEXPORT void JNICALL
Java_com_pramod_piano_MainActivity_nativeSetMasterVolume(
        JNIEnv*, jobject, jfloat volume) {
    std::lock_guard<std::mutex> lock(gEngineMutex);
    if (gEngine) gEngine->setMasterVolume(volume);
}

extern "C" JNIEXPORT void JNICALL
Java_com_pramod_piano_MainActivity_nativeSetEqualizer(
        JNIEnv*, jobject, jfloat lowDb, jfloat midDb, jfloat highDb) {
    std::lock_guard<std::mutex> lock(gEngineMutex);
    if (gEngine) gEngine->setEqualizer(lowDb, midDb, highDb);
}
