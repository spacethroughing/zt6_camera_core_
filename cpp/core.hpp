#pragma once
// ZT6 C++ 控制核心；真机验收须在飞机重新开机后单独进行。
// 建议先读 docs/程序结构与SDK对应说明.md，再按本文件声明定位 core.cpp。
// 本文件定义公共接口；命令行 main.cpp 和 Web 服务 web.cpp 共用这些接口。
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace zt6 {
// 按用户要求暂不开放变倍/联动。保留 SDK 实现供离线测试，CLI/Web 统一拒绝入口。
inline constexpr bool zoom_controls_enabled = false;
using Json = nlohmann::json;
using Bytes = std::vector<uint8_t>;
namespace fs = std::filesystem;
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};
// 连拍中途失败仍需把已确认的照片列表交给 CLI/Web，不能只留下一个错误字符串。
struct PhotoBurstError : Error {
    Json result;
    explicit PhotoBurstError(const Json &value)
        : Error(value.at("message").get<std::string>()), result(value) {}
};
extern volatile sig_atomic_t interrupted;
// 使用单调时钟计算超时，避免系统校时使控制动作意外延长。
double monotonic();
void pause(double seconds);
std::string read_text(const fs::path &path);
Json read_json(const fs::path &path);
// 状态先写临时文件，再原子替换，降低断电/中断造成半份 JSON 的概率。
void write_json(const fs::path &path, const Json &data);
std::string timestamp();
std::string process_identity(int pid);
// 同时核对 PID、Linux 启动时刻、输出文件参数，避免停止到复用 PID 的其他进程。
bool matches_process(const Json &metadata);
// 公开 SDK 使用 CRC16；官方应用内部配置协议使用另一种 CRC32，二者不能混用。
uint16_t crc16(const Bytes &data);
uint32_t crc32(const Bytes &data);
Bytes packet(uint8_t cmd, const Bytes &data = {}, uint16_t sequence = 0, uint8_t control = 1);
Bytes inner_packet(uint8_t cmd, const Bytes &data = {}, uint16_t sequence = 0);
struct Frame {
    uint8_t control, command;
    Bytes data;
};
std::vector<Frame> parse_frames(const Bytes &datagram);
// TCP 没有报文边界：保留 buffer 中尚未收全的数据，下一次接收后继续解析。
std::vector<Frame> parse_inner(Bytes &buffer);
std::string hex(const Bytes &bytes);

// host 是云台地址 192.168.144.25；100.64.0.56 是飞机的 SSH 管理地址。
// state_dir 放配置/进程信息；output_dir 放保存在机载电脑上的 JPEG/MKV。
struct Config {
    std::string host = "192.168.144.25";
    int port = 37260;
    int media_port = 82; // SD 文件 HTTP 端口；默认 82，可为转发/离线测试覆盖。
    double timeout = 3;
    fs::path state_dir, output_dir, workspace;
    std::string rtsp;
    Config();
    void validate() const;
    std::string stream_url() const;
};
// 通过 Linux flock 协调本用户启动的不同 CLI/GUI 控制请求。
// RAII：构造时加锁，离开作用域自动释放；不能阻止遥控器等外部控制源。
class Lock {
    int fd_ = -1;

  public:
    // wait_seconds=0 立即尝试；交互命令可短时等待，避免被后台轮询瞬时抢锁。
    explicit Lock(const fs::path &directory, double wait_seconds = 0);
    ~Lock();
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;
};
// 公开 SDK：UDP 37260，帧头 55 66，CRC16，字段采用小端字节序。
// 每个对象持有自己的连接，只接收来自目标相机 IP/端口的数据。
class SDK {
    int fd_ = -1;
    uint16_t sequence_ = 0;
    double timeout_;
    std::vector<int> feedback_;
    Json wait_attitude(double yaw, double pitch, int mounting, double wait_seconds,
                       double tolerance);
    // 设置只发送一次；这里重复的仅是只读查询，直至读回目标或超时。
    Bytes confirm(uint8_t command, const Bytes &data,
                  const std::function<bool(const Bytes &)> &matches, const std::string &name);

  public:
    explicit SDK(const Config &config);
    ~SDK();
    SDK(const SDK &) = delete;
    SDK &operator=(const SDK &) = delete;
    void send(uint8_t command, const Bytes &data = {});
    std::vector<Frame> receive(double deadline);
    void drain();
    // 清理旧报文 -> 发送一次 -> 等待同 CMD_ID 且带 ACK 标志的有效响应。
    // 已观察到当前相机不回显请求 SEQ，因此不使用 SEQ 相等作为匹配条件。
    Bytes query(uint8_t command, const Bytes &data = {});
    // 下列注释中的页数为 vendor/ZT6_external_SDK_0.1.0.pdf 的 PDF 页序号。
    // 0x0A，PDF 第 6 页；record_state：0 停止、1 录像、2 无卡、3 写卡异常。
    Json status();
    // 0x01，PDF 第 3 页；固件字段每四字节一组，高字节不作为版本数字。
    Json version();
    // 0x0D，PDF 第 8 页；有符号 int16，小端，除以 10 得到角度/角速度。
    Json attitude();
    // 0x08，PDF 第 5 页；ACK 后继续查询 0x0D，稳定到位才返回 completed。
    Json center(double wait_seconds = 10, double tolerance = 1);
    // 0x0E，PDF 第 8 页；参数单位为度，发送前乘 10 后编码成 int16。
    // ZT6 手册规定 yaw -270..270、pitch -90..25；ACK 后等待真实反馈到位。
    Json angle(double yaw, double pitch, double wait_seconds = 10, double tolerance = 1);
    // 0x07，PDF 第 5 页；这是速度控制，不是角度控制。
    // 单次最多 1 秒，正常结束或异常退出时都尝试发送速度 0,0。
    Json move(int yaw, int pitch, double seconds);
    Json stop();
    // 0x10 查询 / 0x11 设置，PDF 第 9 页；3=可见光、7=热成像、0=拼接。
    // 修改的是相机主码流来源，正在录像时拒绝切换，以免同一视频混入不同来源。
    Json view(const std::string &source = "status");
    // 0x20/0x21：相机编码参数，stream 0=录像、1=主码流、2=副码流。
    // 不是浏览器缩放；可选 width/height、codec、bitrate_kbps，缺省保留原值。
    // 主/副流 720p/1080p；用户手册第 70 页另定义录像流 2K/4K。
    Json resolution(int stream, const Json &options = Json::object());
    // 0x19 查询；0x0C/03、04、05 分别设置锁定、跟随、FPV，并读回确认。
    Json gimbal_mode(const std::string &mode = "status");
    // 0x16 最大倍率、0x18 当前倍率、0x0F 绝对变倍；按当前设备范围校验。
    // ZT6 为数字变倍，此接口不宣称自动对焦。
    Json zoom(const Json &ratio = nullptr);
    // 0x0A DATA[7] / 0x0C 0A：联动开关为 toggle，先读状态且只切换一次。
    Json zoom_link(const std::string &action = "status");
    // 0x33 查询 / 0x34 设置：video=30fps，temperature=25fps+温度帧。
    Json thermal_output(const std::string &mode = "status");
    // 0x35 只返回单帧请求 ACK；不把 ACK 当作已经收到逐像素温度数据。
    Json thermal_frame();
    // 0x0C/00，完成结果由 0x0B/00 或失败反馈表示；超时不自动重拍。
    Json photo();
    // 0x0C/02 是“切换录像”，不是分别独立的 start/stop 指令。
    // 先查询状态，只切换一次，再观察状态是否稳定；不把状态成功当作文件落盘证明。
    Json record(bool start);
    Json wait_record(int wanted, double settle = 3.0);
};
// 官方应用内部配置协议，仅用于已经定位到的“自动录像”开关。
// TCP 37256，帧头 55 66 AA BB，0x85 查询、0x86 写入，CRC32。
// 这些编号来自官方 UniGCS 分析和旧版真机验证，不属于公开外部 SDK 表格。
class Settings {
    int fd_ = -1;
    Bytes buffer_;
    double timeout_;
    uint16_t sequence_ = 0;
    void send(uint8_t command, const Bytes &data = {});

  public:
    explicit Settings(const Config &config);
    ~Settings();
    bool read_auto_record();
    Json auto_record(const std::string &action);
};
struct ProcessResult {
    int code;
    std::string output;
};
// 使用参数数组启动 FFmpeg，不经过 shell，文件名不会被解释成 shell 命令。
// detached=true 时尝试创建独立会话，使机载录像可在 SSH 断开后继续。
int spawn_process(const std::vector<std::string> &args, int stdout_fd, int stderr_fd,
                  bool detached = true);
ProcessResult run_process(const std::vector<std::string> &args, double timeout = 20);
void finish_process(int pid, int signal = SIGTERM);
std::vector<std::string> ffmpeg_input(const Config &config);

// 机载保存：通过 RTSP 拉流，文件存在 RK3588 文件系统中，不使用云台 SD 拍摄命令。
// photo 为视频截帧；start/stop 管理 FFmpeg 录像进程，视频直接封装为 MKV。
class Onboard {
    Config config_;
    fs::path prepare(const std::string &extension);

  public:
    explicit Onboard(const Config &config) : config_(config) {}
    Json status();
    Json photo();
    Json start();
    Json stop();
    Json files(const std::string &kind, int start, int count);
};
// 老固件按参数位置解析，必须先 media_type，再 path/start/count，不能直接遍历排序 JSON。
std::string media_path(const std::string &endpoint, const Json &params);
struct SDMediaFile {
    std::string path, name, content_type;
};
// 只接受配置相机 HTTP 文件目录的 URL，避免将下载接口变成任意网络代理。
SDMediaFile sd_media_file(const Config &config, const std::string &url);
Json media_request(const Config &config, const std::string &endpoint, const Json &params);
// 云台 SD 文件列表走 HTTP 82 的 media.cgi；空目录查询在当前固件上有兼容问题，
// 因此先读目录，再逐目录取文件列表，并在程序中完成跨目录分页。
Json sd_files(const Config &config, const std::string &kind, const std::string &path, int start,
              int count);
// 统一业务入口：按 request.command 分派到 SDK、Settings 或 Onboard。
// CLI / Web 共用入口。普通操作先加锁，方向 stop 单独直发。
// cancelled 用于 Web 连接断开时停止后续连拍；已发出的当前单张仍需等待其结果。
Json execute(const Config &config, const Json &request,
             const std::function<bool()> &cancelled = {});
double number(const Json &value, double low, double high, const std::string &name);
} // namespace zt6
