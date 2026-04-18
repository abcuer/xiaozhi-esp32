/*
 * MCP Server Implementation
 * Reference: https://modelcontextprotocol.io/specification/2024-11-05
 */

#include "mcp_server.h"
#include <esp_log.h>
#include <esp_app_desc.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <esp_pthread.h>

#include "application.h"
#include "display.h"
#include "oled_display.h"
#include "board.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "lvgl_display.h"

#define TAG "MCP"

namespace {

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

std::string Trim(std::string value) {
    auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string TrimPunctuation(std::string value) {
    static const char* kPrefixes[] = {"，", "。", "！", "？", ",", ".", "!", "?", "：", ":"};
    static const char* kSuffixes[] = {"，", "。", "！", "？", ",", ".", "!", "?", "：", ":"};
    bool changed = true;
    while (changed && !value.empty()) {
        changed = false;
        for (auto* prefix : kPrefixes) {
            const auto len = std::strlen(prefix);
            if (value.size() >= len && value.compare(0, len, prefix) == 0) {
                value.erase(0, len);
                changed = true;
                break;
            }
        }
        if (changed) {
            continue;
        }
        for (auto* suffix : kSuffixes) {
            const auto len = std::strlen(suffix);
            if (value.size() >= len && value.compare(value.size() - len, len, suffix) == 0) {
                value.resize(value.size() - len);
                changed = true;
                break;
            }
        }
    }
    return Trim(value);
}

std::string ExtractSongKeyword(const std::string& input) {
    auto keyword = Trim(input);
    auto left = keyword.find("《");
    auto right = keyword.find("》");
    if (left != std::string::npos && right != std::string::npos && right > left) {
        keyword = keyword.substr(left + strlen("《"), right - left - strlen("《"));
    }

    keyword = TrimPunctuation(keyword);

    static const char* kLeadingPhrases[] = {
        "播放歌曲", "播放一下", "播放一首", "播放", "放歌曲", "放一下", "放一首", "放首", "放",
        "来一首", "来首", "点一首", "我想听", "想听", "帮我放", "帮我播", "能放", "可以放"
    };
    for (auto* phrase : kLeadingPhrases) {
        const auto len = std::strlen(phrase);
        if (keyword.size() >= len && keyword.compare(0, len, phrase) == 0) {
            keyword.erase(0, len);
            break;
        }
    }

    keyword = TrimPunctuation(keyword);

    static const char* kTrailingParticles[] = {"啊", "呀", "呢", "哦", "嘛", "呗", "啦", "哇", "诶", "欸", "嗯"};
    for (auto* particle : kTrailingParticles) {
        const auto len = std::strlen(particle);
        if (keyword.size() > len && keyword.compare(keyword.size() - len, len, particle) == 0) {
            keyword.resize(keyword.size() - len);
            break;
        }
    }

    return TrimPunctuation(keyword);
}

std::string UrlEncode(const std::string& input) {
    std::string encoded;
    encoded.reserve(input.size() * 3);
    for (unsigned char ch : input) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded.push_back(static_cast<char>(ch));
            continue;
        }
        char buf[4];
        snprintf(buf, sizeof(buf), "%%%02X", ch);
        encoded.append(buf);
    }
    return encoded;
}

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

cJSON* CreateMusicResult(bool success, const char* message) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", success);
    cJSON_AddStringToObject(root, "message", message);
    return root;
}

struct MusicSearchResult {
    bool success = false;
    bool request_failed = false;
    bool timed_out = false;
    std::string message;
    std::string title;
    std::string artist;
    std::string url;
    std::string cover;
    int quality = 0;
    int last_error = 0;
};

struct MusicProbeResult {
    bool reachable = false;
    std::string url;
    std::string content_type;
    int status_code = -1;
};

struct MusicSearchOutcome {
    std::optional<MusicSearchResult> result;
    bool had_query_failure = false;
    bool had_unplayable_candidate = false;
    bool had_empty_result = false;
};

MusicSearchResult QueryTencentMusic(const std::string& keyword, int quality) {
    MusicSearchResult result;
    result.quality = quality;
    auto search_url = "https://api.vkeys.cn/v2/music/tencent?word=" + UrlEncode(keyword) +
                      "&choose=1&quality=" + std::to_string(quality);
    static constexpr int kMusicQueryTimeoutMs = 2500;
    static constexpr int kMaxQueryAttempts = 2;

    for (int attempt = 1; attempt <= kMaxQueryAttempts; ++attempt) {
        auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
        if (!http) {
            result.request_failed = true;
            result.message = "点歌接口请求失败";
        } else {
            http->SetTimeout(kMusicQueryTimeoutMs);
            if (!http->Open("GET", search_url)) {
                result.request_failed = true;
                result.last_error = http->GetLastError();
                result.timed_out = result.last_error == 0x8006;
                result.message = result.timed_out ? "点歌接口连接超时" : "点歌接口请求失败";
                ESP_LOGW(TAG, "Music query open failed: keyword=%s quality=%d attempt=%d err=0x%x",
                         keyword.c_str(), quality, attempt, result.last_error);
                http->Close();
            } else {
                auto status_code = http->GetStatusCode();
                auto response = http->ReadAll();
                http->Close();

                if (status_code != 200 || response.empty()) {
                    result.request_failed = true;
                    result.message = "点歌接口请求失败";
                    result.last_error = http->GetLastError();
                    result.timed_out = result.last_error == 0x8006;
                    ESP_LOGW(TAG, "Music query failed: keyword=%s quality=%d attempt=%d status=%d",
                             keyword.c_str(), quality, attempt, status_code);
                } else {
                    cJSON* root = cJSON_Parse(response.c_str());
                    if (!root) {
                        result.request_failed = true;
                        result.message = "点歌接口响应异常";
                        ESP_LOGW(TAG, "Music query returned invalid JSON: keyword=%s quality=%d attempt=%d",
                                 keyword.c_str(), quality, attempt);
                    } else {
                        auto code = cJSON_GetObjectItem(root, "code");
                        auto data = cJSON_GetObjectItem(root, "data");
                        auto song = data ? cJSON_GetObjectItem(data, "song") : nullptr;
                        auto singer = data ? cJSON_GetObjectItem(data, "singer") : nullptr;
                        auto url = data ? cJSON_GetObjectItem(data, "url") : nullptr;
                        auto cover = data ? cJSON_GetObjectItem(data, "cover") : nullptr;

                        if (!cJSON_IsNumber(code) || code->valueint != 200 || !cJSON_IsObject(data) ||
                            !cJSON_IsString(url) || url->valuestring[0] == '\0') {
                            cJSON_Delete(root);
                            result.request_failed = false;
                            result.message = "未找到可播放歌曲";
                            return result;
                        }

                        result.success = true;
                        result.request_failed = false;
                        result.title = cJSON_IsString(song) ? song->valuestring : keyword;
                        result.artist = cJSON_IsString(singer) ? singer->valuestring : "";
                        result.url = url->valuestring;
                        result.cover = cJSON_IsString(cover) ? cover->valuestring : "";
                        cJSON_Delete(root);
                        return result;
                    }
                }
            }
        }

        if (attempt < kMaxQueryAttempts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    }

    return result;
}

bool IsSupportedMusicContentType(const std::string& content_type) {
    auto lower_type = ToLower(content_type);
    return lower_type.find("audio/mpeg") != std::string::npos ||
           lower_type.find("audio/mp3") != std::string::npos ||
           lower_type.find("audio/mp4") != std::string::npos ||
           lower_type.find("audio/x-m4a") != std::string::npos;
}

bool IsLikelyPreviewMusicUrl(const std::string& url) {
    auto lower_url = ToLower(url);
    auto scheme_pos = lower_url.find("://");
    auto path_start = scheme_pos == std::string::npos ? lower_url.find('/') : lower_url.find('/', scheme_pos + 3);
    if (path_start == std::string::npos) {
        return false;
    }

    auto query_pos = lower_url.find('?', path_start);
    auto path = lower_url.substr(path_start, query_pos == std::string::npos ? std::string::npos : query_pos - path_start);
    auto file_start = path.find_last_of('/');
    auto filename = file_start == std::string::npos ? path : path.substr(file_start + 1);

    // Tencent preview links are commonly exposed as RS*.mp3, while full tracks
    // are typically C*/M*/F* container/file prefixes.
    return filename.rfind("rs", 0) == 0;
}

std::string BuildMusicDisplayName(const std::string& title, const std::string& artist) {
    if (title.empty()) {
        return artist.empty() ? "未知歌曲" : artist;
    }
    if (artist.empty()) {
        return title;
    }
    return title + " - " + artist;
}

const char* DescribeMusicProbeFailure(const MusicProbeResult& probe, const std::string& url) {
    if (probe.status_code == 404) {
        return "死链/已失效";
    }
    if (probe.status_code == -1) {
        return "连接超时或响应头获取失败";
    }
    if (!probe.content_type.empty() && !IsSupportedMusicContentType(probe.content_type)) {
        return "格式不支持";
    }
    if (IsLikelyPreviewMusicUrl(probe.url.empty() ? url : probe.url)) {
        return "试听流/短版音源";
    }
    if (probe.status_code == 200 || probe.status_code == 206) {
        return "可连通但未通过本地校验";
    }
    return "不可播放";
}

MusicProbeResult ProbeMusicStream(const std::string& initial_url) {
    MusicProbeResult result;
    if (initial_url.empty()) {
        return result;
    }

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    if (!http) {
        return result;
    }

    http->SetTimeout(4000);
    std::string stream_url = initial_url;
    int redirect_count = 0;
    while (redirect_count <= 2) {
        if (!http->Open("HEAD", stream_url)) {
            http->Close();
            return result;
        }

        auto status_code = http->GetStatusCode();
        if (status_code == 301 || status_code == 302 || status_code == 303 || status_code == 307 || status_code == 308) {
            auto location = http->GetResponseHeader("Location");
            http->Close();
            if (location.empty()) {
                return result;
            }
            stream_url = location;
            ++redirect_count;
            continue;
        }

        auto content_type = http->GetResponseHeader("Content-Type");
        http->Close();

        result.status_code = status_code;
        result.url = stream_url;
        result.content_type = content_type;
        result.reachable = (status_code == 200 || status_code == 206) &&
                           IsSupportedMusicContentType(content_type) &&
                           !IsLikelyPreviewMusicUrl(stream_url);
        return result;
    }

    return result;
}

MusicSearchOutcome FindPlayableMusicResult(const std::string& keyword) {
    MusicSearchOutcome outcome;

    // Current upstream responds to at least quality=1..12. On this device,
    // MP3 is much more resilient to mid-stream network jitter because the
    // player can resume it by byte range, while M4A/AAC often fails hard once
    // the stream is truncated. So prefer full-length MP3 qualities first, and
    // then fall back to other full-track formats.
    static const int kCandidateQualities[] = {8, 6, 9, 3, 2, 4, 12, 10, 11, 5, 7, 1};

    std::vector<std::string> checked_urls;
    checked_urls.reserve(sizeof(kCandidateQualities) / sizeof(kCandidateQualities[0]));

    for (int quality : kCandidateQualities) {
        auto candidate = QueryTencentMusic(keyword, quality);
        if (!candidate.success || candidate.url.empty()) {
            if (candidate.request_failed) {
                outcome.had_query_failure = true;
                ESP_LOGW(TAG, "点歌查询失败：关键词=%s quality=%d 原因=%s",
                         keyword.c_str(), quality, candidate.message.c_str());
                if (candidate.timed_out) {
                    ESP_LOGW(TAG, "点歌接口超时，继续尝试下一档音源：关键词=%s 当前quality=%d",
                             keyword.c_str(), quality);
                }
            } else {
                outcome.had_empty_result = true;
            }
            continue;
        }

        if (std::find(checked_urls.begin(), checked_urls.end(), candidate.url) != checked_urls.end()) {
            continue;
        }
        checked_urls.push_back(candidate.url);

        auto probe = ProbeMusicStream(candidate.url);
        if (probe.reachable) {
            candidate.url = probe.url;
            ESP_LOGI(TAG, "已选中最终音源：%s quality=%d 格式=%s url=%s",
                     BuildMusicDisplayName(candidate.title, candidate.artist).c_str(),
                     quality, probe.content_type.c_str(), candidate.url.c_str());
            outcome.result = std::move(candidate);
            return outcome;
        }

        outcome.had_unplayable_candidate = true;
        ESP_LOGW(TAG, "音源已丢弃：关键词=%s quality=%d 原因=%s status=%d type=%s url=%s",
                 keyword.c_str(), quality, DescribeMusicProbeFailure(probe, candidate.url),
                 probe.status_code, probe.content_type.c_str(), candidate.url.c_str());
    }

    if (outcome.had_query_failure && !outcome.had_unplayable_candidate && !outcome.had_empty_result) {
        ESP_LOGW(TAG, "所有点歌查询都失败了：关键词=%s", keyword.c_str());
    } else if (!outcome.had_query_failure && (outcome.had_unplayable_candidate || outcome.had_empty_result)) {
        ESP_LOGW(TAG, "所有候选音源都不可播放：关键词=%s", keyword.c_str());
    } else if (outcome.had_query_failure) {
        ESP_LOGW(TAG, "本轮点歌未完成，部分音源查询失败：关键词=%s", keyword.c_str());
    }

    return outcome;
}

template <typename T>
std::optional<T> RunOnMainThreadAndWait(Application& app,
                                        std::chrono::milliseconds timeout,
                                        std::function<T()> task) {
    struct SharedState {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        std::optional<T> result;
    };

    auto state = std::make_shared<SharedState>();
    app.Schedule([state, task = std::move(task)]() mutable {
        T local_result = task();
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->result = std::move(local_result);
            state->done = true;
        }
        state->cv.notify_one();
    });

    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->cv.wait_for(lock, timeout, [state]() {
        return state->done;
    })) {
        return std::nullopt;
    }
    return state->result;
}

}  // namespace

McpServer::McpServer() {
}

McpServer::~McpServer() {
    for (auto tool : tools_) {
        delete tool;
    }
    tools_.clear();
}

void McpServer::AddCommonTools() {
    // *Important* To speed up the response time, we add the common tools to the beginning of
    // the tools list to utilize the prompt cache.
    // **重要** 为了提升响应速度，我们把常用的工具放在前面，利用 prompt cache 的特性。

    // Backup the original tools list and restore it after adding the common tools.
    auto original_tools = std::move(tools_);
    auto& board = Board::GetInstance();

    // Do not add custom tools here.
    // Custom tools must be added in the board's InitializeTools function.

    AddTool("self.get_device_status",
        "Provides the real-time information of the device, including the current status of the audio speaker, screen, battery, network, etc.\n"
        "Use this tool for: \n"
        "1. Answering questions about current condition (e.g. what is the current volume of the audio speaker?)\n"
        "2. As the first step to control the device (e.g. turn up / down the volume of the audio speaker, etc.)",
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
            return board.GetDeviceStatusJson();
        });

    AddTool("self.audio_speaker.set_volume", 
        "Set the volume of the audio speaker. If the current volume is unknown, you must call `self.get_device_status` tool first and then call this tool.",
        PropertyList({
            Property("volume", kPropertyTypeInteger, 0, 100)
        }), 
        [&board](const PropertyList& properties) -> ReturnValue {
            auto codec = board.GetAudioCodec();
            codec->SetOutputVolume(properties["volume"].value<int>());
            return true;
        });
    
    auto backlight = board.GetBacklight();
    if (backlight) {
        AddTool("self.screen.set_brightness",
            "Set the brightness of the screen.",
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 0, 100)
            }),
            [backlight](const PropertyList& properties) -> ReturnValue {
                uint8_t brightness = static_cast<uint8_t>(properties["brightness"].value<int>());
                backlight->SetBrightness(brightness, true);
                return true;
            });
    }

#ifdef HAVE_LVGL
    auto display = board.GetDisplay();
    if (display && display->GetTheme() != nullptr) {
        AddTool("self.screen.set_theme",
            "Set the theme of the screen. The theme can be `light` or `dark`.",
            PropertyList({
                Property("theme", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto theme_name = properties["theme"].value<std::string>();
                auto& theme_manager = LvglThemeManager::GetInstance();
                auto theme = theme_manager.GetTheme(theme_name);
                if (theme != nullptr) {
                    display->SetTheme(theme);
                    return true;
                }
                return false;
            });
    }

    auto camera = board.GetCamera();
    if (camera) {
        AddTool("self.camera.take_photo",
            "Always remember you have a camera. If the user asks you to see something, use this tool to take a photo and then explain it.\n"
            "Args:\n"
            "  `question`: The question that you want to ask about the photo.\n"
            "Return:\n"
            "  A JSON object that provides the photo information.",
            PropertyList({
                Property("question", kPropertyTypeString)
            }),
            [camera](const PropertyList& properties) -> ReturnValue {
                // Lower the priority to do the camera capture
                TaskPriorityReset priority_reset(1);

                if (!camera->Capture()) {
                    throw std::runtime_error("Failed to capture photo");
                }
                auto question = properties["question"].value<std::string>();
                return camera->Explain(question);
            });
    }

#endif

    auto music_search_tool = new McpTool("self.music.search_and_play",
        "搜索并播放一首歌曲。用户让你播放音乐、点歌、播放某首歌时，使用这个工具。参数 keyword 只传歌曲关键词，不要带多余解释。",
        PropertyList({
            Property("keyword", kPropertyTypeString)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto keyword = ExtractSongKeyword(properties["keyword"].value<std::string>());
            if (keyword.empty()) {
                return CreateMusicResult(false, "未提供有效歌曲关键词");
            }

            static constexpr int kMusicSearchRounds = 3;
            std::optional<MusicSearchResult> search_result;
            bool should_report_network_issue = false;
            bool should_report_not_found = false;

            for (int round = 1; round <= kMusicSearchRounds; ++round) {
                auto outcome = FindPlayableMusicResult(keyword);
                if (outcome.result.has_value()) {
                    search_result = std::move(outcome.result);
                    break;
                }

                if (!outcome.had_query_failure) {
                    should_report_not_found = true;
                    break;
                }

                should_report_network_issue = true;
                ESP_LOGW(TAG, "点歌第%d轮未成功，准备重试：关键词=%s", round, keyword.c_str());
                if (round < kMusicSearchRounds) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                }
            }

            if (!search_result.has_value()) {
                if (should_report_not_found) {
                    return CreateMusicResult(false, "没有找到可播放的歌曲");
                }
                if (should_report_network_issue) {
                    return CreateMusicResult(false, "点歌网络异常，请稍后再试");
                }
                return CreateMusicResult(false, "没有找到可播放的歌曲");
            }

            std::string title = search_result->title;
            std::string artist = search_result->artist;
            std::string play_url = search_result->url;

            auto& app = Application::GetInstance();
            auto started = RunOnMainThreadAndWait<bool>(app,
                                                        std::chrono::milliseconds(15000),
                                                        [&app, &play_url, &title, &artist]() {
                                                            return app.StartMusicPlayback(play_url, title, artist);
                                                        });
            if (!started.has_value()) {
                return CreateMusicResult(false, "设备忙，请稍后重试");
            }

            if (!started.value()) {
                return CreateMusicResult(false, "歌曲链接不可播放或格式暂不支持");
            }

            cJSON* result = cJSON_CreateObject();
            std::string play_message = title.empty() ? "正在播放" : ("正在播放《" + title + "》");
            cJSON_AddBoolToObject(result, "success", true);
            cJSON_AddStringToObject(result, "message", play_message.c_str());
            cJSON_AddStringToObject(result, "keyword", keyword.c_str());
            cJSON_AddStringToObject(result, "title", title.c_str());
            cJSON_AddStringToObject(result, "artist", artist.c_str());
            cJSON_AddStringToObject(result, "url", play_url.c_str());
            cJSON_AddNumberToObject(result, "quality", search_result->quality);
            if (!search_result->cover.empty()) {
                cJSON_AddStringToObject(result, "cover", search_result->cover.c_str());
            }
            return result;
        });
    music_search_tool->set_requires_main_thread(false);
    AddTool(music_search_tool);

    AddTool("self.music.stop",
        "停止当前正在播放的网络音乐。用户说停止播放、停止音乐、别放了时，使用这个工具。",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            if (!app.GetMusicPlayer().IsPlaying()) {
                return CreateMusicResult(true, "当前没有正在播放的音乐");
            }
            app.StopMusicPlayback(true, "音乐已停止");
            return CreateMusicResult(true, "音乐已停止");
        });

    AddTool("self.music.get_status",
        "获取当前音乐播放状态，包括是否正在播放、歌曲名、歌手、URL 和错误信息。",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            return Application::GetInstance().GetMusicPlayer().GetStatusJson();
        });

    // Restore the original tools list to the end of the tools list
    tools_.insert(tools_.end(), original_tools.begin(), original_tools.end());
}

void McpServer::AddUserOnlyTools() {
    // System tools
    AddUserOnlyTool("self.get_system_info",
        "Get the system information",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& board = Board::GetInstance();
            return board.GetSystemInfoJson();
        });

    AddUserOnlyTool("self.reboot", "Reboot the system",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            app.Schedule([&app]() {
                ESP_LOGW(TAG, "User requested reboot");
                vTaskDelay(pdMS_TO_TICKS(1000));

                app.Reboot();
            });
            return true;
        });

    // Firmware upgrade
    AddUserOnlyTool("self.upgrade_firmware", "Upgrade firmware from a specific URL. This will download and install the firmware, then reboot the device.",
        PropertyList({
            Property("url", kPropertyTypeString, "The URL of the firmware binary file to download and install")
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            auto url = properties["url"].value<std::string>();
            ESP_LOGI(TAG, "User requested firmware upgrade from URL: %s", url.c_str());
            
            auto& app = Application::GetInstance();
            app.Schedule([url, &app]() {
                bool success = app.UpgradeFirmware(url);
                if (!success) {
                    ESP_LOGE(TAG, "Firmware upgrade failed");
                }
            });
            
            return true;
        });

    // Display control
#ifdef HAVE_LVGL
    auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (display) {
        AddUserOnlyTool("self.screen.get_info", "Information about the screen, including width, height, etc.",
            PropertyList(),
            [display](const PropertyList& properties) -> ReturnValue {
                cJSON *json = cJSON_CreateObject();
                cJSON_AddNumberToObject(json, "width", display->width());
                cJSON_AddNumberToObject(json, "height", display->height());
                if (dynamic_cast<OledDisplay*>(display)) {
                    cJSON_AddBoolToObject(json, "monochrome", true);
                } else {
                    cJSON_AddBoolToObject(json, "monochrome", false);
                }
                return json;
            });

#if CONFIG_LV_USE_SNAPSHOT
        AddUserOnlyTool("self.screen.snapshot", "Snapshot the screen and upload it to a specific URL",
            PropertyList({
                Property("url", kPropertyTypeString),
                Property("quality", kPropertyTypeInteger, 80, 1, 100)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto quality = properties["quality"].value<int>();

                std::string jpeg_data;
                if (!display->SnapshotToJpeg(jpeg_data, quality)) {
                    throw std::runtime_error("Failed to snapshot screen");
                }

                ESP_LOGI(TAG, "Upload snapshot %u bytes to %s", jpeg_data.size(), url.c_str());
                
                // 构造multipart/form-data请求体
                std::string boundary = "----ESP32_SCREEN_SNAPSHOT_BOUNDARY";
                
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
                http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
                if (!http->Open("POST", url)) {
                    throw std::runtime_error("Failed to open URL: " + url);
                }
                {
                    // 文件字段头部
                    std::string file_header;
                    file_header += "--" + boundary + "\r\n";
                    file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"screenshot.jpg\"\r\n";
                    file_header += "Content-Type: image/jpeg\r\n";
                    file_header += "\r\n";
                    http->Write(file_header.c_str(), file_header.size());
                }

                // JPEG数据
                http->Write((const char*)jpeg_data.data(), jpeg_data.size());

                {
                    // multipart尾部
                    std::string multipart_footer;
                    multipart_footer += "\r\n--" + boundary + "--\r\n";
                    http->Write(multipart_footer.c_str(), multipart_footer.size());
                }
                http->Write("", 0);

                if (http->GetStatusCode() != 200) {
                    throw std::runtime_error("Unexpected status code: " + std::to_string(http->GetStatusCode()));
                }
                std::string result = http->ReadAll();
                http->Close();
                ESP_LOGI(TAG, "Snapshot screen result: %s", result.c_str());
                return true;
            });
        
        AddUserOnlyTool("self.screen.preview_image", "Preview an image on the screen",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);

                if (!http->Open("GET", url)) {
                    throw std::runtime_error("Failed to open URL: " + url);
                }
                int status_code = http->GetStatusCode();
                if (status_code != 200) {
                    throw std::runtime_error("Unexpected status code: " + std::to_string(status_code));
                }

                size_t content_length = http->GetBodyLength();
                char* data = (char*)heap_caps_malloc(content_length, MALLOC_CAP_8BIT);
                if (data == nullptr) {
                    throw std::runtime_error("Failed to allocate memory for image: " + url);
                }
                size_t total_read = 0;
                while (total_read < content_length) {
                    int ret = http->Read(data + total_read, content_length - total_read);
                    if (ret < 0) {
                        heap_caps_free(data);
                        throw std::runtime_error("Failed to download image: " + url);
                    }
                    if (ret == 0) {
                        break;
                    }
                    total_read += ret;
                }
                http->Close();

                auto image = std::make_unique<LvglAllocatedImage>(data, content_length);
                display->SetPreviewImage(std::move(image));
                return true;
            });
#endif // CONFIG_LV_USE_SNAPSHOT
    }
#endif // HAVE_LVGL

    // Assets download url
    auto& assets = Assets::GetInstance();
    if (assets.partition_valid()) {
        AddUserOnlyTool("self.assets.set_download_url", "Set the download url for the assets",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                Settings settings("assets", true);
                settings.SetString("download_url", url);
                return true;
            });
    }

    AddUserOnlyTool("self.music.play_url", "Play a music URL directly for debugging.",
        PropertyList({
            Property("url", kPropertyTypeString),
            Property("title", kPropertyTypeString, std::string("调试播放")),
            Property("artist", kPropertyTypeString, std::string(""))
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            if (!app.StartMusicPlayback(properties["url"].value<std::string>(),
                                        properties["title"].value<std::string>(),
                                        properties["artist"].value<std::string>())) {
                return CreateMusicResult(false, "歌曲链接不可播放或格式暂不支持");
            }
            return CreateMusicResult(true, "开始播放调试音乐");
        });
}

void McpServer::AddTool(McpTool* tool) {
    // Prevent adding duplicate tools
    if (std::find_if(tools_.begin(), tools_.end(), [tool](const McpTool* t) { return t->name() == tool->name(); }) != tools_.end()) {
        ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
        return;
    }

    ESP_LOGI(TAG, "Add tool: %s%s", tool->name().c_str(), tool->user_only() ? " [user]" : "");
    tools_.push_back(tool);
}

void McpServer::AddTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    AddTool(new McpTool(name, description, properties, callback));
}

void McpServer::AddUserOnlyTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    auto tool = new McpTool(name, description, properties, callback);
    tool->set_user_only(true);
    AddTool(tool);
}

void McpServer::ParseMessage(const std::string& message) {
    cJSON* json = cJSON_Parse(message.c_str());
    if (json == nullptr) {
        ESP_LOGE(TAG, "Failed to parse MCP message: %s", message.c_str());
        return;
    }
    ParseMessage(json);
    cJSON_Delete(json);
}

void McpServer::ParseCapabilities(const cJSON* capabilities) {
    auto vision = cJSON_GetObjectItem(capabilities, "vision");
    if (cJSON_IsObject(vision)) {
        auto url = cJSON_GetObjectItem(vision, "url");
        auto token = cJSON_GetObjectItem(vision, "token");
        if (cJSON_IsString(url)) {
            auto camera = Board::GetInstance().GetCamera();
            if (camera) {
                std::string url_str = std::string(url->valuestring);
                std::string token_str;
                if (cJSON_IsString(token)) {
                    token_str = std::string(token->valuestring);
                }
                camera->SetExplainUrl(url_str, token_str);
            }
        }
    }
}

void McpServer::ParseMessage(const cJSON* json) {
    // Check JSONRPC version
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0) {
        ESP_LOGE(TAG, "Invalid JSONRPC version: %s", version ? version->valuestring : "null");
        return;
    }
    
    // Check method
    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method)) {
        ESP_LOGE(TAG, "Missing method");
        return;
    }
    
    auto method_str = std::string(method->valuestring);
    if (method_str.find("notifications") == 0) {
        return;
    }
    
    // Check params
    auto params = cJSON_GetObjectItem(json, "params");
    if (params != nullptr && !cJSON_IsObject(params)) {
        ESP_LOGE(TAG, "Invalid params for method: %s", method_str.c_str());
        return;
    }

    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id)) {
        ESP_LOGE(TAG, "Invalid id for method: %s", method_str.c_str());
        return;
    }
    auto id_int = id->valueint;
    
    if (method_str == "initialize") {
        if (cJSON_IsObject(params)) {
            auto capabilities = cJSON_GetObjectItem(params, "capabilities");
            if (cJSON_IsObject(capabilities)) {
                ParseCapabilities(capabilities);
            }
        }
        auto app_desc = esp_app_get_description();
        std::string message = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"" BOARD_NAME "\",\"version\":\"";
        message += app_desc->version;
        message += "\"}}";
        ReplyResult(id_int, message);
    } else if (method_str == "tools/list") {
        std::string cursor_str = "";
        bool list_user_only_tools = false;
        if (params != nullptr) {
            auto cursor = cJSON_GetObjectItem(params, "cursor");
            if (cJSON_IsString(cursor)) {
                cursor_str = std::string(cursor->valuestring);
            }
            auto with_user_tools = cJSON_GetObjectItem(params, "withUserTools");
            if (cJSON_IsBool(with_user_tools)) {
                list_user_only_tools = with_user_tools->valueint == 1;
            }
        }
        GetToolsList(id_int, cursor_str, list_user_only_tools);
    } else if (method_str == "tools/call") {
        if (!cJSON_IsObject(params)) {
            ESP_LOGE(TAG, "tools/call: Missing params");
            ReplyError(id_int, "Missing params");
            return;
        }
        auto tool_name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(tool_name)) {
            ESP_LOGE(TAG, "tools/call: Missing name");
            ReplyError(id_int, "Missing name");
            return;
        }
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments)) {
            ESP_LOGE(TAG, "tools/call: Invalid arguments");
            ReplyError(id_int, "Invalid arguments");
            return;
        }
        DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments);
    } else {
        ESP_LOGE(TAG, "Method not implemented: %s", method_str.c_str());
        ReplyError(id_int, "Method not implemented: " + method_str);
    }
}

void McpServer::ReplyResult(int id, const std::string& result) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::ReplyError(int id, const std::string& message) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"error\":{\"message\":\"";
    payload += message;
    payload += "\"}}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::GetToolsList(int id, const std::string& cursor, bool list_user_only_tools) {
    const int max_payload_size = 8000;
    std::string json = "{\"tools\":[";
    
    bool found_cursor = cursor.empty();
    auto it = tools_.begin();
    std::string next_cursor = "";
    
    while (it != tools_.end()) {
        // 如果我们还没有找到起始位置，继续搜索
        if (!found_cursor) {
            if ((*it)->name() == cursor) {
                found_cursor = true;
            } else {
                ++it;
                continue;
            }
        }

        if (!list_user_only_tools && (*it)->user_only()) {
            ++it;
            continue;
        }
        
        // 添加tool前检查大小
        std::string tool_json = (*it)->to_json() + ",";
        if (json.length() + tool_json.length() + 30 > max_payload_size) {
            // 如果添加这个tool会超出大小限制，设置next_cursor并退出循环
            next_cursor = (*it)->name();
            break;
        }
        
        json += tool_json;
        ++it;
    }
    
    if (json.back() == ',') {
        json.pop_back();
    }
    
    if (json.back() == '[' && !tools_.empty()) {
        // 如果没有添加任何tool，返回错误
        ESP_LOGE(TAG, "tools/list: Failed to add tool %s because of payload size limit", next_cursor.c_str());
        ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit");
        return;
    }

    if (next_cursor.empty()) {
        json += "]}";
    } else {
        json += "],\"nextCursor\":\"" + next_cursor + "\"}";
    }
    
    ReplyResult(id, json);
}

void McpServer::DoToolCall(int id, const std::string& tool_name, const cJSON* tool_arguments) {
    auto tool_iter = std::find_if(tools_.begin(), tools_.end(), 
                                 [&tool_name](const McpTool* tool) { 
                                     return tool->name() == tool_name; 
                                 });
    
    if (tool_iter == tools_.end()) {
        ESP_LOGE(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
        ReplyError(id, "Unknown tool: " + tool_name);
        return;
    }

    PropertyList arguments = (*tool_iter)->properties();
    try {
        for (auto& argument : arguments) {
            bool found = false;
            if (cJSON_IsObject(tool_arguments)) {
                auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
                if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                    argument.set_value<bool>(value->valueint == 1);
                    found = true;
                } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value)) {
                    argument.set_value<int>(value->valueint);
                    found = true;
                } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                    argument.set_value<std::string>(value->valuestring);
                    found = true;
                }
            }

            if (!argument.has_default_value() && !found) {
                ESP_LOGE(TAG, "tools/call: Missing valid argument: %s", argument.name().c_str());
                ReplyError(id, "Missing valid argument: " + argument.name());
                return;
            }
        }
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "tools/call: %s", e.what());
        ReplyError(id, e.what());
        return;
    }

    if (!(*tool_iter)->requires_main_thread()) {
        auto* tool = *tool_iter;
        {
            ScopedThreadConfig thread_cfg("mcp_tool", 20480, 4);
            std::thread([this, id, tool, arguments = std::move(arguments)]() mutable {
                try {
                    ReplyResult(id, tool->Call(arguments));
                } catch (const std::exception& e) {
                    ESP_LOGE(TAG, "tools/call async: %s", e.what());
                    ReplyError(id, e.what());
                }
            }).detach();
        }
        return;
    }

    // Use main thread to call the tool
    auto& app = Application::GetInstance();
    app.Schedule([this, id, tool_iter, arguments = std::move(arguments)]() {
        try {
            ReplyResult(id, (*tool_iter)->Call(arguments));
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "tools/call: %s", e.what());
            ReplyError(id, e.what());
        }
    });
}
