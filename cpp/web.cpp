// 本地浏览器控制台：C++ 提供 HTTP、状态缓存、视频预览和控制接口。
// 设备业务仍调用 core.cpp 的 execute()，不在网页端重复实现相机协议。
#include "core.hpp"
#include <httplib.h>
#include <algorithm>
#include <condition_variable>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <poll.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using zt6::Bytes;
using zt6::Json;
static volatile sig_atomic_t ending = 0;
static void cancel(int) {
    ending = 1;
}

// 仅在一次响应存活期间占用名额；大视频使用固定大小块转发，不存整文件。
struct DownloadPermit {
    std::atomic<int> &active;
    explicit DownloadPermit(std::atomic<int> &value) : active(value) {}
    ~DownloadPermit() {
        --active;
    }
};

static std::unique_ptr<httplib::Client> media_client(const zt6::Config &config) {
    auto client = std::make_unique<httplib::Client>(config.host, config.media_port);
    client->set_connection_timeout(std::chrono::duration<double>(config.timeout));
    // 下载不设置整文件时限，只限制连接及连续无数据等待。
    client->set_read_timeout(15);
    return client;
}

static void sd_download(const zt6::Config &config, const httplib::Request &req,
                        httplib::Response &res, std::shared_ptr<DownloadPermit> permit) {
    auto file = zt6::sd_media_file(config, req.get_param_value("url"));
    if (req.ranges.size() > 1) {
        res.status = 416; // 浏览器正常下载/续传使用单段范围。
        return;
    }
    auto client = media_client(config);
    auto head = client->Head(file.path);
    if (!head) {
        res.status = 502;
        res.set_content(
            Json{{"error", "无法读取 SD 文件信息：" + httplib::to_string(head.error())}}.dump(),
            "application/json");
        return;
    }
    if (head->status != 200) {
        res.status = head->status == 404 ? 404 : 502;
        res.set_content(Json{{"error", "相机文件接口 HTTP " + std::to_string(head->status)}}.dump(),
                        "application/json");
        return;
    }
    const auto raw_length = head->get_header_value("Content-Length");
    if (raw_length.empty() || raw_length.find_first_not_of("0123456789") != std::string::npos)
        throw zt6::Error("相机未提供有效文件大小，无法下载");
    const auto length = std::stoull(raw_length);
    if (!length)
        throw zt6::Error("SD 文件为空，请等待写入完成后再下载");
    res.set_header("Content-Disposition", "attachment; filename=\"" + file.name + "\"");
    res.set_header("Accept-Ranges", "bytes");
    // httplib 根据浏览器单段 Range 自动生成 206 和 Content-Range，并传入 offset/count。
    res.set_content_provider(
        size_t(length), file.content_type,
        [config, file, length, permit](size_t offset, size_t count, httplib::DataSink &sink) {
            auto source = media_client(config);
            httplib::Headers headers;
            if (offset || count != length)
                headers.emplace("Range", "bytes=" + std::to_string(offset) + "-" +
                                             std::to_string(offset + count - 1));
            size_t skip = 0, remaining = count;
            bool accepted = false, failed = false;
            auto result = source->Get(
                file.path, headers,
                [&](const httplib::Response &response) {
                    if (response.status == 206) {
                        const auto expected = "bytes " + std::to_string(offset) + "-" +
                                              std::to_string(offset + count - 1) + "/" +
                                              std::to_string(length);
                        accepted = response.get_header_value("Content-Range") == expected;
                    } else if (response.status == 200) {
                        // 旧相机可能忽略 Range：丢弃前缀，只转发所需区间，内存仍有界。
                        skip = offset;
                        accepted =
                            response.get_header_value("Content-Length") == std::to_string(length);
                    }
                    return accepted;
                },
                [&](const char *data, size_t bytes) {
                    if (ending || (sink.is_writable && !sink.is_writable())) {
                        failed = true;
                        return false;
                    }
                    const auto discard = std::min(skip, bytes);
                    skip -= discard;
                    data += discard;
                    bytes -= discard;
                    const auto send = std::min(bytes, remaining);
                    if (send && !sink.write(data, send)) {
                        failed = true;
                        return false;
                    }
                    remaining -= send;
                    // 收齐后取消上游读取；不缓存、不读入后续无关内容。
                    return remaining != 0;
                });
            (void)result; // 收齐时主动取消会返回 Canceled；用实际字节数判定成功。
            return accepted && !failed && remaining == 0;
        });
}

// 所有浏览器共用一条解码管线，只保留最新 JPEG，慢客户端不会积压旧画面。
class Preview {
    zt6::Config config_;
    int fps_, width_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::thread worker_;
    bool closing_ = false;
    int clients_ = 0, pid_ = -1;
    uint64_t sequence_ = 0, generation_ = 0;
    double frame_time_ = 0;
    std::shared_ptr<std::string> frame_;
    std::string error_ = "等待浏览器连接";
    void work() {
        for (;;) {
            uint64_t generation;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                changed_.wait(lock, [&] { return closing_ || clients_ > 0; });
                if (closing_)
                    return;
                generation = generation_;
            }
            int output[2]{-1, -1};
            int log = -1;
            int child = -1;
            try {
                auto command = zt6::ffmpeg_input(config_);
                std::vector<std::string> tail{"-map",
                                              "0:v:0",
                                              "-an",
                                              "-vf",
                                              "fps=" + std::to_string(fps_) +
                                                  ",scale=" + std::to_string(width_) + ":-2",
                                              "-q:v",
                                              "5",
                                              "-threads",
                                              "2",
                                              "-f",
                                              "image2pipe",
                                              "-vcodec",
                                              "mjpeg",
                                              "pipe:1"};
                command.insert(command.end(), tail.begin(), tail.end());
                if (pipe2(output, O_CLOEXEC))
                    throw zt6::Error("预览管道创建失败");
                log = open((config_.state_dir / "preview.ffmpeg.log").c_str(),
                           O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
                if (log < 0)
                    throw zt6::Error("预览日志创建失败");
                child = zt6::spawn_process(command, output[1], log);
                ::close(output[1]);
                output[1] = -1;
                ::close(log);
                log = -1;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    pid_ = child;
                    error_ = "正在等待视频关键帧";
                }
                Bytes buffer;
                double idle = 0;
                for (;;) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (closing_ || generation != generation_)
                            break;
                        if (!clients_) {
                            if (!idle)
                                idle = zt6::monotonic();
                            if (zt6::monotonic() - idle > 5)
                                break;
                        } else
                            idle = 0;
                    }
                    pollfd fd{output[0], POLLIN, 0};
                    int ready = poll(&fd, 1, 200);
                    if (ready < 0 && errno == EINTR)
                        continue;
                    if (ready < 0)
                        break;
                    if (!ready)
                        continue;
                    uint8_t bytes[16384];
                    auto n = read(output[0], bytes, sizeof(bytes));
                    if (n <= 0)
                        break;
                    buffer.insert(buffer.end(), bytes, bytes + n);
                    // FFmpeg image2pipe 连续输出 JPEG；SOI/EOI 识别每帧边界。
                    for (;;) {
                        const Bytes start{0xff, 0xd8}, finish{0xff, 0xd9};
                        auto first =
                            std::search(buffer.begin(), buffer.end(), start.begin(), start.end());
                        if (first == buffer.end()) {
                            if (buffer.size() > 4 * 1024 * 1024)
                                buffer.clear();
                            break;
                        }
                        auto last =
                            std::search(first + 2, buffer.end(), finish.begin(), finish.end());
                        if (last == buffer.end()) {
                            if (buffer.size() > 4 * 1024 * 1024)
                                buffer.clear();
                            break;
                        }
                        auto jpeg = std::make_shared<std::string>(first, last + 2);
                        buffer.erase(buffer.begin(), last + 2);
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (generation == generation_) {
                            frame_ = jpeg;
                            frame_time_ = zt6::monotonic();
                            ++sequence_;
                            error_.clear();
                            changed_.notify_all();
                        }
                    }
                }
            } catch (const std::exception &e) {
                std::lock_guard<std::mutex> lock(mutex_);
                error_ = e.what();
            }
            {
                // 先撤销可被 restart() 看见的 PID，再回收子进程，避免 PID 复用后误发信号。
                std::lock_guard<std::mutex> lock(mutex_);
                pid_ = -1;
                if (!closing_ && generation == generation_ && clients_ > 0)
                    error_ = "视频暂时中断，正在重连";
                if (!clients_)
                    frame_.reset();
            }
            if (output[0] >= 0)
                ::close(output[0]);
            if (output[1] >= 0)
                ::close(output[1]);
            if (log >= 0)
                ::close(log);
            if (child > 0)
                zt6::finish_process(child);
            std::unique_lock<std::mutex> lock(mutex_);
            if (closing_)
                return;
            changed_.wait_for(lock, std::chrono::milliseconds(400));
        }
    }

  public:
    Preview(const zt6::Config &config, int fps, int width)
        : config_(config), fps_(fps), width_(width) {
        // 所有成员初始化后再启动线程，避免线程读到未初始化的状态。
        worker_ = std::thread([this] { work(); });
    }
    ~Preview() {
        close();
    }
    bool acquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closing_ || clients_ >= 4)
            return false;
        ++clients_;
        changed_.notify_all();
        return true;
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (clients_ > 0)
            --clients_;
        changed_.notify_all();
    }
    void restart() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        frame_.reset();
        if (pid_ > 0)
            kill(pid_, SIGTERM);
        changed_.notify_all();
    }
    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closing_)
                return;
            closing_ = true;
            if (pid_ > 0)
                kill(pid_, SIGTERM);
            changed_.notify_all();
        }
        if (worker_.joinable())
            worker_.join();
    }
    Json info() {
        std::lock_guard<std::mutex> lock(mutex_);
        return {{"sequence", sequence_},
                {"clients", clients_},
                {"fps_limit", fps_},
                {"width_limit", width_},
                {"age_seconds", frame_time_ ? Json(zt6::monotonic() - frame_time_) : Json(nullptr)},
                {"error", error_}};
    }
    std::pair<uint64_t, std::shared_ptr<std::string>> wait(uint64_t previous, double timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        double end = zt6::monotonic() + timeout;
        while (!closing_) {
            if (frame_ && sequence_ != previous && zt6::monotonic() - frame_time_ < 5)
                return {sequence_, frame_};
            double remain = end - zt6::monotonic();
            if (remain <= 0)
                break;
            changed_.wait_for(lock, std::chrono::duration<double>(remain));
        }
        return {0, nullptr};
    }
};

// HTTP 请求携带会话随机令牌，拒绝来自其他网页的跨站控制请求。
static std::string make_token() {
    std::ifstream random("/dev/urandom", std::ios::binary);
    Bytes b(32);
    if (!random.read(reinterpret_cast<char *>(b.data()), b.size()))
        throw zt6::Error("无法生成会话令牌");
    return zt6::hex(b);
}
static void reply(httplib::Response &response, const Json &value, int status = 200) {
    response.status = status;
    auto payload = value;
    if (status >= 400 && payload.is_object()) {
        payload["success"] = false;
        payload["status"] = "error";
        if (payload.contains("error"))
            payload["message"] = payload["error"];
    }
    response.set_content(payload.dump(), "application/json; charset=utf-8");
}
static Json action_request(const Json &data) {
    std::string action = data.value("action", "");
    std::string storage = data.value("storage", "onboard");
    if (storage != "sd" && storage != "onboard")
        throw zt6::Error("未知保存位置");
    Json request;
    if (action == "photo")
        request = {
            {"command", "photo"}, {"storage", storage}, {"count", data.value("count", Json(1))}};
    else if (action == "record_start" || action == "record_stop")
        request = {{"command", "record"},
                   {"action", action == "record_start" ? "start" : "stop"},
                   {"storage", storage}};
    else if (action == "storage")
        request = {{"command", "storage"}, {"destination", storage}};
    else if (action == "center" || action == "stop")
        request = {{"command", action}};
    else if (action == "angle")
        request = {{"command", "angle"},
                   {"yaw", zt6::number(data.at("yaw"), -270, 270, "yaw")},
                   {"pitch", zt6::number(data.at("pitch"), -90, 25, "pitch")}};
    else if (action == "move")
        request = {{"command", "move"},
                   {"yaw", zt6::number(data.value("yaw", Json(0)), -40, 40, "yaw")},
                   {"pitch", zt6::number(data.value("pitch", Json(0)), -40, 40, "pitch")},
                   {"seconds", .18}};
    else if (action == "view")
        request = {{"command", "view"}, {"source", data.at("source")}};
    else if (action == "auto_record")
        request = {{"command", "auto-record"}, {"action", data.value("value", "status")}};
    else if (action == "resolution") {
        request = {{"command", "resolution"}, {"stream", data.value("stream", "main")}};
        for (auto key : {"width", "height", "codec", "bitrate_kbps", "reapply"})
            if (data.contains(key))
                request[key] = data[key];
    } else if (action == "gimbal_mode" || action == "thermal_output")
        request = {{"command", action == "gimbal_mode" ? "gimbal-mode" : "thermal-output"},
                   {"mode", data.value("mode", "status")}};
    else if (action == "zoom")
        request = {{"command", "zoom"}, {"ratio", data.value("ratio", Json(nullptr))}};
    else if (action == "zoom_link")
        request = {{"command", "zoom-link"}, {"action", data.value("value", "status")}};
    else if (action == "thermal_frame")
        request = {{"command", "thermal-frame"}};
    else
        throw zt6::Error("未知操作");
    if (action == "angle" || action == "center") {
        request["wait_seconds"] =
            zt6::number(data.value("wait_seconds", Json(10)), .1, 20, "wait-seconds");
        request["tolerance"] = zt6::number(data.value("tolerance", Json(1)), .1, 5, "tolerance");
    }
    return request;
}

class App {
    std::mutex mutex_;
    Json cache_{{"connected", false}, {"error", "正在连接云台"}};
    std::atomic<bool> closing_{false};
    std::thread poller_;

  public:
    zt6::Config config;
    Preview preview;
    std::string token;
    std::atomic<int> sd_downloads{0};
    App(const zt6::Config &c, int fps, int width)
        : config(c), preview(c, fps, width), token(make_token()) {
        poller_ = std::thread([this] {
            while (!closing_) {
                try {
                    // 轮询与交互仍串行访问相机，但轮询每条 SDK 查询最多 0.5 秒。
                    // 遇到控制锁立即跳过，避免后台查询长时间挡住独立 CLI 操作。
                    auto polling_config = config;
                    polling_config.timeout = std::min(config.timeout, .5);
                    auto state = zt6::execute(
                        polling_config,
                        {{"command", "status"}, {"full", true}, {"background_poll", true}});
                    state["connected"] = !state.contains("sdk_error");
                    state["busy"] = false;
                    state["updated_at"] = zt6::monotonic();
                    std::lock_guard<std::mutex> lock(mutex_);
                    cache_ = state;
                } catch (const std::exception &e) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    cache_["busy"] = true;
                    cache_["error"] = e.what();
                }
                for (int i = 0; i < 10 && !closing_; ++i)
                    zt6::pause(.1);
            }
        });
    }
    ~App() {
        close();
    }
    void close() {
        if (closing_.exchange(true))
            return;
        preview.close();
        if (poller_.joinable())
            poller_.join();
    }
    Json status() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto result = cache_;
        double age = result.contains("updated_at")
                         ? zt6::monotonic() - result["updated_at"].get<double>()
                         : 9999;
        result["status_age_seconds"] = age;
        if (age > 8)
            result["connected"] = false;
        result["preview"] = preview.info();
        result["version"] = "zt6-cpp-2.0";
        result["camera_host"] = config.host;
        result["zoom_enabled"] = zt6::zoom_controls_enabled;
        result["output_dir"] = config.output_dir.string();
        return result;
    }
};

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, cancel);
    signal(SIGTERM, cancel);
    signal(SIGHUP, cancel);
    try {
        zt6::Config config;
        int port = 18765, fps = 8, width = 960;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            auto val = [&]() {
                if (++i >= argc)
                    throw zt6::Error("缺少参数：" + arg);
                return std::string(argv[i]);
            };
            if (arg == "--host")
                config.host = val();
            else if (arg == "--camera-port")
                config.port = std::stoi(val());
            else if (arg == "--media-port")
                config.media_port = std::stoi(val());
            else if (arg == "--port")
                port = std::stoi(val());
            else if (arg == "--timeout")
                config.timeout = std::stod(val());
            else if (arg == "--fps")
                fps = std::stoi(val());
            else if (arg == "--width")
                width = std::stoi(val());
            else if (arg == "--rtsp")
                config.rtsp = val();
            else if (arg == "--state-dir")
                config.state_dir = zt6::fs::absolute(val());
            else if (arg == "--output-dir")
                config.output_dir = zt6::fs::absolute(val());
            else if (arg == "--help") {
                std::cout
                    << "zt6-web [--port 18765] [--host 192.168.144.25] [--fps 8] [--width "
                       "960]\n仅监听 127.0.0.1；从电脑使用 scripts/start_web.sh 建立 SSH 隧道。\n";
                return 0;
            } else
                throw zt6::Error("未知参数：" + arg);
        }
        config.validate();
        if (port < 1024 || port > 65535 || fps < 1 || fps > 15 ||
            (width != 640 && width != 960 && width != 1280))
            throw zt6::Error("HTTP端口/预览帧率/宽度无效");
        zt6::fs::create_directories(config.state_dir);
        App app(config, fps, width);
        httplib::Server server;
        server.new_task_queue = []() { return new httplib::ThreadPool(12, 32); };
        server.set_payload_max_length(4096);
        server.set_read_timeout(5);
        server.set_write_timeout(5);
        server.set_default_headers(
            {{"Cache-Control", "no-store"},
             {"X-Content-Type-Options", "nosniff"},
             {"X-Frame-Options", "DENY"},
             {"Referrer-Policy", "no-referrer"},
             {"Content-Security-Policy",
              "default-src 'self'; img-src 'self' data:; script-src 'self'; style-src 'self'; "
              "connect-src 'self'; frame-ancestors 'none'"}});
        server.set_pre_routing_handler([&](const httplib::Request &req, httplib::Response &res) {
            auto host = req.get_header_value("Host");
            auto colon = host.find(':');
            auto name = host.substr(0, colon);
            if (name != "127.0.0.1" && name != "localhost") {
                reply(res, {{"error", "仅允许 localhost 访问"}}, 403);
                return httplib::Server::HandlerResponse::Handled;
            }
            if (req.method == "POST" && req.get_header_value("X-ZT6-Token") != app.token) {
                reply(res, {{"error", "会话已失效，请刷新页面"}}, 403);
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });
        server.set_exception_handler([](const auto &, auto &res, std::exception_ptr ptr) {
            try {
                if (ptr)
                    std::rethrow_exception(ptr);
            } catch (const zt6::PhotoBurstError &e) {
                reply(res, e.result, 400);
            } catch (const std::exception &e) {
                reply(res, {{"error", e.what()}}, 400);
            }
        });
        for (auto asset : std::vector<std::pair<std::string, std::string>>{
                 {"/", "index.html"}, {"/app.js", "app.js"}, {"/style.css", "style.css"}}) {
            server.Get(asset.first, [&, file = asset.second](const auto &, auto &res) {
                auto path = config.workspace / "web" / file;
                if (!zt6::fs::is_regular_file(path)) {
                    reply(res, {{"error", "页面资源不存在，请检查部署目录"}}, 404);
                    return;
                }
                res.set_file_content(path.string(),
                                     file == "index.html" ? "text/html; charset=utf-8"
                                     : file == "app.js"   ? "text/javascript; charset=utf-8"
                                                          : "text/css; charset=utf-8");
            });
        }
        server.Get("/api/health",
                   [&](const auto &, auto &res) { reply(res, {{"service", "zt6-cpp-2.0"}}); });
        server.Get("/api/session", [&](const auto &, auto &res) {
            reply(res, {{"token", app.token}, {"version", "zt6-cpp-2.0"}});
        });
        server.Get("/api/status", [&](const auto &, auto &res) { reply(res, app.status()); });
        server.Get("/api/files", [&](const auto &req, auto &res) {
            reply(res, zt6::execute(
                           config,
                           {{"command", "files"},
                            {"storage",
                             req.has_param("storage") ? req.get_param_value("storage") : "onboard"},
                            {"kind", req.has_param("kind") ? req.get_param_value("kind") : "photo"},
                            {"count", 30}}));
        });
        server.Get("/api/download", [&](const auto &req, auto &res) {
            const auto storage =
                req.has_param("storage") ? req.get_param_value("storage") : "onboard";
            if (storage == "sd") {
                if (app.sd_downloads.fetch_add(1) >= 2) {
                    --app.sd_downloads;
                    reply(res, {{"error", "同时最多下载两个 SD 文件，请稍后重试"}}, 429);
                    return;
                }
                auto permit = std::make_shared<DownloadPermit>(app.sd_downloads);
                sd_download(config, req, res, permit);
                return;
            }
            if (storage != "onboard")
                throw zt6::Error("未知下载位置");
            std::string name = req.get_param_value("file");
            zt6::fs::path path(name);
            if (name.find_first_not_of(
                    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") !=
                    std::string::npos ||
                path.filename() != path || name.rfind("zt6_", 0) != 0 ||
                (path.extension() != ".jpg" && path.extension() != ".mkv"))
                throw zt6::Error("无效文件名");
            auto file = zt6::fs::canonical(config.output_dir / path);
            if (file.parent_path() != zt6::fs::canonical(config.output_dir))
                throw zt6::Error("无效文件路径");
            res.set_header("Content-Disposition", "attachment; filename=\"" + name + "\"");
            res.set_file_content(file.string());
        });
        server.Post("/api/action", [&](const auto &req, auto &res) {
            auto data = Json::parse(req.body);
            if (!data.is_object())
                throw zt6::Error("请求必须是对象");
            auto command = action_request(data);
            auto result =
                zt6::execute(config, command, [&req] { return req.is_connection_closed(); });
            if (command["command"] == "view" ||
                ((command["command"] == "resolution" || command["command"] == "thermal-output") &&
                 result.value("applied", result.value("changed", false))))
                app.preview.restart();
            Json response{{"ok", true}, {"result", result}};
            // 姿态动作将完成布尔值同时放在外层，错误也可从同一字段读取。
            if (result.contains("success"))
                response["success"] = result["success"];
            reply(res, response);
        });
        auto stream = [&](const httplib::Request &req, httplib::Response &res) {
            if (!app.preview.acquire()) {
                reply(res, {{"error", "预览连接已满"}}, 429);
                return;
            }
            auto first = app.preview.wait(0, 15);
            if (!first.second) {
                app.preview.release();
                reply(res, {{"error", app.preview.info()["error"]}}, 503);
                return;
            }
            if (req.path == "/api/frame.jpg") {
                res.set_content(*first.second, "image/jpeg");
                app.preview.release();
                return;
            }
            struct Cursor {
                uint64_t sequence = 0;
                std::shared_ptr<std::string> frame;
            };
            auto cursor = std::make_shared<Cursor>();
            cursor->sequence = first.first;
            cursor->frame = first.second;
            res.set_chunked_content_provider(
                "multipart/x-mixed-replace; boundary=zt6frame",
                [&, cursor](size_t, httplib::DataSink &sink) {
                    if (!cursor->frame) {
                        auto next = app.preview.wait(cursor->sequence, 12);
                        cursor->sequence = next.first;
                        cursor->frame = next.second;
                    }
                    if (!cursor->frame)
                        return false;
                    auto header = "--zt6frame\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                                  std::to_string(cursor->frame->size()) + "\r\n\r\n";
                    bool ok = sink.write(header.data(), header.size()) &&
                              sink.write(cursor->frame->data(), cursor->frame->size()) &&
                              sink.write("\r\n", 2);
                    cursor->frame.reset();
                    return ok;
                },
                [&](bool) { app.preview.release(); });
        };
        server.Get("/stream.mjpg", stream);
        server.Get("/api/frame.jpg", stream);
        // 先绑定再启动退出监视，避免端口冲突时留下工作线程。
        if (!server.bind_to_port("127.0.0.1", port))
            throw zt6::Error("HTTP 端口已占用");
        std::thread watcher([&] {
            while (!ending)
                zt6::pause(.1);
            app.close();
            server.stop();
        });
        std::cout << "ZT6 C++ Web: http://127.0.0.1:" << port << "\n" << std::flush;
        try {
            server.listen_after_bind();
        } catch (...) {
            // 监听异常也先回收退出线程，避免 joinable 线程析构触发 terminate。
            ending = 1;
            watcher.join();
            throw;
        }
        ending = 1;
        watcher.join();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Web 错误：" << e.what() << "\n";
        return 2;
    }
}
