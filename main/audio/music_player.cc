#include "music_player.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>

#include <esp_log.h>
#include <esp_pthread.h>

#include "board.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_ae_rate_cvt.h"

namespace {

constexpr int kHttpTimeoutMs = 5000;
constexpr int kMaxConsecutiveReadErrors = 8;
constexpr int kMaxHttpRedirects = 2;
constexpr int kMaxInitialStreamRetries = 2;
constexpr int kStreamCompletionThresholdPercent = 90;
constexpr size_t kStreamTailToleranceBytes = 32 * 1024;
constexpr size_t kInputBufferSize = 4096;
constexpr size_t kInitialOutputBufferSize = 4096;
constexpr size_t kMaxPcmQueueSize = 6;
constexpr size_t kMinStartQueueSize = 2;

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string GetUrlPathLower(const std::string& url) {
    auto lower = ToLower(url);
    auto query_pos = lower.find('?');
    if (query_pos != std::string::npos) {
        lower.resize(query_pos);
    }
    return lower;
}

bool EndsWith(const std::string& value, const char* suffix) {
    const auto suffix_len = std::strlen(suffix);
    return value.size() >= suffix_len && value.compare(value.size() - suffix_len, suffix_len, suffix) == 0;
}

bool IsNearStreamCompletion(size_t total_bytes_read, size_t content_length) {
    if (content_length == 0) {
        return false;
    }
    if (total_bytes_read >= content_length) {
        return true;
    }

    auto remaining = content_length - total_bytes_read;
    if (remaining <= kStreamTailToleranceBytes) {
        return true;
    }

    return static_cast<uint64_t>(total_bytes_read) * 100 >=
           static_cast<uint64_t>(content_length) * kStreamCompletionThresholdPercent;
}

class ScopedThreadConfig {
public:
    ScopedThreadConfig(const char* name, size_t stack_size, int priority) {
        previous_cfg_ = esp_pthread_get_default_config();
        auto cfg = previous_cfg_;
        cfg.thread_name = name;
        cfg.stack_size = stack_size;
        cfg.prio = priority;
        esp_pthread_set_cfg(&cfg);
    }

    ~ScopedThreadConfig() {
        esp_pthread_set_cfg(&previous_cfg_);
    }

private:
    esp_pthread_cfg_t previous_cfg_ = {};
};

union DecoderConfigStorage {
    esp_m4a_dec_cfg_t m4a_cfg;

    DecoderConfigStorage() {
        std::memset(this, 0, sizeof(*this));
    }
};

}  // namespace

static const char* TAG = "MusicPlayer";

MusicPlayer::MusicPlayer() = default;

MusicPlayer::~MusicPlayer() {
    Stop();
}

void MusicPlayer::Initialize(AudioCodec* codec) {
    codec_ = codec;
}

void MusicPlayer::SetCallbacks(Callbacks callbacks) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    callbacks_ = std::move(callbacks);
}

bool MusicPlayer::PlayFromUrl(const std::string& url, const std::string& title, const std::string& artist) {
    if (codec_ == nullptr || url.empty()) {
        return false;
    }

    Stop();

    std::lock_guard<std::mutex> lock(state_mutex_);
    session_.id++;
    session_.url = url;
    session_.title = title;
    session_.artist = artist;
    last_error_.clear();
    playing_ = true;
    stop_requested_.store(false);
    stream_finished_.store(false);
    playback_started_notified_ = false;

    StartThreadsLocked();
    return true;
}

void MusicPlayer::Stop() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!playing_ && !fetch_thread_.joinable() && !playback_thread_.joinable()) {
            return;
        }
        stop_requested_.store(true);
        stream_finished_.store(true);
    }

    ClearQueue();
    queue_cv_.notify_all();
    JoinThreads();

    std::lock_guard<std::mutex> lock(state_mutex_);
    ResetStateLocked();
}

bool MusicPlayer::IsPlaying() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return playing_;
}

cJSON* MusicPlayer::GetStatusJson() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "playing", playing_);
    cJSON_AddStringToObject(root, "title", session_.title.c_str());
    cJSON_AddStringToObject(root, "artist", session_.artist.c_str());
    cJSON_AddStringToObject(root, "url", session_.url.c_str());
    cJSON_AddStringToObject(root, "error", last_error_.c_str());
    return root;
}

void MusicPlayer::StartThreadsLocked() {
    auto session_id = session_.id;

    {
        ScopedThreadConfig thread_cfg("music_fetch", 12288, 4);
        fetch_thread_ = std::thread([this, session_id]() {
            FetchThreadMain(session_id);
        });
    }

    {
        ScopedThreadConfig thread_cfg("music_play", 6144, 5);
        playback_thread_ = std::thread([this, session_id]() {
            PlaybackThreadMain(session_id);
        });
    }
}

void MusicPlayer::JoinThreads() {
    if (fetch_thread_.joinable()) {
        fetch_thread_.join();
    }
    if (playback_thread_.joinable()) {
        playback_thread_.join();
    }
}

void MusicPlayer::RegisterDecodersOnce() {
    static std::once_flag once;
    std::call_once(once, []() {
        auto ret = esp_audio_dec_register_default();
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "esp_audio_dec_register_default failed: %d", ret);
        }
        ret = esp_audio_simple_dec_register_default();
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "esp_audio_simple_dec_register_default failed: %d", ret);
        }
    });
}

MusicPlayer::DecoderType MusicPlayer::DetectDecoderType(const std::string& url, const std::string& content_type) {
    auto lower_path = GetUrlPathLower(url);
    if (EndsWith(lower_path, ".mp3")) {
        return DecoderType::Mp3;
    }
    if (EndsWith(lower_path, ".m4a") || EndsWith(lower_path, ".mp4")) {
        return DecoderType::M4a;
    }

    auto lower_type = ToLower(content_type);
    if (lower_type.find("audio/mpeg") != std::string::npos || lower_type.find("audio/mp3") != std::string::npos) {
        return DecoderType::Mp3;
    }
    if (lower_type.find("audio/mp4") != std::string::npos || lower_type.find("audio/x-m4a") != std::string::npos) {
        return DecoderType::M4a;
    }
    return DecoderType::None;
}

void MusicPlayer::FetchThreadMain(uint32_t session_id) {
    RegisterDecodersOnce();

    std::unique_ptr<Http> http;
    esp_audio_simple_dec_handle_t decoder = nullptr;
    void* resampler = nullptr;
    int resampler_src_rate = 0;
    int resampler_channels = 0;
    DecoderConfigStorage decoder_cfg_storage;
    std::string error_message;

    auto close_decoder = [&]() {
        if (decoder != nullptr) {
            esp_audio_simple_dec_close(decoder);
            decoder = nullptr;
        }
    };
    auto close_resampler = [&]() {
        auto handle = static_cast<esp_ae_rate_cvt_handle_t>(resampler);
        if (handle != nullptr) {
            esp_ae_rate_cvt_close(handle);
            resampler = nullptr;
        }
        resampler_src_rate = 0;
        resampler_channels = 0;
    };

    for (int stream_attempt = 1; stream_attempt <= kMaxInitialStreamRetries + 1 && IsSessionActive(session_id); ++stream_attempt) {
        error_message.clear();
        http.reset();
        close_decoder();
        close_resampler();

        http = Board::GetInstance().GetNetwork()->CreateHttp(3);
        if (!http) {
            error_message = "创建音乐 HTTP 客户端失败";
            break;
        }

        http->SetTimeout(kHttpTimeoutMs);
        http->SetKeepAlive(true);

        SessionInfo session;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            session = session_;
        }

        std::string stream_url = session.url;
        int status_code = -1;
        int redirect_count = 0;
        while (redirect_count <= kMaxHttpRedirects) {
            if (!http->Open("GET", stream_url)) {
                error_message = "歌曲链接请求失败";
                break;
            }

            status_code = http->GetStatusCode();
            if (status_code == 301 || status_code == 302 || status_code == 303 || status_code == 307 || status_code == 308) {
                auto location = http->GetResponseHeader("Location");
                ESP_LOGW(TAG, "Music stream redirect: status=%d location=%s", status_code, location.c_str());
                http->Close();
                if (location.empty()) {
                    error_message = "歌曲链接请求失败";
                    break;
                }
                stream_url = location;
                ++redirect_count;
                continue;
            }
            break;
        }
        if (!error_message.empty()) {
            break;
        }

        if (status_code != 200) {
            auto content_type = http->GetResponseHeader("Content-Type");
            auto location = http->GetResponseHeader("Location");
            ESP_LOGE(TAG, "Music stream request failed: status=%d type=%s location=%s url=%s",
                     status_code, content_type.c_str(), location.c_str(), stream_url.c_str());
            http->Close();
            error_message = "歌曲链接请求失败";
            break;
        }

        auto content_type = http->GetResponseHeader("Content-Type");
        auto content_length = http->GetBodyLength();
        ESP_LOGI(TAG, "Music stream opened: status=%d type=%s length=%u url=%s",
                 status_code, content_type.c_str(), static_cast<unsigned>(content_length), stream_url.c_str());

        auto decoder_type = DetectDecoderType(stream_url, content_type);
        if (decoder_type == DecoderType::None) {
            error_message = "歌曲链接不可播放或格式暂不支持";
            break;
        }

        esp_audio_simple_dec_cfg_t dec_cfg = {};
        dec_cfg.use_frame_dec = false;
        if (decoder_type == DecoderType::Mp3) {
            dec_cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
        } else {
            decoder_cfg_storage.m4a_cfg.track_idx = 0;
            decoder_cfg_storage.m4a_cfg.aac_plus_enable = true;
            dec_cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
            dec_cfg.dec_cfg = &decoder_cfg_storage.m4a_cfg;
            dec_cfg.cfg_size = sizeof(decoder_cfg_storage.m4a_cfg);
        }

        auto ret = esp_audio_simple_dec_open(&dec_cfg, &decoder);
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "Failed to open simple decoder: %d", ret);
            error_message = "歌曲链接不可播放或格式暂不支持";
            break;
        }

        std::vector<uint8_t> input_buffer(kInputBufferSize);
        std::vector<uint8_t> output_buffer(kInitialOutputBufferSize);
        esp_audio_simple_dec_info_t dec_info = {};
        bool info_ready = false;
        bool playback_started = false;
        bool retry_stream_start = false;
        int consecutive_read_errors = 0;
        size_t total_bytes_read = 0;

        while (IsSessionActive(session_id)) {
            int bytes_read = http->Read(reinterpret_cast<char*>(input_buffer.data()), input_buffer.size());
            if (bytes_read < 0) {
                if (playback_started && IsNearStreamCompletion(total_bytes_read, content_length)) {
                    ESP_LOGW(TAG, "Treat truncated stream as EOF: read=%u total=%u",
                             static_cast<unsigned>(total_bytes_read), static_cast<unsigned>(content_length));
                    error_message.clear();
                    break;
                }
                if (!playback_started && total_bytes_read == 0 && stream_attempt <= kMaxInitialStreamRetries) {
                    ESP_LOGW(TAG, "Music stream start failed before first frame, retrying open (%d/%d)",
                             stream_attempt, kMaxInitialStreamRetries + 1);
                    retry_stream_start = true;
                    error_message.clear();
                    break;
                }
                consecutive_read_errors++;
                if (consecutive_read_errors >= kMaxConsecutiveReadErrors) {
                    error_message = "音乐流读取失败";
                    break;
                }
                ESP_LOGW(TAG, "Music stream read timeout/error (%d/%d)", consecutive_read_errors, kMaxConsecutiveReadErrors);
                continue;
            }
            consecutive_read_errors = 0;
            total_bytes_read += bytes_read;

            esp_audio_simple_dec_raw_t raw = {
                .buffer = input_buffer.data(),
                .len = static_cast<uint32_t>(bytes_read),
                .eos = bytes_read == 0,
                .consumed = 0,
                .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
            };

            bool decoded_this_round = false;
            while ((raw.len > 0 || raw.eos) && IsSessionActive(session_id)) {
                esp_audio_simple_dec_out_t out_frame = {
                    .buffer = output_buffer.data(),
                    .len = static_cast<uint32_t>(output_buffer.size()),
                    .needed_size = 0,
                    .decoded_size = 0,
                };

                auto ret = esp_audio_simple_dec_process(decoder, &raw, &out_frame);
                if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    if (out_frame.needed_size == 0) {
                        error_message = "歌曲链接不可播放或格式暂不支持";
                        break;
                    }
                    output_buffer.resize(out_frame.needed_size);
                    continue;
                }
                if (ret != ESP_AUDIO_ERR_OK) {
                    ESP_LOGE(TAG, "Simple decoder process failed: %d", ret);
                    error_message = "歌曲链接不可播放或格式暂不支持";
                    break;
                }

                if (out_frame.decoded_size > 0) {
                    if (!info_ready) {
                        ret = esp_audio_simple_dec_get_info(decoder, &dec_info);
                        if (ret != ESP_AUDIO_ERR_OK) {
                            ESP_LOGE(TAG, "Failed to get decoder info: %d", ret);
                            error_message = "歌曲链接不可播放或格式暂不支持";
                            break;
                        }
                        info_ready = true;
                    }

                    auto pcm = ConvertPcmBuffer(out_frame.buffer, out_frame.decoded_size,
                                                dec_info.sample_rate, dec_info.channel,
                                                dec_info.bits_per_sample, resampler,
                                                resampler_src_rate, resampler_channels);
                    if (pcm.empty()) {
                        error_message = "歌曲链接不可播放或格式暂不支持";
                        break;
                    }

                    {
                        std::unique_lock<std::mutex> queue_lock(queue_mutex_);
                        queue_cv_.wait(queue_lock, [this, session_id]() {
                            return !IsSessionActive(session_id) || pcm_queue_.size() < kMaxPcmQueueSize;
                        });
                        if (!IsSessionActive(session_id)) {
                            break;
                        }
                        pcm_queue_.push_back(std::move(pcm));
                    }
                    queue_cv_.notify_all();
                    decoded_this_round = true;
                    playback_started = true;
                    NotifyPlaybackStarted();
                }

                if (error_message.size() > 0) {
                    break;
                }
                if (raw.consumed == 0) {
                    break;
                }
                raw.len -= raw.consumed;
                raw.buffer += raw.consumed;
                raw.eos = raw.eos && raw.len == 0;
            }

            if (!error_message.empty()) {
                break;
            }
            if (bytes_read == 0 && !decoded_this_round) {
                if (!playback_started && total_bytes_read == 0 && stream_attempt <= kMaxInitialStreamRetries) {
                    ESP_LOGW(TAG, "Music stream closed before first frame, retrying open (%d/%d)",
                             stream_attempt, kMaxInitialStreamRetries + 1);
                    retry_stream_start = true;
                    error_message.clear();
                    break;
                }
                if (playback_started && IsNearStreamCompletion(total_bytes_read, content_length)) {
                    ESP_LOGW(TAG, "Treat short stream EOF as completed: read=%u total=%u",
                             static_cast<unsigned>(total_bytes_read), static_cast<unsigned>(content_length));
                } else if (content_length > 0 && total_bytes_read < content_length) {
                    ESP_LOGW(TAG, "Music stream ended early: read=%u total=%u",
                             static_cast<unsigned>(total_bytes_read), static_cast<unsigned>(content_length));
                    error_message = "音乐流读取失败";
                }
                break;
            }
            if (bytes_read == 0) {
                continue;
            }
        }

        if (!error_message.empty() && playback_started && IsNearStreamCompletion(total_bytes_read, content_length)) {
            ESP_LOGW(TAG, "Ignore late playback error near EOF: read=%u total=%u error=%s",
                     static_cast<unsigned>(total_bytes_read), static_cast<unsigned>(content_length), error_message.c_str());
            error_message.clear();
        }

        http->Close();
        close_decoder();
        close_resampler();

        if (retry_stream_start) {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            continue;
        }
        break;
    }

    if (http) {
        http->Close();
    }
    close_decoder();
    close_resampler();

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (session_.id != session_id) {
            return;
        }
        if (!error_message.empty()) {
            last_error_ = error_message;
        }
        stream_finished_.store(true);
    }
    queue_cv_.notify_all();
}

void MusicPlayer::PlaybackThreadMain(uint32_t session_id) {
    while (true) {
        std::vector<int16_t> pcm;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this]() {
                return !pcm_queue_.empty() || stream_finished_.load() || stop_requested_.load();
            });

            if (!pcm_queue_.empty() && !stream_finished_.load() && pcm_queue_.size() < kMinStartQueueSize && !stop_requested_.load()) {
                queue_cv_.wait_for(lock, std::chrono::milliseconds(120), [this]() {
                    return pcm_queue_.size() >= kMinStartQueueSize || stream_finished_.load() || stop_requested_.load();
                });
            }

            if (pcm_queue_.empty()) {
                if (stream_finished_.load() || stop_requested_.load()) {
                    break;
                }
                continue;
            }

            pcm = std::move(pcm_queue_.front());
            pcm_queue_.pop_front();
        }
        queue_cv_.notify_all();

        if (!codec_->output_enabled()) {
            codec_->EnableOutput(true);
        }
        codec_->OutputData(pcm);
    }

    std::string error;
    bool manual_stop = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (session_.id != session_id) {
            return;
        }
        error = last_error_;
        manual_stop = stop_requested_.load() && error.empty();
    }

    if (manual_stop) {
        return;
    }

    if (!error.empty()) {
        FinalizeSession(session_id, false, error);
        return;
    }

    FinalizeSession(session_id, true, "播放结束");
}

bool MusicPlayer::IsSessionActive(uint32_t session_id) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return playing_ && session_.id == session_id && !stop_requested_.load();
}

void MusicPlayer::ResetStateLocked() {
    playing_ = false;
    stop_requested_.store(false);
    stream_finished_.store(false);
    playback_started_notified_ = false;
    last_error_.clear();
    session_.url.clear();
    session_.title.clear();
    session_.artist.clear();
}

void MusicPlayer::ClearQueue() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    pcm_queue_.clear();
}

void MusicPlayer::NotifyPlaybackStarted() {
    Callbacks callbacks;
    std::string title;
    std::string artist;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (playback_started_notified_) {
            return;
        }
        playback_started_notified_ = true;
        callbacks = callbacks_;
        title = session_.title;
        artist = session_.artist;
    }
    if (callbacks.on_playback_started) {
        callbacks.on_playback_started(title, artist);
    }
}

void MusicPlayer::NotifyPlaybackStopped(const std::string& reason) {
    Callbacks callbacks;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        callbacks = callbacks_;
    }
    if (callbacks.on_playback_stopped) {
        callbacks.on_playback_stopped(reason);
    }
}

void MusicPlayer::NotifyPlaybackError(const std::string& reason) {
    Callbacks callbacks;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        callbacks = callbacks_;
    }
    if (callbacks.on_playback_error) {
        callbacks.on_playback_error(reason);
    }
}

void MusicPlayer::FinalizeSession(uint32_t session_id, bool notify_stop, const std::string& reason) {
    bool notify_error = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (session_.id != session_id) {
            return;
        }
        notify_error = !last_error_.empty();
        playing_ = false;
        stop_requested_.store(false);
        stream_finished_.store(false);
        playback_started_notified_ = false;
    }

    ClearQueue();

    if (notify_error) {
        NotifyPlaybackError(reason);
    } else if (notify_stop) {
        NotifyPlaybackStopped(reason);
    }
}

std::vector<int16_t> MusicPlayer::ConvertPcmBuffer(const uint8_t* pcm_bytes,
                                                   size_t pcm_size,
                                                   uint32_t sample_rate,
                                                   uint8_t channels,
                                                   uint8_t bits_per_sample,
                                                   void*& resampler,
                                                   int& resampler_src_rate,
                                                   int& resampler_channels) {
    if (pcm_bytes == nullptr || pcm_size == 0 || bits_per_sample != 16 || channels == 0) {
        return {};
    }

    const auto sample_count = pcm_size / sizeof(int16_t);
    const auto* source = reinterpret_cast<const int16_t*>(pcm_bytes);
    std::vector<int16_t> converted(source, source + sample_count);

    const auto target_channels = std::max(codec_->output_channels(), 1);
    if (channels != target_channels) {
        const auto frame_count = converted.size() / channels;
        std::vector<int16_t> remapped;
        remapped.reserve(frame_count * target_channels);

        if (target_channels == 1) {
            remapped.resize(frame_count);
            for (size_t frame = 0; frame < frame_count; ++frame) {
                int32_t mixed = 0;
                for (uint8_t ch = 0; ch < channels; ++ch) {
                    mixed += converted[frame * channels + ch];
                }
                remapped[frame] = static_cast<int16_t>(mixed / channels);
            }
        } else if (channels == 1 && target_channels == 2) {
            remapped.resize(frame_count * 2);
            for (size_t frame = 0; frame < frame_count; ++frame) {
                remapped[frame * 2] = converted[frame];
                remapped[frame * 2 + 1] = converted[frame];
            }
        } else if (channels >= 2 && target_channels == 2) {
            remapped.resize(frame_count * 2);
            for (size_t frame = 0; frame < frame_count; ++frame) {
                remapped[frame * 2] = converted[frame * channels];
                remapped[frame * 2 + 1] = converted[frame * channels + 1];
            }
        } else {
            return {};
        }
        converted = std::move(remapped);
        channels = target_channels;
    }

    if (sample_rate == static_cast<uint32_t>(codec_->output_sample_rate())) {
        return converted;
    }

    auto handle = static_cast<esp_ae_rate_cvt_handle_t>(resampler);
    if (handle == nullptr || resampler_src_rate != static_cast<int>(sample_rate) || resampler_channels != channels) {
        if (handle != nullptr) {
            esp_ae_rate_cvt_close(handle);
            handle = nullptr;
        }
        esp_ae_rate_cvt_cfg_t cfg = {
            .src_rate = sample_rate,
            .dest_rate = static_cast<uint32_t>(codec_->output_sample_rate()),
            .channel = channels,
            .bits_per_sample = ESP_AE_BIT16,
            .complexity = 2,
            .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
        };
        auto ret = esp_ae_rate_cvt_open(&cfg, &handle);
        if (ret != ESP_AE_ERR_OK || handle == nullptr) {
            ESP_LOGE(TAG, "Failed to open rate converter: %d", ret);
            return {};
        }
        resampler = handle;
        resampler_src_rate = sample_rate;
        resampler_channels = channels;
    }

    uint32_t input_samples = converted.size() / channels;
    uint32_t output_samples = 0;
    auto ret = esp_ae_rate_cvt_get_max_out_sample_num(handle, input_samples, &output_samples);
    if (ret != ESP_AE_ERR_OK || output_samples == 0) {
        ESP_LOGE(TAG, "Failed to query rate converter output size: %d", ret);
        return {};
    }
    std::vector<int16_t> resampled(output_samples * channels);
    uint32_t actual_output = output_samples;
    ret = esp_ae_rate_cvt_process(handle,
                                  reinterpret_cast<esp_ae_sample_t>(converted.data()),
                                  input_samples,
                                  reinterpret_cast<esp_ae_sample_t>(resampled.data()),
                                  &actual_output);
    if (ret != ESP_AE_ERR_OK) {
        ESP_LOGE(TAG, "Failed to process rate conversion: %d", ret);
        return {};
    }
    resampled.resize(actual_output * channels);
    return resampled;
}
