#include "Speaking.h"
#include "../../Utils/user_log.h"
#include "../../Events/AppEvents.h"
#include "../../third_party/snowboy/include/snowboy-detect-c-wrapper.h"
#include <chrono>
#include <cmath>
#include <cstdlib>

// 静态成员变量定义
std::atomic<bool> SpeakingState::state_running_{false};
std::thread SpeakingState::state_running_thread_;
std::atomic<bool> SpeakingState::interrupt_running_{false};
std::thread SpeakingState::interrupt_thread_;
std::atomic<long long> SpeakingState::start_tick_ms_{0};
std::atomic<bool> SpeakingState::wake_response_pending_{false};
std::atomic<long long> SpeakingState::guard_ms_{0};

void SpeakingState::MarkWakeResponse() {
    wake_response_pending_.store(true);
}

/*
 * 打断(barge-in)的误触发治理
 * --------------------------
 * 板子上没有回声消除(AEC), 麦克风会收到扬声器里机器人自己的声音, 而唤醒词
 * 「小农同学」又和日常播报内容(例如"江西农业大学")高度重叠, 所以早期版本
 * "命中即打断"会频繁自己打断自己。这里用四道闸门, 全部可用环境变量调:
 *
 *   BARGE_ENABLE=1          设 0 彻底关闭打断(现场演示时救命用)
 *   BARGE_SENSITIVITY=0.7   播报期灵敏度, 默认与空闲态(Idle.cc)一致;
 *                           误触发多就往下降(0.6/0.5), 打断不了就别再降
 *   BARGE_GUARD_MS=1500       唤醒应答轮(idle->speaking, 播 waked.pcm, 实测 1.12s)
 *                             的保护窗口; 期间播放队列一出现空档就提前放行
 *   BARGE_REPLY_GUARD_MS=200  回复轮(thinking->speaking, 真正的 TTS)的窗口。
 *                             这里必须很小: 回复音频是连续流入的, 队列几乎不会空,
 *                             若按唤醒应答那种秒级窗口挡着, 整轮回复都打断不了
 *   BARGE_GAIN_RATIO=0      命中时要求"当前音量"高于"播放基线×倍率"以区分回声;
 *                           默认 0=关闭(机器人自己很大声时该判据容易误杀真人插话),
 *                           确认误触发来自外放回声再设 1.3~1.6
 *   BARGE_HITS=1            需要连续几次命中才真打断(误触发仍多就设 2)
 *   BARGE_HIT_WINDOW_MS=1200 多次命中的最大间隔
 *   BARGE_COOLDOWN_MS=2000  忽略一次命中后的静默期, 防止同一句话反复命中
 */
static long long _env_ll(const char *name, long long dflt)
{
    const char *v = getenv(name);
    if (!v || !*v) return dflt;
    char *end = nullptr;
    long long r = strtoll(v, &end, 10);
    return (end && end != v) ? r : dflt;
}

static double _env_dbl(const char *name, double dflt)
{
    const char *v = getenv(name);
    if (!v || !*v) return dflt;
    char *end = nullptr;
    double r = strtod(v, &end);
    return (end && end != v) ? r : dflt;
}

static std::string _env_str(const char *name, const char *dflt)
{
    const char *v = getenv(name);
    return (v && *v) ? std::string(v) : std::string(dflt);
}

static long long _now_ms(void) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

/* 一帧 int16 PCM 的 RMS 能量(归一化到 0~1) */
static double _frame_rms(const std::vector<int16_t> &data)
{
    if (data.empty()) return 0.0;
    double acc = 0.0;
    for (size_t i = 0; i < data.size(); i++) {
        double s = static_cast<double>(data[i]) / 32768.0;
        acc += s * s;
    }
    return sqrt(acc / static_cast<double>(data.size()));
}

void SpeakingState::Enter(Application* app) {
    std::string json_message = R"({"type": "state", "state": "speaking"})";
    app->ws_client_.SendText(json_message);
    // start播放
    app->audio_processor_.startPlaying();
    // 打断需要: 播报期间麦克风保持采集, 并另起一线程做唤醒词检测
    if (_env_ll("BARGE_ENABLE", 1) != 0) {
        const bool wake_round = wake_response_pending_.exchange(false);
        guard_ms_.store(_env_ll(wake_round ? "BARGE_GUARD_MS" : "BARGE_REPLY_GUARD_MS",
                                wake_round ? 1500 : 200));
        app->audio_processor_.startRecording();
        start_tick_ms_.store(_now_ms());
        USER_LOG_INFO("Barge armed: round=%s guard_ms=%lld",
                      wake_round ? "wake-response" : "reply", guard_ms_.load());
        interrupt_running_.store(true);
        interrupt_thread_ = std::thread([app]() { RunInterruptDetection(app); });
    }
    // running
    state_running_.store(true);
    state_running_thread_ = std::thread([app]() { Run(app); });
    USER_LOG_INFO("Into speaking state.");
}

void SpeakingState::Run(Application* app) {
    USER_LOG_INFO("Speaking state run.");
    while(state_running_.load() == true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if(app->get_tts_completed() && app->audio_processor_.playbackQueueIsEmpty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            USER_LOG_INFO("Speaking end.");
            if(app->get_dialogue_completed() == false) {
                app->eventQueue_.Enqueue(static_cast<int>(AppEvent::speaking_end));
            } else {
                app->eventQueue_.Enqueue(static_cast<int>(AppEvent::dialogue_end));
            }
            app->set_tts_completed(false);
            app->set_dialogue_completed(false);
            break;
        }
    }
}

void SpeakingState::RunInterruptDetection(Application* app) {
    const long long guard_ms   = guard_ms_.load();
    const long long hit_window = _env_ll("BARGE_HIT_WINDOW_MS", 1200);
    const long long cooldown   = _env_ll("BARGE_COOLDOWN_MS", 2000);
    const double gain_ratio    = _env_dbl("BARGE_GAIN_RATIO", 0);   // 0 = 关闭这道闸门
    const int hits_required    = static_cast<int>(_env_ll("BARGE_HITS", 1));
    const std::string sens     = _env_str("BARGE_SENSITIVITY", "0.7");

    std::string banner = std::string("Interrupt detection run. sensitivity=") + sens +
                  " guard_ms=" + std::to_string(guard_ms) +
                  " hits=" + std::to_string(hits_required) +
                  " gain_ratio=" + std::to_string(gain_ratio);
    USER_LOG_INFO("%s", banner.c_str());

    SnowboyDetect* detector = SnowboyDetectConstructor("third_party/snowboy/resources/common.res",
                                                       "third_party/snowboy/resources/models/xiaonong.pmdl");
    SnowboyDetectSetSensitivity(detector, sens.c_str());
    SnowboyDetectSetAudioGain(detector, 1);
    SnowboyDetectApplyFrontend(detector, false);

    /* 闸门一: 唤醒应答(waked.pcm)还在播时完全不判定。
     *         播放队列一旦空出档, 说明应答放完, 提前放行。 */
    const long long guard_until = start_tick_ms_.load() + guard_ms;
    while (interrupt_running_.load() == true && _now_ms() < guard_until) {
        if (app->audio_processor_.playbackQueueIsEmpty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!interrupt_running_.load()) {
        SnowboyDetectDestructor(detector);
        USER_LOG_INFO("Interrupt detection stopped.");
        return;
    }
    USER_LOG_INFO("Barge guard ended after %lldms (waited_for_gap=%d).",
                  _now_ms() - start_tick_ms_.load(),
                  app->audio_processor_.playbackQueueIsEmpty() ? 1 : 0);
    /* 把窗口期积压的音频丢掉, 免得把"我在"喂进检测器 */
    app->audio_processor_.clearRecordedAudioQueue();

    double baseline = 0.0;      // 播放期音量基线(≈外放回声强度)
    double recent = 0.0;        // 当前音量
    long long cooldown_until = 0;
    int hits = 0;
    long long first_hit_ms = 0;
    std::vector<int16_t> data;

    // 诊断计数: 到底有没有拿到音频、有没有命中、被哪道闸门拦下
    const bool stats_on = (_env_ll("BARGE_STATS", 0) != 0);
    long long frames = 0, samples = 0, raw_hits = 0, ignored_echo = 0, ignored_cd = 0;
    long long next_stats_ms = _now_ms() + 1000;

    while (interrupt_running_.load() == true) {
        if (app->audio_processor_.recordedQueueIsEmpty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (!app->audio_processor_.getRecordedAudio(data)) continue;
        frames++; samples += (long long)data.size();
        double rms = _frame_rms(data);

        if (stats_on && _now_ms() >= next_stats_ms) {
            next_stats_ms = _now_ms() + 1000;
            USER_LOG_INFO("BARGE stats frames=%lld pcm=%lld rms=%.4f recent=%.4f "
                          "baseline=%.4f raw_hits=%lld ignored(echo=%lld cooldown=%lld)",
                          frames, samples, rms, recent, baseline, raw_hits,
                          ignored_echo, ignored_cd);
        }

        recent   += 0.15 * (rms - recent);   // 快: 约 260ms 时间常数, 不被单个音节峰值带跑
        baseline += 0.02 * (rms - baseline);   // 慢: 约 2s, 代表外放回声的平均强度

        if (SnowboyDetectRunDetection(detector, data.data(), data.size(), false) <= 0)
            continue;

        /* 命中了, 但先别当真: 逐条闸门筛 */
        SnowboyDetectReset(detector);
        raw_hits++;
        long long now = _now_ms();

        if (now < cooldown_until) {
            ignored_cd++;
            USER_LOG_INFO("Barge hit ignored (cooldown).");
            continue;
        }

        /* 闸门二: 外放回声的音量和基线差不多, 真人近讲要明显更响 */
        if (gain_ratio > 0.0 && baseline > 0.002 && recent < baseline * gain_ratio) {
            cooldown_until = now + cooldown;
            ignored_echo++;
            hits = 0;
            std::string why = "Barge hit ignored (looks like echo): recent=" +
                              std::to_string(recent) + " baseline=" + std::to_string(baseline);
            USER_LOG_INFO("%s", why.c_str());
            continue;
        }

        /* 闸门三: 需要连续多次命中(BARGE_HITS=2 时启用) */
        if (hits_required > 1) {
            if (hits == 0 || now - first_hit_ms > hit_window) {
                hits = 1;
                first_hit_ms = now;
                USER_LOG_INFO("Barge hit 1/%d, waiting confirmation.", hits_required);
                continue;
            }
            hits++;
            if (hits < hits_required) {
                USER_LOG_INFO("Barge hit %d/%d.", hits, hits_required);
                continue;
            }
        }

        /* 判定成立: 立刻静音, 通知服务端停止本轮, 转 listening */
        std::string go = "Wake detected while speaking, interrupting. recent=" +
                         std::to_string(recent) + " baseline=" + std::to_string(baseline);
        USER_LOG_INFO("%s", go.c_str());
        app->audio_processor_.clearPlaybackAudioQueue();
        app->ws_client_.SendText(R"({"type": "interrupt"})");
        app->eventQueue_.Enqueue(static_cast<int>(AppEvent::wake_detected));
        break;
    }

    USER_LOG_INFO("Interrupt detection stopped. frames=%lld pcm=%lld raw_hits=%lld "
                  "ignored(echo=%lld cooldown=%lld)",
                  frames, samples, raw_hits, ignored_echo, ignored_cd);
    SnowboyDetectDestructor(detector);
}

void SpeakingState::Exit(Application* app) {
    // stop打断检测
    interrupt_running_.store(false);
    if (interrupt_thread_.joinable()) {
        interrupt_thread_.join();
    }
    // clear playback audio queue
    app->audio_processor_.clearPlaybackAudioQueue();
    // 复位播报完成标志, 否则下一轮 speaking 可能被上一轮的残留状态提前结束
    app->set_tts_completed(false);
    // stop播放
    app->audio_processor_.stopPlaying();
    // stop录音
    app->audio_processor_.stopRecording();
    // stop state running
    state_running_.store(false);
    state_running_thread_.join();
    USER_LOG_INFO("Speaking exit.");
}
