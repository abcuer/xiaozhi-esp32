#ifndef MUSIC_PLAYER_H
#define MUSIC_PLAYER_H

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <cJSON.h>

#include "audio_codec.h"

class MusicPlayer {
public:
    struct Callbacks {
        std::function<void(const std::string& title, const std::string& artist)> on_playback_started;
        std::function<void(const std::string& reason)> on_playback_stopped;
        std::function<void(const std::string& reason)> on_playback_error;
    };

    MusicPlayer();
    ~MusicPlayer();

    void Initialize(AudioCodec* codec);
    void SetCallbacks(Callbacks callbacks);

    bool PlayFromUrl(const std::string& url, const std::string& title, const std::string& artist);
    void Stop();
    bool IsPlaying() const;
    cJSON* GetStatusJson() const;

private:
    enum class DecoderType {
        None,
        Mp3,
        M4a,
    };

    struct SessionInfo {
        uint32_t id = 0;
        std::string url;
        std::string title;
        std::string artist;
    };

    AudioCodec* codec_ = nullptr;
    Callbacks callbacks_;

    mutable std::mutex state_mutex_;
    SessionInfo session_;
    std::string last_error_;
    bool playing_ = false;
    std::atomic_bool stop_requested_{false};
    std::atomic_bool stream_finished_{false};
    bool playback_started_notified_ = false;

    std::deque<std::vector<int16_t>> pcm_queue_;
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    std::thread fetch_thread_;
    std::thread playback_thread_;

    void StartThreadsLocked();
    void JoinThreads();

    static void RegisterDecodersOnce();
    static DecoderType DetectDecoderType(const std::string& url, const std::string& content_type);

    void FetchThreadMain(uint32_t session_id);
    void PlaybackThreadMain(uint32_t session_id);

    bool IsSessionActive(uint32_t session_id) const;
    void ResetStateLocked();
    void ClearQueue();
    void NotifyPlaybackStarted();
    void NotifyPlaybackStopped(const std::string& reason);
    void NotifyPlaybackError(const std::string& reason);
    void FinalizeSession(uint32_t session_id, bool notify_stop, const std::string& reason);

    std::vector<int16_t> ConvertPcmBuffer(const uint8_t* pcm_bytes,
                                          size_t pcm_size,
                                          uint32_t sample_rate,
                                          uint8_t channels,
                                          uint8_t bits_per_sample,
                                          void*& resampler,
                                          int& resampler_src_rate,
                                          int& resampler_channels);
};

#endif
