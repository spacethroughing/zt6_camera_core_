// ZT6 C++ 核心：CLI 与 Web 共用协议、状态和文件管理。
// 阅读顺序：通用工具 -> 报文编解码 -> SDK/配置协议 -> FFmpeg -> 业务分派。
#include "core.hpp"
#include <httplib.h>
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <spawn.h>
#include <sstream>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
extern char **environ;

namespace zt6 {
volatile sig_atomic_t interrupted = 0;
double monotonic() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
void pause(double seconds) {
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}
std::string read_text(const fs::path &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return {};
    return {std::istreambuf_iterator<char>(f), {}};
}
Json read_json(const fs::path &path) {
    auto text = read_text(path);
    return text.empty() ? Json::object() : Json::parse(text);
}
static void write_all(int fd, const uint8_t *data, size_t length) {
    while (length) {
        auto n = ::write(fd, data, length);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            throw Error("写入失败：" + std::string(strerror(errno)));
        data += n;
        length -= size_t(n);
    }
}
// 配置文件采用“临时文件 + fsync + rename”保存。
// rename 发生在写完之后，读取方尽量不会看到只写了一半的 JSON。
void write_json(const fs::path &path, const Json &data) {
    fs::create_directories(path.parent_path());
    std::string temporary = path.string() + ".tmp.XXXXXX";
    std::vector<char> name(temporary.begin(), temporary.end());
    name.push_back(0);
    int fd = mkstemp(name.data());
    if (fd < 0)
        throw Error("无法创建状态文件");
    try {
        auto text = data.dump(2) + "\n";
        write_all(fd, reinterpret_cast<const uint8_t *>(text.data()), text.size());
        if (fsync(fd))
            throw Error("状态文件同步失败：" + std::string(strerror(errno)));
        close(fd);
        fd = -1;
        if (rename(name.data(), path.c_str()))
            throw Error("无法保存状态文件");
    } catch (...) {
        if (fd >= 0)
            close(fd);
        unlink(name.data());
        throw;
    }
}
std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char prefix[64];
    strftime(prefix, sizeof(prefix), "%Y%m%d_%H%M%S", &tm);
    auto us =
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count() %
        1000000;
    std::ostringstream out;
    out << prefix << "_" << std::setw(6) << std::setfill('0') << us;
    return out.str();
}
// /proc/PID/stat 的第 22 字段是进程启动时刻；与 PID 一起辨认录像进程。
// comm 字段可能含空格，因此先找最后一个右括号，再拆分剩余字段。
std::string process_identity(int pid) {
    auto raw = read_text("/proc/" + std::to_string(pid) + "/stat");
    auto end = raw.rfind(')');
    if (end == std::string::npos)
        return {};
    std::istringstream input(raw.substr(end + 2));
    std::string field;
    for (int i = 0; i <= 19; ++i) {
        if (!(input >> field))
            return {};
        if (i == 0 && field == "Z")
            return {};
    }
    return field;
}
// 停止录像前的身份核对：PID 存在、启动时刻一致、命令行含准确的输出文件参数。
// 这样旧的 recording.json 不会仅凭一个已经复用的 PID 停掉无关进程。
bool matches_process(const Json &metadata) {
    if (!metadata.contains("pid") || !metadata.contains("start_ticks") ||
        metadata["start_ticks"].is_null())
        return false;
    int pid = metadata.value("pid", 0);
    if (pid <= 1)
        return false;
    auto expected = metadata["start_ticks"].is_string() ? metadata["start_ticks"].get<std::string>()
                                                        : metadata["start_ticks"].dump();
    if (process_identity(pid) != expected)
        return false;
    auto command = read_text("/proc/" + std::to_string(pid) + "/cmdline");
    auto path = metadata.value("path", "");
    size_t start = 0;
    while (start < command.size()) {
        auto end = command.find('\0', start);
        if (command.substr(start, end - start) == path)
            return true;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return false;
}
// SDK 所有多字节字段都按低字节在前处理，不能依赖运行机器的隐含字节序。
static uint16_t u16(const Bytes &b, size_t i) {
    return uint16_t(b.at(i)) | uint16_t(b.at(i + 1)) << 8;
}
static uint32_t u32(const Bytes &b, size_t i) {
    return uint32_t(u16(b, i)) | (uint32_t(u16(b, i + 2)) << 16);
}
static void append16(Bytes &b, uint16_t v) {
    b.push_back(uint8_t(v));
    b.push_back(uint8_t(v >> 8));
}
static void append32(Bytes &b, uint32_t v) {
    append16(b, uint16_t(v));
    append16(b, uint16_t(v >> 16));
}
// 公开 SDK 的校验：初值 0，多项式 0x1021，覆盖 CRC 字段之前的全部字节。
uint16_t crc16(const Bytes &bytes) {
    uint16_t c = 0;
    for (auto b : bytes) {
        c ^= uint16_t(b) << 8;
        for (int i = 0; i < 8; ++i)
            c = uint16_t((c << 1) ^ ((c & 0x8000) ? 0x1021 : 0));
    }
    return c;
}
// 官方应用内部协议的 CRC32：初值 0，多项式 0x04C11DB7，不反射、不做末尾异或。
// 它不是常见 zlib CRC32，不能直接用 zlib 结果替代。
uint32_t crc32(const Bytes &bytes) {
    uint32_t c = 0;
    for (auto b : bytes) {
        c ^= uint32_t(b) << 24;
        for (int i = 0; i < 8; ++i)
            c = (c << 1) ^ ((c & 0x80000000) ? 0x04c11db7u : 0);
    }
    return c;
}
// 外部 SDK 帧：55 66 | CTRL | 长度LE16 | 序号LE16 | CMD | DATA | CRC16LE。
// CTRL 默认 1（需要 ACK）；照片/录像另通过 0x0B 反馈，不用通用 query 等待 0x0C。
Bytes packet(uint8_t cmd, const Bytes &data, uint16_t seq, uint8_t control) {
    Bytes b{0x55, 0x66, control};
    append16(b, uint16_t(data.size()));
    append16(b, seq);
    b.push_back(cmd);
    b.insert(b.end(), data.begin(), data.end());
    append16(b, crc16(b));
    return b;
}
// 内部配置帧：55 66 AA BB | CTRL | 长度LE32 | 序号LE16 | CMD | 头CRC32 | DATA | 全帧CRC32。
// 本程序只在自动录像设置中使用这条 TCP 通路，不提供任意内部命令发送入口。
Bytes inner_packet(uint8_t cmd, const Bytes &data, uint16_t seq) {
    Bytes b{0x55, 0x66, 0xaa, 0xbb, 1};
    append32(b, uint32_t(data.size()));
    append16(b, seq);
    b.push_back(cmd);
    append32(b, crc32(b));
    b.insert(b.end(), data.begin(), data.end());
    append32(b, crc32(b));
    return b;
}
std::string hex(const Bytes &bytes) {
    std::ostringstream out;
    for (auto b : bytes)
        out << std::hex << std::setw(2) << std::setfill('0') << int(b);
    return out.str();
}
// 一个 UDP 数据报可能拼着多条 SDK 帧；逐条检查起始字节、长度、CRC 后再接受。
// 不完整或校验失败的数据不会作为有效相机状态返回。
std::vector<Frame> parse_frames(const Bytes &b) {
    std::vector<Frame> result;
    size_t i = 0;
    while (i + 10 <= b.size()) {
        if (b[i] != 0x55 || b[i + 1] != 0x66) {
            ++i;
            continue;
        }
        size_t n = u16(b, i + 3) + 10;
        if (i + n > b.size() ||
            crc16(Bytes(b.begin() + i, b.begin() + i + n - 2)) != u16(b, i + n - 2)) {
            ++i;
            continue;
        }
        result.push_back({b[i + 2], b[i + 7], Bytes(b.begin() + i + 8, b.begin() + i + n - 2)});
        i += n;
    }
    return result;
}
// TCP 可能拆帧或粘帧：不足一帧时把尾部留在 b 中，下一次接收后继续解析。
// 先校验固定头，再信任长度；长度上限用于防止异常报文无限占用缓冲区。
std::vector<Frame> parse_inner(Bytes &b) {
    std::vector<Frame> result;
    const Bytes magic{0x55, 0x66, 0xaa, 0xbb};
    while (b.size() >= 20) {
        auto it = std::search(b.begin(), b.end(), magic.begin(), magic.end());
        if (it == b.end()) {
            b.erase(b.begin(), b.end() - 3);
            break;
        }
        b.erase(b.begin(), it);
        if (b.size() < 20)
            break;
        if (crc32(Bytes(b.begin(), b.begin() + 12)) != u32(b, 12)) {
            b.erase(b.begin());
            continue;
        }
        size_t n = size_t(u32(b, 5)) + 20;
        if (n > 4 * 1024 * 1024) {
            b.erase(b.begin());
            continue;
        }
        if (b.size() < n)
            break;
        if (crc32(Bytes(b.begin(), b.begin() + n - 4)) == u32(b, n - 4))
            result.push_back({b[4], b[11], Bytes(b.begin() + 16, b.begin() + n - 4)});
        b.erase(b.begin(), b.begin() + n);
    }
    return result;
}
Config::Config() {
    const char *home = getenv("HOME");
    fs::path base = home ? home : "/tmp";
    state_dir = base / ".local/state/zt6-camera";
    output_dir = base / "zt6_camera_data";
    workspace = ZT6_WORKSPACE;
}
void Config::validate() const {
    in_addr addr{};
    if (inet_pton(AF_INET, host.c_str(), &addr) != 1)
        throw Error("相机地址必须是 IPv4");
    if (port < 1 || port > 65535 || media_port < 1 || media_port > 65535 ||
        !std::isfinite(timeout) || timeout <= 0 || timeout > 30)
        throw Error("端口或超时无效");
}
std::string Config::stream_url() const {
    return rtsp.empty() ? "rtsp://" + host + ":8554/video1" : rtsp;
}
// 文件锁由操作系统在 fd 关闭时释放，CLI 进程退出后不会留下永久“已加锁”状态。
Lock::Lock(const fs::path &dir, double wait_seconds) {
    if (!std::isfinite(wait_seconds) || wait_seconds < 0 || wait_seconds > 30)
        throw Error("控制锁等待时间必须在 0..30 秒之间");
    fs::create_directories(dir);
    chmod(dir.c_str(), 0700);
    fd_ = open((dir / "control.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd_ < 0)
        throw Error("控制锁创建失败");
    const double deadline = monotonic() + wait_seconds;
    for (;;) {
        if (!flock(fd_, LOCK_EX | LOCK_NB)) {
            if (!interrupted)
                break;
            close(fd_);
            fd_ = -1;
            throw Error("等待控制锁时已中断，未执行命令");
        }
        const int error = errno;
        if (error == EINTR && !interrupted)
            continue;
        if (!interrupted && (error == EWOULDBLOCK || error == EAGAIN) && monotonic() < deadline) {
            pause(std::min(.02, deadline - monotonic()));
            continue;
        }
        close(fd_);
        fd_ = -1;
        if (interrupted)
            throw Error("等待控制锁时已中断，未执行命令");
        if (error != EWOULDBLOCK && error != EAGAIN)
            throw Error("控制锁获取失败：" + std::string(strerror(error)));
        throw Error("另一条控制命令正在运行，请稍后重试。" +
                    std::string(wait_seconds > 0 ? "已短时等待；请检查 Web 或定时录像命令。" : ""));
    }
}
Lock::~Lock() {
    if (fd_ >= 0)
        close(fd_);
}
// UDP 用 connect 固定收发对象；TCP 用非阻塞 connect + poll 限制连接等待时间。
// 连接成功后恢复阻塞方式，具体 SDK 接收仍用 deadline 控制等待。
static int connect_socket(const std::string &host, int port, int type, double timeout) {
    int fd = socket(AF_INET, type | SOCK_CLOEXEC, 0);
    if (fd < 0)
        throw Error("无法创建网络套接字");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(port));
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int r = connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    if (r < 0 && errno != EINPROGRESS) {
        close(fd);
        throw Error("相机网络连接失败");
    }
    if (r < 0) {
        pollfd p{fd, POLLOUT, 0};
        int err = 0;
        socklen_t n = sizeof(err);
        if (poll(&p, 1, int(timeout * 1000)) <= 0 ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &n) || err) {
            close(fd);
            throw Error("相机连接超时/拒绝连接");
        }
    }
    fcntl(fd, F_SETFL, flags);
    timeval tv{time_t(timeout), suseconds_t((timeout - std::floor(timeout)) * 1e6)};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}
SDK::SDK(const Config &c)
    : fd_(connect_socket(c.host, c.port, SOCK_DGRAM, c.timeout)), timeout_(c.timeout) {}
SDK::~SDK() {
    if (fd_ >= 0)
        close(fd_);
}
void SDK::send(uint8_t cmd, const Bytes &data) {
    auto bytes = packet(cmd, data, sequence_++);
    if (::send(fd_, bytes.data(), bytes.size(), 0) != ssize_t(bytes.size()))
        throw Error("SDK 发送失败");
}
std::vector<Frame> SDK::receive(double deadline) {
    double remain = deadline - monotonic();
    if (remain <= 0)
        throw Error("SDK 查询超时");
    pollfd p{fd_, POLLIN, 0};
    int r = poll(&p, 1, std::max(1, int(remain * 1000)));
    if (r <= 0)
        throw Error("SDK 查询超时：检查相机网络/供电。");
    Bytes b(65536);
    auto n = recv(fd_, b.data(), b.size(), 0);
    if (n <= 0)
        throw Error("SDK 接收失败");
    b.resize(size_t(n));
    auto result = parse_frames(b);
    for (auto &f : result)
        if (f.command == 0x0b && !f.data.empty())
            feedback_.push_back(f.data[0]);
    return result;
}
void SDK::drain() {
    Bytes b(65536);
    for (int i = 0; i < 256; ++i) {
        auto n = recv(fd_, b.data(), b.size(), MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            throw Error("SDK 连接异常");
        }
        auto frames = parse_frames(Bytes(b.begin(), b.begin() + n));
        for (auto &f : frames)
            if (f.command == 0x0b && !f.data.empty())
                feedback_.push_back(f.data[0]);
    }
}
// 当前 ZT6 的响应 SEQ 是自己的计数，不保证回显请求 SEQ。
// 因此清空已到达的旧数据后，只接受目标连接上“相同命令 + ACK 标志”的有效帧。
Bytes SDK::query(uint8_t cmd, const Bytes &data) {
    drain();
    send(cmd, data);
    double end = monotonic() + timeout_;
    while (monotonic() < end)
        for (auto &frame : receive(end))
            if (frame.command == cmd && (frame.control & 2))
                return frame.data;
    throw Error("SDK 查询超时");
}
static std::string record_name(int state) {
    switch (state) {
    case 0:
        return "stopped";
    case 1:
        return "recording";
    case 2:
        return "no_sd_card";
    case 3:
        return "recording_card_error";
    default:
        return "unknown";
    }
}
// 0x0A：DATA[3] 是 record_state，DATA[4] 是运动模式，DATA[5] 是安装方向。
// 安装方向为 2 表示倒装，后续 GUI 的角度显示还需核对倒装坐标。
Json SDK::status() {
    auto b = query(0x0a);
    if (b.size() < 6)
        throw Error("云台状态响应过短");
    return {{"record_state", b[3]},
            {"record_status", record_name(b[3])},
            {"gimbal_mode", b[4]},
            {"mounting_direction", b[5]},
            {"raw", hex(b)}};
}
Json SDK::version() {
    auto b = query(1);
    if (b.size() < 8)
        throw Error("固件响应过短");
    Json j = Json::object();
    std::vector<std::string> names{"camera", "gimbal", "zoom"};
    for (size_t i = 0; i < names.size() && i * 4 + 3 < b.size(); ++i) {
        size_t n = i * 4;
        j[names[i]] =
            std::to_string(b[n + 2]) + "." + std::to_string(b[n + 1]) + "." + std::to_string(b[n]);
    }
    return j;
}
// 0x0D：yaw、pitch、roll 及三个角速度依次排列，每项是有符号小端 int16。
// 除以 10 才是度/度每秒；此处保留 SDK 原始坐标定义，不擅自做倒装补偿。
Json SDK::attitude() {
    auto b = query(0x0d);
    if (b.size() < 6)
        throw Error("姿态响应过短");
    Json j;
    std::vector<std::string> names{"yaw", "pitch", "roll", "yaw_rate", "pitch_rate", "roll_rate"};
    for (size_t i = 0; i < names.size() && i * 2 + 1 < b.size(); ++i)
        j[names[i]] = int16_t(u16(b, i * 2)) / 10.0;
    return j;
}
// 0x08/01：一键回中。ACK=1 只确认接收成功，随后用独立姿态查询确认完成。
Json SDK::center(double wait_seconds, double tolerance) {
    number(wait_seconds, .1, 20, "wait-seconds");
    number(tolerance, .1, 5, "tolerance");
    int mounting = status()["mounting_direction"];
    if (mounting != 1 && mounting != 2)
        throw Error("安装方向未知，无法确认回中坐标；未发送回中命令");
    auto b = query(8, {1});
    if (b != Bytes{1})
        throw Error("回中命令未确认");
    auto result = wait_attitude(0, 0, mounting, wait_seconds, tolerance);
    result["center"] = "completed";
    return result;
}
double number(const Json &value, double low, double high, const std::string &name) {
    if (!value.is_number())
        throw Error(name + " 必须是数字");
    double v = value.get<double>();
    if (!std::isfinite(v) || v < low || v > high)
        throw Error(name + " 超出范围 " + std::to_string(low) + ".." + std::to_string(high));
    return v;
}
// 0x0E：输入是度，先检查 ZT6 手册范围，再乘 10、四舍五入、写成两个 int16。
// ACK 给出的是“此刻”的角度，不把它当作目标已经到达。
Json SDK::angle(double yaw, double pitch, double wait_seconds, double tolerance) {
    number(yaw, -270, 270, "yaw");
    number(pitch, -90, 25, "pitch");
    number(wait_seconds, .1, 20, "wait-seconds");
    number(tolerance, .1, 5, "tolerance");
    // 实际报文精度为 0.1°，到位判断使用同一个量化目标。
    yaw = std::round(yaw * 10) / 10;
    pitch = std::round(pitch * 10) / 10;
    int mounting = status()["mounting_direction"];
    if (mounting != 1 && mounting != 2)
        throw Error("安装方向未知，无法确认目标坐标；未发送角度命令");
    Bytes b;
    append16(b, uint16_t(int16_t(std::lround(yaw * 10))));
    append16(b, uint16_t(int16_t(std::lround(pitch * 10))));
    auto reply = query(0x0e, b);
    if (reply.size() < 6)
        throw Error("角度控制响应过短");
    auto result = wait_attitude(yaw, pitch, mounting, wait_seconds, tolerance);
    // 保留旧接口的 ACK 时刻姿态；attitude 才是完成时的最终独立反馈。
    result["reported"] = {{"yaw", int16_t(u16(reply, 0)) / 10.0},
                          {"pitch", int16_t(u16(reply, 2)) / 10.0},
                          {"roll", int16_t(u16(reply, 4)) / 10.0}};
    return result;
}
// 没有厂商“动作已完成”的 ACK：主机通过 0x0D 的角度和角速度连续确认。
// 2026-11-10 真机确认：本台倒装 ZT6 的俯仰反馈=目标+180°（回绕至 ±180°）。
// 正装不补偿；换算仅用于内部到位判断，输出 attitude 保留原始 SDK 坐标。
Json SDK::wait_attitude(double yaw, double pitch, int mounting, double wait_seconds,
                        double tolerance) {
    const double began = monotonic(), deadline = began + wait_seconds;
    double stable_since = 0;
    int samples = 0;
    Json last = Json::object();
    std::string last_error;
    // 每次查询也受总等待期限约束；离开函数时恢复普通 SDK 超时。
    struct RestoreTimeout {
        double &value;
        double original;
        ~RestoreTimeout() {
            value = original;
        }
    } restore{timeout_, timeout_};
    while (monotonic() < deadline && !interrupted) {
        try {
            timeout_ = std::min({restore.original, .4, deadline - monotonic()});
            if (timeout_ <= 0)
                break;
            last = attitude();
            last_error.clear();
        } catch (const Error &e) {
            stable_since = 0;
            samples = 0;
            last_error = e.what();
            continue; // 只重试只读查询，不重复发送运动指令。
        }
        double actual_yaw = last.at("yaw").get<double>();
        double actual_pitch =
            std::remainder(last.at("pitch").get<double>() - (mounting == 2 ? 180.0 : 0.0), 360.0);
        const bool in_position = std::abs(std::remainder(actual_yaw - yaw, 360.0)) <= tolerance &&
                                 std::abs(std::remainder(actual_pitch - pitch, 360.0)) <= tolerance;
        const bool slow = last.contains("yaw_rate") && last.contains("pitch_rate") &&
                          std::abs(last["yaw_rate"].get<double>()) <= 2 &&
                          std::abs(last["pitch_rate"].get<double>()) <= 2;
        if (in_position && slow) {
            if (!samples)
                stable_since = monotonic();
            ++samples;
            if (samples >= 3 && monotonic() - stable_since >= .3 && !interrupted)
                return {{"success", true},
                        {"status", "completed"},
                        {"message", "已完成"},
                        {"requested", {{"yaw", yaw}, {"pitch", pitch}}},
                        {"attitude", last},
                        {"mounting_direction", mounting},
                        {"tolerance_deg", tolerance},
                        {"stable_seconds", monotonic() - stable_since},
                        {"elapsed_seconds", monotonic() - began}};
        } else {
            stable_since = 0;
            samples = 0;
        }
        pause(std::max(0.0, std::min(.1, deadline - monotonic())));
    }
    if (interrupted)
        throw Error("到位确认已中断，未确认完成；动作已发送，请查询 attitude 或执行 stop");
    throw Error("到位确认超时，未确认完成；未重复发送动作指令。最后姿态：" + last.dump() +
                (last_error.empty() ? "" : "；最后查询错误：" + last_error));
}
// 0x07/00 00：方向速度归零是幂等操作，允许连续发多次降低丢包影响。
// 单次发送失败仍继续尝试剩余停止报文；全部失败时将错误交给调用者。
Json SDK::stop() {
    int sent = 0;
    for (int i = 0; i < 3; ++i) {
        try {
            send(7, {0, 0});
            ++sent;
        } catch (const Error &) {
        }
        pause(.015);
    }
    if (!sent)
        throw Error("停止报文发送失败");
    return {{"motion", "stop_sent"}, {"packets_sent", sent}};
}
// 0x07：载荷是两个带符号速度值。动作限定在短时间内，并在结束/异常时尝试停止。
// interrupted 由 CLI 信号处理器设置；即使用户中断等待，也会走到末尾发送归零。
Json SDK::move(int yaw, int pitch, double seconds) {
    if (yaw < -100 || yaw > 100 || pitch < -100 || pitch > 100 || !std::isfinite(seconds) ||
        seconds <= 0 || seconds > 1)
        throw Error("方向速度 -100..100，单次时长 (0,1] 秒");
    try {
        send(7, {uint8_t(int8_t(yaw)), uint8_t(int8_t(pitch))});
        double end = monotonic() + seconds;
        while (monotonic() < end && !interrupted)
            pause(.01);
    } catch (...) {
        try {
            stop();
        } catch (...) {
        }
        throw;
    }
    stop();
    return {{"motion", "stopped"}, {"duration", seconds}};
}
// 0x10 查询，0x11 设置主码流来源：3 可见光、7 热成像、0 拼接。
// 切换前检查 SD 录像；统一 execute 入口还会检查机载录像，避免录制中途换画面。
Json SDK::view(const std::string &source) {
    if (source != "status") {
        int wanted = source == "visible" ? 3 : source == "thermal" ? 7 : source == "pip" ? 0 : -1;
        if (wanted < 0)
            throw Error("未知画面类型");
        auto before = query(0x10);
        if (before.empty())
            throw Error("画面模式响应为空");
        if (before[0] != wanted) {
            int state = status()["record_state"];
            if (state == 1 || state == 3)
                throw Error("SD 正在录像，请先停止后切换画面");
            auto b = query(0x11, {uint8_t(wanted)});
            if (b.empty() || b[0] != wanted)
                throw Error("画面切换未确认，未重复发送");
            pause(.2);
        }
    }
    auto b = query(0x10);
    if (b.empty())
        throw Error("画面模式响应为空");
    return {{"mode", b[0]},
            {"view", b[0] == 3   ? "visible"
                     : b[0] == 7 ? "thermal"
                     : b[0] == 0 ? "pip"
                                 : "other"}};
}
// 新设置共用的读回确认：写指令由调用者发一次，这里只重复读取，不自动重设。
Bytes SDK::confirm(uint8_t command, const Bytes &data,
                   const std::function<bool(const Bytes &)> &matches, const std::string &name) {
    const double original = timeout_;
    const double deadline = monotonic() + std::clamp(original * 2, 2.0, 10.0);
    struct Restore {
        double &value;
        double original;
        ~Restore() {
            value = original;
        }
    } restore{timeout_, original};
    std::string last_error;
    while (monotonic() < deadline && !interrupted) {
        try {
            timeout_ = std::min({original, .5, deadline - monotonic()});
            if (timeout_ <= 0)
                break;
            auto reply = query(command, data);
            if (matches(reply))
                return reply;
            last_error = "读回值尚未达到目标";
        } catch (const Error &e) {
            last_error = e.what();
        }
        pause(std::max(0.0, std::min(.1, deadline - monotonic())));
    }
    throw Error(name + (interrupted ? "确认已中断" : "读回确认超时") +
                "；设置可能已生效，未重复发送写指令。" + last_error);
}
static int whole_number(const Json &value, int low, int high, const std::string &name) {
    double n = number(value, low, high, name);
    if (n != std::trunc(n))
        throw Error(name + " 必须是整数");
    return int(n);
}
static Json decode_resolution(const Bytes &b, int stream) {
    if (b.size() < 9 || b[0] != stream || (b[1] != 1 && b[1] != 2) || !u16(b, 2) || !u16(b, 4))
        throw Error("编码参数响应格式错误");
    return {{"stream_id", stream},
            {"stream", stream == 0   ? "record"
                       : stream == 1 ? "main"
                                     : "sub"},
            {"codec", b[1] == 1 ? "h264" : "h265"},
            {"width", u16(b, 2)},
            {"height", u16(b, 4)},
            {"bitrate_kbps", u16(b, 6)},
            {"fps", b[8]}};
}
// 0x20/0x21 的每个多字节字段均为小端；最后一字节是保留位，不是可设置帧率。
// 外部 SDK 表列出 720p/1080p；用户手册 v1.3 第 70 页另给出录像流 2K/4K 报文。
// 这两个额外尺寸仅用于 stream=0，不能据此推断主/副 RTSP 也支持 4K 设置。
static bool supported_resolution(int stream, int width, int height) {
    return (width == 1920 && height == 1080) || (width == 1280 && height == 720) ||
           (stream == 0 &&
            ((width == 2560 && height == 1440) || (width == 3840 && height == 2160)));
}
Json SDK::resolution(int stream, const Json &options) {
    if (stream < 0 || stream > 2 || !options.is_object())
        throw Error("码流参数无效");
    for (auto it = options.begin(); it != options.end(); ++it)
        if (it.key() != "width" && it.key() != "height" && it.key() != "codec" &&
            it.key() != "bitrate_kbps" && it.key() != "reapply")
            throw Error("未知编码设置参数：" + it.key());
    if (options.contains("width") != options.contains("height"))
        throw Error("分辨率必须同时指定 width 和 height");
    auto before = decode_resolution(query(0x20, {uint8_t(stream)}), stream);
    auto wanted = before;
    if (options.contains("width")) {
        int width = whole_number(options["width"], 1, 4096, "width");
        int height = whole_number(options["height"], 1, 2160, "height");
        if (!supported_resolution(stream, width, height))
            throw Error("公开 SDK 支持 1920x1080/1280x720；录像流另支持 "
                        "2560x1440/3840x2160，其他原生尺寸仅查询");
        wanted["width"] = width;
        wanted["height"] = height;
    }
    if (options.contains("codec")) {
        auto codec = options["codec"].get<std::string>();
        if (codec != "h264" && codec != "h265")
            throw Error("codec 为 h264 或 h265");
        if (stream == 0 && codec != before["codec"])
            throw Error("公开 SDK 暂不支持修改录像流编码格式");
        wanted["codec"] = codec;
    }
    if (options.contains("bitrate_kbps"))
        wanted["bitrate_kbps"] = whole_number(options["bitrate_kbps"], 1, 65535, "bitrate_kbps");
    bool changed = wanted != before;
    bool reapply = options.value("reapply", false);
    bool applied = changed || reapply;
    if (applied) {
        int width = wanted["width"], height = wanted["height"];
        if (!supported_resolution(stream, width, height))
            throw Error("当前码流为其他原生尺寸；请先选择公开 SDK 支持的 1920x1080 或 1280x720");
        Bytes data{uint8_t(stream), uint8_t(wanted["codec"] == "h264" ? 1 : 2)};
        for (auto key : {"width", "height", "bitrate_kbps"})
            append16(data, uint16_t(wanted[key].get<int>()));
        data.push_back(0); // reserve，保留设备原帧率，不发送伪造帧率设置。
        auto ack = query(0x21, data);
        if (ack.size() < 2 || ack[0] != stream || ack[1] != 1)
            throw Error("相机拒绝编码参数设置，未重复发送");
        before =
            decode_resolution(confirm(
                                  0x20, {uint8_t(stream)},
                                  [&](const Bytes &b) {
                                      auto actual = decode_resolution(b, stream);
                                      for (auto key : {"width", "height", "codec", "bitrate_kbps"})
                                          if (actual[key] != wanted[key])
                                              return false;
                                      return true;
                                  },
                                  "编码参数"),
                              stream);
    }
    before["success"] = true;
    before["changed"] = changed;
    before["applied"] = applied;
    return before;
}
// 运动模式的 0x0C 没有设置成功 ACK，用 0x19 的实际模式确认，不借用拍照反馈。
Json SDK::gimbal_mode(const std::string &mode) {
    int wanted = mode == "lock" ? 0 : mode == "follow" ? 1 : mode == "fpv" ? 2 : -1;
    if (mode != "status" && wanted < 0)
        throw Error("云台模式为 status/lock/follow/fpv");
    auto before = query(0x19);
    if (before.size() != 1 || before[0] > 2)
        throw Error("云台模式响应无效");
    bool changed = wanted >= 0 && before[0] != wanted;
    if (changed) {
        send(0x0c, {uint8_t(wanted + 3)});
        before = confirm(
            0x19, {}, [&](const Bytes &b) { return b.size() == 1 && b[0] == wanted; }, "云台模式");
    }
    return {{"success", true},
            {"changed", changed},
            {"gimbal_mode", before[0]},
            {"mode", before[0] == 0   ? "lock"
                     : before[0] == 1 ? "follow"
                                      : "fpv"}};
}
static int zoom_tenths(const Bytes &b) {
    if (b.size() != 2 || b[0] < 1 || b[0] > 30 || b[1] > 9)
        throw Error("变倍响应无效");
    return b[0] * 10 + b[1];
}
Json SDK::zoom(const Json &ratio) {
    int wanted = ratio.is_null() ? -1 : int(std::lround(number(ratio, 1, 30, "ratio") * 10));
    int maximum = zoom_tenths(query(0x16));
    int current = zoom_tenths(query(0x18));
    if (wanted > maximum)
        throw Error("倍率超过当前画面支持的最大值 " + std::to_string(maximum / 10.0));
    bool changed = wanted >= 0 && current != wanted;
    if (changed) {
        auto ack = query(0x0f, {uint8_t(wanted / 10), uint8_t(wanted % 10)});
        if (ack != Bytes{1})
            throw Error("变倍命令未确认，未重复发送");
        current = zoom_tenths(
            confirm(0x18, {}, [&](const Bytes &b) { return zoom_tenths(b) == wanted; }, "变倍"));
    }
    return {{"success", true},
            {"changed", changed},
            {"ratio", current / 10.0},
            {"max_ratio", maximum / 10.0},
            {"zoom_type", "digital"}};
}
Json SDK::thermal_output(const std::string &mode) {
    int wanted = mode == "video" ? 0 : mode == "temperature" ? 1 : -1;
    if (mode != "status" && wanted < 0)
        throw Error("热像输出模式为 status/video/temperature");
    auto before = query(0x33);
    if (before.size() != 1 || before[0] > 1)
        throw Error("热像输出模式响应无效");
    bool changed = wanted >= 0 && before[0] != wanted;
    if (changed) {
        auto ack = query(0x34, {uint8_t(wanted)});
        if (ack != Bytes{uint8_t(wanted)})
            throw Error("热像输出设置未确认，未重复发送");
        before = confirm(
            0x33, {}, [&](const Bytes &b) { return b.size() == 1 && b[0] == wanted; },
            "热像输出模式");
    }
    return {{"success", true},
            {"changed", changed},
            {"mode_id", before[0]},
            {"mode", before[0] ? "temperature" : "video"},
            {"fps", before[0] ? 25 : 30},
            {"temperature_output", before[0] == 1}};
}
// ZT6 手册的热像联动用于拼接画面下 1–2 倍同步变倍，不将可见光 6 倍当作热像 6 倍。
Json SDK::zoom_link(const std::string &action) {
    if (action != "status" && action != "on" && action != "off")
        throw Error("zoom-link 参数为 status/on/off");
    auto before = query(0x0a);
    if (before.size() < 8 || before[7] > 1)
        throw Error("设备未返回有效联动变倍状态");
    bool changed = action != "status" && (before[7] == 1) != (action == "on");
    if (changed) {
        send(0x0c, {10});
        before = confirm(
            0x0a, {},
            [&](const Bytes &b) { return b.size() >= 8 && b[7] == (action == "on" ? 1 : 0); },
            "联动变倍");
    }
    return {{"success", true},
            {"changed", changed},
            {"linked", before[7] == 1},
            {"note", "热像联动用于拼接画面下 1–2 倍同步变倍"}};
}
Json SDK::thermal_frame() {
    // 0x35 是独立的单帧请求，公开协议未规定必须先进入连续温度帧模式。
    // 当前真机在 video 模式也会接受请求，不额外增加软件前置限制。
    auto ack = query(0x35);
    if (ack != Bytes{1})
        throw Error("温度帧请求被拒绝，未重复发送");
    return {{"success", true},
            {"status", "request_accepted"},
            {"request_accepted", true},
            {"frame_received", false},
            {"message", "相机已接受单帧请求；本接口不接收逐像素温度矩阵"}};
}
// SD 原生拍照：先查卡状态，只发送一次 0x0C/00，再等待 0x0B。
// 反馈 0 表示拍照成功；1/4 是失败。等待超时不自动重拍，避免重复拍摄。
Json SDK::photo() {
    if (interrupted)
        throw Error("拍照已中断，未发送拍照命令");
    int state = status()["record_state"];
    if (state != 0 && state != 1)
        throw Error("无法保存到 SD：" + record_name(state));
    drain();
    feedback_.clear();
    if (interrupted)
        throw Error("拍照已中断，未发送拍照命令");
    send(0x0c, {0});
    double end = monotonic() + timeout_;
    try {
        while (monotonic() < end && !interrupted)
            for (auto &f : receive(end))
                if (f.command == 0x0b && !f.data.empty()) {
                    if (f.data[0] == 0)
                        return {{"storage", "sd"}, {"photo", "camera_reported_success"}};
                    if (f.data[0] == 1 || f.data[0] == 4)
                        throw Error("SD 拍照失败，请检查存储卡");
                }
    } catch (const Error &e) {
        if (std::string(e.what()).find("超时") == std::string::npos)
            throw;
    }
    throw Error(interrupted ? "拍照已发送，等待反馈已中断，结果未确认；未自动重拍"
                            : "拍照已发送，未收到成功反馈，结果未确认；未自动重拍");
}
// SD 录像切换后的确认：连续观察目标状态至少 settle 秒，不能只看到一次 0 就说已停止。
// 之前真机出现过“短暂停止又恢复”，关闭自动录像开关后旧版才恢复正常。
Json SDK::wait_record(int wanted, double settle) {
    double end = monotonic() + std::max(10.0, timeout_ * 3);
    double stable = 0;
    while (monotonic() < end && !interrupted) {
        pause(.3);
        Json state;
        try {
            state = status();
        } catch (const Error &) {
            // 查询中断期间不能证明状态稳定，重新累计确认窗口。
            stable = 0;
            continue;
        }
        int now = state["record_state"];
        if (now == 2 || now == 3 ||
            std::find(feedback_.begin(), feedback_.end(), 4) != feedback_.end())
            throw Error("SD 卡录制异常，未再次切换");
        if (now == wanted) {
            if (!stable)
                stable = monotonic();
            if (monotonic() - stable >= settle)
                return state;
        } else if (stable) {
            if (wanted == 0 && now == 1)
                throw Error("SD 录像停止后自行恢复，请检查 auto-record 设置；未重复切换");
            stable = 0;
        }
    }
    throw Error("录像目标状态未确认；未重复切换，请查询 status");
}
// 0x0C/02 是切换开关：先用 0x0A 决定是否有必要切换，不能盲目重发。
// 返回 camera_state_only 明确表示只确认控制状态，不保证新视频已写入并可播放。
Json SDK::record(bool start) {
    auto before = status();
    int current = before["record_state"];
    int wanted = start ? 1 : 0;
    bool changed = current != wanted;
    if (!changed) {
        if (!start)
            before = wait_record(0);
    } else {
        if (current == 2 || (current != 0 && current != 1 && current != 3) ||
            (current == 3 && start))
            throw Error("无法控制 SD 录像：" + record_name(current));
        feedback_.clear();
        send(0x0c, {2});
        before = wait_record(wanted);
    }
    before["storage"] = "sd";
    before["changed"] = changed;
    before["file_verification"] = "camera_state_only";
    return before;
}

// 与公开 UDP SDK 分开：官方应用的自动录像配置使用 TCP 37256。
Settings::Settings(const Config &c)
    : fd_(connect_socket(c.host, 37256, SOCK_STREAM, c.timeout)), timeout_(c.timeout) {}
Settings::~Settings() {
    if (fd_ >= 0)
        close(fd_);
}
void Settings::send(uint8_t cmd, const Bytes &data) {
    auto b = inner_packet(cmd, data, sequence_++);
    write_all(fd_, b.data(), b.size());
}
// 根据官方 UniGCS 实现读取 0x85，响应载荷 0/1 分别表示自动录像关闭/开启。
// 这是官方应用内部接口，公开外部 SDK PDF 没有给出这两个配置命令。
bool Settings::read_auto_record() {
    send(0x85);
    double end = monotonic() + timeout_;
    while (monotonic() < end) {
        for (auto &f : parse_inner(buffer_))
            if ((f.control & 2) && f.command == 0x85 && f.data.size() == 1 && f.data[0] <= 1)
                return f.data[0] != 0;
        pollfd p{fd_, POLLIN, 0};
        if (poll(&p, 1, std::max(1, int((end - monotonic()) * 1000))) <= 0)
            break;
        Bytes b(65536);
        auto n = recv(fd_, b.data(), b.size(), 0);
        if (n <= 0)
            break;
        buffer_.insert(buffer_.end(), b.begin(), b.begin() + n);
    }
    throw Error("自动录像设置查询超时，此固件可能不支持配置协议");
}
// 0x86 直接写入目标布尔值，而不是切换；写完还要用 0x85 读回确认。
Json Settings::auto_record(const std::string &action) {
    if (action != "status" && action != "on" && action != "off")
        throw Error("自动录像参数为 status/on/off");
    bool before = read_auto_record();
    if (action == "status" || before == (action == "on"))
        return {{"auto_record", before}, {"changed", false}};
    bool wanted = action == "on";
    send(0x86, {uint8_t(wanted)});
    pause(.2);
    bool after = read_auto_record();
    if (after != wanted)
        throw Error("自动录像设置未读回确认");
    return {{"auto_record", after}, {"previous", before}, {"changed", true}};
}

// 用 posix_spawnp 启动 FFmpeg：参数数组直接传递，不拼接 shell 命令。
// 在多线程 Web 服务中，避免 fork 后再执行复杂 C++ 分配/锁操作。
int spawn_process(const std::vector<std::string> &args, int stdout_fd, int stderr_fd,
                  bool detached) {
    if (args.empty())
        throw Error("空进程参数");
    std::vector<char *> argv;
    for (auto &a : args)
        argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    int null = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (null < 0)
        throw Error("无法打开 /dev/null");
    posix_spawn_file_actions_adddup2(&actions, null, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, stdout_fd, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, stderr_fd, STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, null);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t mask, defaults;
    sigemptyset(&mask);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGHUP);
    sigaddset(&defaults, SIGPIPE);
    posix_spawnattr_setsigmask(&attr, &mask);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_SETSID
    if (detached)
        flags |= POSIX_SPAWN_SETSID;
#else
    (void)detached;
#endif
    posix_spawnattr_setflags(&attr, flags);
    pid_t pid = -1;
    int error = posix_spawnp(&pid, argv[0], &actions, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    close(null);
    if (error)
        throw Error("进程启动失败：" + std::string(strerror(error)));
    return pid;
}
void finish_process(int pid, int sig) {
    if (pid <= 1)
        return;
    kill(pid, sig);
    double end = monotonic() + 5;
    int status = 0;
    while (monotonic() < end) {
        auto r = waitpid(pid, &status, WNOHANG);
        if (r == pid || (r < 0 && errno == ECHILD))
            return;
        pause(.03);
    }
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}
// 有等待上限的外部工具执行，用于截图、FFprobe 和 FFmpeg 能力探测。
// 这里等待的是本程序启动的子进程；不是根据用户输入任意执行 shell。
ProcessResult run_process(const std::vector<std::string> &args, double timeout) {
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC))
        throw Error("无法创建进程管道");
    int pid;
    try {
        pid = spawn_process(args, pipefd[1], pipefd[1]);
    } catch (...) {
        close(pipefd[0]);
        close(pipefd[1]);
        throw;
    }
    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    std::string out;
    double end = monotonic() + timeout;
    int status = 0;
    bool ended = false;
    while (monotonic() < end && !interrupted) {
        char b[16384];
        ssize_t n;
        while ((n = read(pipefd[0], b, sizeof(b))) > 0)
            if (out.size() < 2 * 1024 * 1024)
                out.append(b, size_t(n));
        auto r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            ended = true;
            while ((n = read(pipefd[0], b, sizeof(b))) > 0)
                if (out.size() < 2 * 1024 * 1024)
                    out.append(b, size_t(n));
            break;
        }
        pollfd p{pipefd[0], POLLIN, 0};
        poll(&p, 1, 30);
    }
    close(pipefd[0]);
    if (!ended) {
        finish_process(pid);
        throw Error(interrupted ? "操作已中断" : "外部进程超时");
    }
    return {WIFEXITED(status) ? WEXITSTATUS(status)
                              : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0),
            out};
}
// RK3588 现有 FFmpeg 支持的 RTSP 超时选项可能不同：启动前读帮助选择 stimeout/timeout。
// RTSP 走 TCP，默认 video1；保存到机载电脑与向云台 SD 发命令是两条独立通路。
std::vector<std::string> ffmpeg_input(const Config &c) {
    static std::string timeout_option;
    static std::mutex mutex;
    {
        std::lock_guard<std::mutex> guard(mutex);
        if (timeout_option.empty()) {
            auto help = run_process({"ffmpeg", "-hide_banner", "-h", "demuxer=rtsp"}, 5).output;
            timeout_option =
                help.find("-stimeout ") != std::string::npos ? "-stimeout" : "-timeout";
        }
    }
    return {"ffmpeg", "-hide_banner",    "-loglevel", "warning",      "-nostdin",
            "-n",     "-rtsp_transport", "tcp",       timeout_option, "5000000",
            "-i",     c.stream_url()};
}
// 只检查记录在 recording.json 中的那一个录像进程，不扫描/停止其他 FFmpeg。
Json Onboard::status() {
    auto meta = read_json(config_.state_dir / "recording.json");
    bool active = matches_process(meta);
    if (!active && meta.value("pid", 0) > 1) {
        int status;
        waitpid(meta.value("pid", 0), &status, WNOHANG);
    }
    return {{"storage", "onboard"},
            {"record_status", active ? "recording" : "stopped"},
            {"active", active},
            {"last_recording", meta}};
}
fs::path Onboard::prepare(const std::string &extension) {
    fs::create_directories(config_.output_dir);
    if (fs::space(config_.output_dir).available < 512ull * 1024 * 1024)
        throw Error("机载空间不足 512 MiB");
    return config_.output_dir / ("zt6_" + timestamp() + extension);
}
// 机载拍照：从当前 RTSP 流解码一帧为 JPEG，分辨率取决于码流，不等同于云台原生拍照。
Json Onboard::photo() {
    auto path = prepare(".jpg");
    auto command = ffmpeg_input(config_);
    std::vector<std::string> tail{"-map", "0:v:0", "-frames:v", "1", "-q:v", "2", path.string()};
    command.insert(command.end(), tail.begin(), tail.end());
    auto result = run_process(command, 20);
    if (result.code || !fs::exists(path) || fs::file_size(path) == 0)
        throw Error("RTSP 截图失败：" + result.output.substr(result.output.size() > 1500
                                                                 ? result.output.size() - 1500
                                                                 : 0));
    return {{"storage", "onboard"},
            {"photo", "saved"},
            {"path", path.string()},
            {"bytes", fs::file_size(path)},
            {"note", "从当前主码流截帧"}};
}
// 机载录像：-c:v copy 不重新编码，直接封装为 MKV。
// 启动后等待 progress 中确实出现 frame>0，不能仅以“进程还活着”判定开始成功。
Json Onboard::start() {
    auto previous = status();
    if (previous["active"] == true) {
        previous["changed"] = false;
        return previous;
    }
    auto path = prepare(".mkv");
    auto log = path;
    log.replace_extension(".ffmpeg.log");
    auto progress = path;
    progress.replace_extension(".progress");
    auto command = ffmpeg_input(config_);
    std::vector<std::string> tail{"-map",      "0:v:0",           "-an", "-c:v",     "copy",
                                  "-progress", progress.string(), "-f",  "matroska", path.string()};
    command.insert(command.end(), tail.begin(), tail.end());
    int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    int null = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (fd < 0 || null < 0) {
        if (fd >= 0)
            close(fd);
        if (null >= 0)
            close(null);
        throw Error("录像日志创建失败");
    }
    int pid;
    try {
        pid = spawn_process(command, null, fd);
    } catch (...) {
        close(fd);
        close(null);
        throw;
    }
    close(fd);
    close(null);
    Json meta{{"pid", pid},
              {"start_ticks", process_identity(pid)},
              {"path", path.string()},
              {"log", log.string()},
              {"progress", progress.string()},
              {"started_at", timestamp()},
              {"rtsp", config_.stream_url()},
              {"implementation", "cpp"}};
    try {
        write_json(config_.state_dir / "recording.json", meta);
    } catch (...) {
        finish_process(pid, SIGINT);
        throw;
    }
    double end = monotonic() + 15;
    while (monotonic() < end && !interrupted) {
        int status_code;
        auto r = waitpid(pid, &status_code, WNOHANG);
        if (r == pid)
            throw Error("机载录像启动失败：" + read_text(log));
        auto text = read_text(progress);
        std::istringstream lines(text);
        std::string line;
        while (std::getline(lines, line))
            if (line.rfind("frame=", 0) == 0 && std::stoi(line.substr(6)) > 0) {
                auto result = status();
                result["changed"] = true;
                return result;
            }
        pause(.15);
    }
    stop();
    throw Error("未在 15 秒内收到视频帧，已停止本次录像");
}
// 核对进程身份后发 SIGINT，让 FFmpeg 正常结束并写好容器尾部；超时给出错误。
Json Onboard::stop() {
    auto before = status();
    if (before["active"] != true) {
        before["changed"] = false;
        return before;
    }
    auto meta = before["last_recording"];
    int pid = meta["pid"];
    if (!matches_process(meta))
        throw Error("录像进程已变化，未发送信号");
    kill(pid, SIGINT);
    double end = monotonic() + 12;
    while (monotonic() < end) {
        if (!matches_process(meta)) {
            int ignored;
            waitpid(pid, &ignored, WNOHANG);
            fs::path path = meta["path"].get<std::string>();
            return {{"storage", "onboard"},
                    {"record_status", "stopped"},
                    {"changed", true},
                    {"path", path.string()},
                    {"bytes", fs::exists(path) ? fs::file_size(path) : 0}};
        }
        pause(.1);
    }
    throw Error("录像进程未退出，未强制杀进程，请检查日志");
}
Json Onboard::files(const std::string &kind, int start, int count) {
    std::vector<fs::path> paths;
    if (fs::exists(config_.output_dir))
        for (auto &entry : fs::directory_iterator(config_.output_dir))
            if (entry.is_regular_file() && entry.path().filename().string().rfind("zt6_", 0) == 0 &&
                entry.path().extension() == (kind == "photo" ? ".jpg" : ".mkv"))
                paths.push_back(entry.path());
    std::sort(paths.rbegin(), paths.rend());
    Json list = Json::array();
    for (size_t i = size_t(start); i < paths.size() && list.size() < size_t(count); ++i)
        list.push_back({{"path", paths[i].string()}, {"bytes", fs::file_size(paths[i])}});
    return {{"storage", "onboard"}, {"total", paths.size()}, {"files", list}};
}
static std::string encode(const std::string &s) {
    std::ostringstream out;
    for (unsigned char c : s)
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out << c;
        else
            out << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << int(c);
    return out.str();
}
// 固件会拒绝字母排序的 count=...&media_type=...，显式按手册顺序拼接。
std::string media_path(const std::string &endpoint, const Json &params) {
    std::string url = "//cgi-bin/media.cgi/api/v1/" + endpoint + "?";
    auto append = [&](const std::string &key, const Json &value) {
        url += encode(key) + "=" +
               encode(value.is_string() ? value.get<std::string>() : value.dump()) + "&";
    };
    for (const auto *key : {"media_type", "path", "start", "count"})
        if (params.contains(key))
            append(key, params.at(key));
    for (auto it = params.begin(); it != params.end(); ++it)
        if (it.key() != "media_type" && it.key() != "path" && it.key() != "start" &&
            it.key() != "count")
            append(it.key(), it.value());
    if (url.back() == '&')
        url.pop_back();
    return url;
}
SDMediaFile sd_media_file(const Config &c, const std::string &url) {
    const auto origin = "http://" + c.host + ":" + std::to_string(c.media_port);
    if (url.rfind(origin + "/", 0) != 0)
        throw Error("SD 下载地址必须来自配置的相机文件服务");
    const auto path = url.substr(origin.size());
    const auto split = path.find('/', 1);
    const auto last = split == std::string::npos ? split : path.find('/', split + 1);
    if (split == std::string::npos || last == std::string::npos ||
        path.find('/', last + 1) != std::string::npos)
        throw Error("无效 SD 文件路径");
    const auto kind = path.substr(1, split - 1);
    const auto directory = path.substr(split + 1, last - split - 1);
    const auto name = path.substr(last + 1);
    const std::string allowed = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-";
    if ((kind != "photo" && kind != "mp4") || directory.empty() || name.empty() ||
        directory.find_first_not_of(allowed) != std::string::npos ||
        name.find_first_not_of(allowed) != std::string::npos ||
        directory.find("..") != std::string::npos || name.find("..") != std::string::npos)
        throw Error("无效 SD 文件路径");
    auto extension = fs::path(name).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return char(std::tolower(ch)); });
    if (kind == "photo" && (extension == ".jpg" || extension == ".jpeg"))
        return {path, name, "image/jpeg"};
    if (kind == "mp4" && extension == ".mp4")
        return {path, name, "video/mp4"};
    throw Error("SD 下载只支持相机照片 JPEG 和视频 MP4");
}
// 手册 Web Server 接口：HTTP 82，GET //cgi-bin/media.cgi/api/v1/...。
// 请求参数需 URL 编码，相机返回 HTTP 200 时仍要检查 JSON 的 code/success。
// 真机的 Content-Type 和报头空行只有 LF；CMake 为 httplib 开启 LF 兼容。
Json media_request(const Config &c, const std::string &endpoint, const Json &params) {
    httplib::Client client(c.host, c.media_port);
    client.set_connection_timeout(std::chrono::duration<double>(c.timeout));
    client.set_read_timeout(std::chrono::duration<double>(c.timeout));
    auto url = media_path(endpoint, params);
    auto reply = client.Get(url);
    if (!reply)
        throw Error("SD 文件接口请求失败（" + endpoint + "）：" +
                    httplib::to_string(reply.error()) + "；目标 " + c.host + ":" +
                    std::to_string(c.media_port));
    if (reply->status != 200)
        throw Error("SD 文件接口 HTTP " + std::to_string(reply->status) + "（" + endpoint +
                    "）；目标 " + c.host + ":" + std::to_string(c.media_port));
    auto data = Json::parse(reply->body);
    if (!data.value("success", true) || data.value("code", 200) != 200)
        throw Error("SD 文件接口失败（" + endpoint + "）：" + data.dump());
    return data;
}
// 当前固件 path="" 会返回空列表，所以先 getdirectories，再逐目录 getmedialist。
// skip 和 collected 实现跨目录分页；此接口只列文件，不删除或格式化。
Json sd_files(const Config &c, const std::string &kind, const std::string &path, int start,
              int count) {
    Json params{{"media_type", kind == "photo" ? 0 : 1},
                {"path", path},
                {"start", start},
                {"count", count}};
    if (!path.empty())
        return media_request(c, "getmedialist", params);
    auto dirs = media_request(c, "getdirectories",
                              {{"media_type", params["media_type"]}})["data"]["directories"];
    Json collected = Json::array();
    int skip = start;
    for (auto &dir : dirs) {
        int cursor = 0;
        while (collected.size() < size_t(count)) {
            int batch = std::min(1000, skip + count - int(collected.size()));
            auto p = params;
            p["path"] = dir["path"];
            p["start"] = cursor;
            p["count"] = batch;
            auto entries = media_request(c, "getmedialist", p)["data"]["list"];
            for (auto item : entries)
                if (skip)
                    --skip;
                else if (collected.size() < size_t(count)) {
                    item["directory"] = dir["path"];
                    collected.push_back(item);
                }
            cursor += int(entries.size());
            if (entries.size() < size_t(batch))
                break;
        }
        if (collected.size() >= size_t(count))
            break;
    }
    return {{"storage", "sd"},
            {"code", 200},
            {"success", true},
            {"data", {{"list", collected}, {"directories", dirs}, {"start", start}}}};
}

// 统一业务入口：先验证全局配置，再确定保存目标，最后分派到相应模块。
// stop 特意绕过普通控制锁，使另一条长操作占锁时仍能尝试停止云台方向运动。
Json execute(const Config &c, const Json &req, const std::function<bool()> &cancelled) {
    c.validate();
    std::string cmd = req.value("command", "");
    // 先校验数量，非法请求不取得控制锁、不建立相机连接。
    int photo_count =
        cmd == "photo" ? whole_number(req.value("count", Json(1)), 1, 100, "count") : 1;
    if (!zoom_controls_enabled && (cmd == "zoom" || cmd == "zoom-link"))
        throw Error("变倍及热像联动功能暂未启用");
    if (cmd == "stop") {
        SDK sdk(c);
        return sdk.stop();
    }
    // 后台轮询立即让路；方向脉冲也不排队，避免松手后才开始执行旧的移动请求。
    // 其他交互命令最多等 2 秒，足以跨过一次受限时长的后台状态查询。
    const bool no_wait = cmd == "move" || req.value("background_poll", false);
    Lock lock(c.state_dir, no_wait ? 0 : 2);
    Onboard onboard(c);
    auto config = read_json(c.state_dir / "config.json");
    std::string storage = req.value("storage", config.value("storage", "onboard"));
    if (storage != "sd" && storage != "onboard")
        throw Error("保存位置必须是 onboard 或 sd");
    if (cmd == "storage") {
        auto destination = req.value("destination", "");
        if (!destination.empty()) {
            if (destination != "sd" && destination != "onboard")
                throw Error("未知保存位置");
            if (onboard.status()["active"] == true)
                throw Error("机载正在录像，请先停止再切换保存位置");
            config["storage"] = destination;
            write_json(c.state_dir / "config.json", config);
        }
        return {{"default_storage", config.value("storage", "onboard")},
                {"onboard_directory", c.output_dir.string()}};
    }
    if (cmd == "files") {
        int start = req.value("start", 0), count = req.value("count", 20);
        std::string kind = req.value("kind", "photo");
        if (start < 0 || count < 1 || count > 1000 || (kind != "photo" && kind != "video"))
            throw Error("文件分页或类型无效");
        return storage == "onboard" ? onboard.files(kind, start, count)
                                    : sd_files(c, kind, req.value("path", ""), start, count);
    }
    if (cmd == "auto-record") {
        Settings settings(c);
        return settings.auto_record(req.value("action", "status"));
    }
    if (cmd == "status" || cmd == "doctor") {
        Json result{{"selected_storage", storage},
                    {"onboard", onboard.status()},
                    {"output_dir", c.output_dir.string()},
                    {"implementation", "cpp"}};
        result["zoom_enabled"] = zoom_controls_enabled;
        try {
            SDK sdk(c);
            result["camera"] = sdk.status();
            if (req.value("full", false) || cmd == "doctor") {
                try {
                    result["attitude"] = sdk.attitude();
                    result["video"] = sdk.view();
                } catch (const Error &e) {
                    result["telemetry_error"] = e.what();
                }
            }
            if (cmd == "doctor")
                result["firmware"] = sdk.version();
        } catch (const Error &e) {
            result["sdk_error"] = e.what();
        }
        if (cmd == "doctor") {
            auto probe =
                run_process({"ffprobe", "-v", "error", "-rtsp_transport", "tcp", "-show_entries",
                             "stream=codec_name,width,height", "-of", "json", c.stream_url()},
                            15);
            result["video_probe"] = probe.output;
        }
        return result;
    }
    if (cmd == "photo") {
        auto capture = [&] { return storage == "onboard" ? onboard.photo() : SDK(c).photo(); };
        if (photo_count == 1)
            return capture(); // 保留既有单张输出字段，避免破坏 path 等调用方式。
        Json results = Json::array();
        auto fail = [&](const std::string &reason, int failed_index = 0) {
            auto message = "连拍未完成：已确认 " + std::to_string(results.size()) + "/" +
                           std::to_string(photo_count) + " 张；" + reason;
            Json value{{"success", false},
                       {"status", interrupted ? "interrupted" : "error"},
                       {"message", message},
                       {"error", message},
                       {"storage", storage},
                       {"requested_count", photo_count},
                       {"completed_count", results.size()},
                       {"results", results}};
            if (failed_index)
                value["failed_index"] = failed_index; // 从 1 起；该张可能已拍但尚未确认。
            throw PhotoBurstError(value);
        };
        auto check_cancel = [&] {
            if (interrupted || (cancelled && cancelled()))
                fail("请求已中断或连接已断开，未发送后续拍照指令");
        };
        // 整批持有同一控制锁，避免拍摄期间被其他 CLI/Web 切换画面或保存位置。
        // 每张 SD 请求使用独立 UDP 连接，避免前一张迟到反馈混入下一张。
        for (int index = 1; index <= photo_count; ++index) {
            check_cancel();
            try {
                results.push_back(capture());
            } catch (const std::exception &e) {
                fail(e.what(), index); // 失败/未确认立即结束，不重发、不补拍。
            }
            if (index < photo_count) {
                double end = monotonic() + 1; // 确认后留 1 秒，非固定帧率高速连拍。
                while (monotonic() < end) {
                    check_cancel();
                    pause(std::min(.05, std::max(0.0, end - monotonic())));
                }
            }
        }
        return {{"success", true},
                {"status", "completed"},
                {"message", "连拍已完成"},
                {"storage", storage},
                {"requested_count", photo_count},
                {"completed_count", results.size()},
                {"results", results}};
    }
    if (cmd == "record") {
        auto action = req.value("action", "");
        if (action != "start" && action != "stop" && action != "status")
            throw Error("record 参数为 start/stop/status");
        double duration = req.value("seconds", 0.0);
        if (duration < 0 || !std::isfinite(duration) || duration > 86400 ||
            (duration && action != "start"))
            throw Error("定时秒数只用于 start，最多 86400 秒");
        if (storage == "onboard") {
            if (action == "status")
                return onboard.status();
            if (action == "stop")
                return onboard.stop();
            auto started = onboard.start();
            if (duration) {
                if (started["changed"] != true)
                    throw Error("已有机载录像，未接管定时停止");
                double end = monotonic() + duration;
                while (monotonic() < end && !interrupted)
                    pause(.05);
                auto stopped = onboard.stop();
                return {{"started", started}, {"stopped", stopped}};
            }
            return started;
        }
        SDK sdk(c);
        if (action == "status")
            return sdk.status();
        if (action == "stop")
            return sdk.record(false);
        auto started = sdk.record(true);
        if (duration) {
            if (started["changed"] != true)
                throw Error("已有 SD 录像，未接管定时停止");
            double end = monotonic() + duration;
            while (monotonic() < end && !interrupted)
                pause(.05);
            auto stopped = sdk.record(false);
            return {{"started", started}, {"stopped", stopped}};
        }
        return started;
    }
    SDK sdk(c);
    if (cmd == "gimbal-mode")
        return sdk.gimbal_mode(req.value("mode", "status"));
    if (cmd == "zoom")
        return sdk.zoom(req.value("ratio", Json(nullptr)));
    if (cmd == "zoom-link")
        return sdk.zoom_link(req.value("action", "status"));
    if (cmd == "resolution") {
        std::string stream_name = req.value("stream", "main");
        int stream = stream_name == "record" ? 0
                     : stream_name == "main" ? 1
                     : stream_name == "sub"  ? 2
                                             : -1;
        if (stream < 0)
            throw Error("stream 为 main/sub/record");
        Json options = Json::object();
        for (auto key : {"width", "height", "codec", "bitrate_kbps", "reapply"})
            if (req.contains(key))
                options[key] = req[key];
        if (!options.empty()) {
            if (onboard.status()["active"] == true)
                throw Error("机载正在录像，请先停止再修改相机编码参数");
            int recording = sdk.status()["record_state"];
            if (recording == 1 || recording == 3)
                throw Error("SD 正在录像，请先停止再修改相机编码参数");
        }
        return sdk.resolution(stream, options);
    }
    if (cmd == "thermal-output") {
        auto mode = req.value("mode", "status");
        if (mode != "status") {
            if (onboard.status()["active"] == true)
                throw Error("机载正在录像，请先停止再修改热像输出模式");
            int recording = sdk.status()["record_state"];
            if (recording == 1 || recording == 3)
                throw Error("SD 正在录像，请先停止再修改热像输出模式");
        }
        return sdk.thermal_output(mode);
    }
    if (cmd == "thermal-frame")
        return sdk.thermal_frame();
    if (cmd == "version")
        return sdk.version();
    if (cmd == "attitude")
        return sdk.attitude();
    if (cmd == "center")
        return sdk.center(number(req.value("wait_seconds", Json(10)), .1, 20, "wait-seconds"),
                          number(req.value("tolerance", Json(1)), .1, 5, "tolerance"));
    if (cmd == "angle")
        return sdk.angle(number(req.at("yaw"), -270, 270, "yaw"),
                         number(req.at("pitch"), -90, 25, "pitch"),
                         number(req.value("wait_seconds", Json(10)), .1, 20, "wait-seconds"),
                         number(req.value("tolerance", Json(1)), .1, 5, "tolerance"));
    if (cmd == "move")
        return sdk.move(int(number(req.value("yaw", Json(0)), -100, 100, "yaw")),
                        int(number(req.value("pitch", Json(0)), -100, 100, "pitch")),
                        number(req.value("seconds", Json(.2)), .01, 1, "seconds"));
    if (cmd == "view") {
        auto source = req.value("source", "status");
        if (source != "status" && onboard.status()["active"] == true)
            throw Error("机载正在录像，请先停止再切换画面");
        return sdk.view(source);
    }
    throw Error("未知命令：" + cmd);
}
} // namespace zt6
