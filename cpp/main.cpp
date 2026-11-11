#include "core.hpp"
#include <iostream>

// C++ 命令行入口：解析参数后调用公共核心，避免 CLI 和 Web 的行为分叉。
// 命令帮助只描述本版接口；旧版 Python 部署不具备全部新增命令。
// 信号处理函数只设置标志，循环发现标志后结束动作，避免在信号处理里执行复杂操作。
static void cancel(int) {
    zt6::interrupted = 1;
}
static void help() {
    std::cout << R"(ZT6 C++ 控制台 v2
用法：zt6 [全局参数] 命令 [命令参数]
全局：--host IPv4 --port 37260 --media-port 82 --timeout 3 --state-dir PATH
      --output-dir PATH --rtsp URL --storage onboard|sd
命令：
  status [--full]       相机、存储、实时姿态和画面状态
  storage [onboard|sd]  查看/设置默认保存位置
  photo [--count N]    拍照/逐张连拍，默认 1 张，范围 1–100；确认后间隔 1 秒
  record start|stop|status [--seconds N]
  files [--kind photo|video] [--path SD目录] [--start 0] [--count 20]
  attitude             查询实际 yaw/pitch/roll，单位度
  angle --yaw N --pitch N   设置目标角度，到位后返回“已完成”
  center               一键回中，到位后返回“已完成”
  上述命令可选 --wait-seconds 10 --tolerance 1（秒、度；yaw ±270，pitch -90..25）
  move --yaw N --pitch N [--seconds 0.2]  短时方向控制，自动停止
  stop                 立即发送云台方向停止命令
  view [visible|thermal|pip|status]   切换可见光/热成像/拼接画面
  resolution [--stream main|sub|record]   查询相机编码参数
    设置可加 --width 1280 --height 720 [--codec h264|h265] [--bitrate-kbps 4000]
    主/副流设置 720p/1080p；录像流另支持 2K(2560x1440)/4K(3840x2160)
    --reapply 即使查询值相同也重新应用一次；不是 Web 预览宽度
  gimbal-mode [status|lock|follow|fpv]   查询/设置云台工作模式
  zoom / zoom-link     按用户要求暂未启用；SDK 实现保留
  thermal-output [status|video|temperature]  30fps视频 / 25fps+温度帧
  thermal-frame        请求一次温度帧，仅返回相机 ACK，不接收温度矩阵
  auto-record [on|off|status]        云台自动录像设置
  version              云台固件版本
  doctor               只读诊断
Web：bin/zt6-web；本机浏览器启动：bash scripts/start_web.sh
)";
}
int main(int argc, char **argv) {
    // 关闭已断开的管道不应直接杀死主进程；Ctrl+C/终止信号交给业务循环处理。
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, cancel);
    signal(SIGTERM, cancel);
    signal(SIGHUP, cancel);
    try {
        // config 是网络/目录等全局配置，request 是本次命令及业务参数。
        // 例如：angle --yaw 10 --pitch -5 -> command=angle, yaw=10, pitch=-5。
        zt6::Config config;
        zt6::Json request = zt6::Json::object();
        std::vector<std::string> positional;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            auto value = [&]() {
                if (++i >= argc)
                    throw zt6::Error("缺少参数：" + a);
                return std::string(argv[i]);
            };
            if (a == "--help" || a == "-h") {
                help();
                return 0;
            }
            if (a == "--host")
                config.host = value();
            else if (a == "--port")
                config.port = std::stoi(value());
            else if (a == "--media-port")
                config.media_port = std::stoi(value());
            else if (a == "--timeout")
                config.timeout = std::stod(value());
            else if (a == "--state-dir")
                config.state_dir = zt6::fs::absolute(value());
            else if (a == "--output-dir")
                config.output_dir = zt6::fs::absolute(value());
            else if (a == "--rtsp")
                config.rtsp = value();
            else if (a == "--storage")
                request["storage"] = value();
            else if (a == "--full")
                request["full"] = true;
            else if (a == "--yaw")
                request["yaw"] = std::stod(value());
            else if (a == "--pitch")
                request["pitch"] = std::stod(value());
            else if (a == "--seconds")
                request["seconds"] = std::stod(value());
            else if (a == "--wait-seconds")
                request["wait_seconds"] = std::stod(value());
            else if (a == "--tolerance")
                request["tolerance"] = std::stod(value());
            else if (a == "--stream")
                request["stream"] = value();
            else if (a == "--width")
                request["width"] = std::stoi(value());
            else if (a == "--height")
                request["height"] = std::stoi(value());
            else if (a == "--codec")
                request["codec"] = value();
            else if (a == "--bitrate-kbps")
                request["bitrate_kbps"] = std::stoi(value());
            else if (a == "--ratio")
                request["ratio"] = std::stod(value());
            else if (a == "--reapply")
                request["reapply"] = true;
            else if (a == "--start")
                request["start"] = std::stoi(value());
            else if (a == "--count") {
                auto text = value();
                size_t used = 0;
                int count = std::stoi(text, &used);
                if (used != text.size())
                    throw zt6::Error("count 必须是整数");
                request["count"] = count;
            } else if (a == "--kind")
                request["kind"] = value();
            else if (a == "--path")
                request["path"] = value();
            else if (a.rfind("--", 0) == 0)
                throw zt6::Error("未知参数：" + a);
            else
                positional.push_back(a);
        }
        // 第二个位置参数的意义由命令决定：record 的 action、storage 的 destination 等。
        if (positional.empty()) {
            help();
            return 0;
        }
        if (positional.size() > 2)
            throw zt6::Error("多余的命令参数");
        request["command"] = positional[0];
        if (positional.size() == 2) {
            auto cmd = positional[0];
            if (cmd == "record" || cmd == "auto-record" || cmd == "zoom-link")
                request["action"] = positional[1];
            else if (cmd == "storage")
                request["destination"] = positional[1];
            else if (cmd == "view")
                request["source"] = positional[1];
            else if (cmd == "gimbal-mode" || cmd == "thermal-output")
                request["mode"] = positional[1];
            else
                throw zt6::Error("此命令不接受第二个位置参数");
        }
        // 成功结果统一输出 JSON，Web 服务复用同样的数据结构。
        // 退出码：0 成功；2 参数/设备错误；130 用户中断。
        auto result = zt6::execute(config, request);
        if (zt6::interrupted)
            throw zt6::Error("命令已中断，请核对设备状态");
        std::cout << result.dump(2) << "\n";
        return 0;
    } catch (const zt6::PhotoBurstError &e) {
        std::cout << e.result.dump(2) << "\n";
        std::cerr << "错误：" << e.what() << "\n";
        return zt6::interrupted ? 130 : 2;
    } catch (const std::exception &e) {
        // 错误也向标准输出写 JSON，便于脚本统一读取 success；stderr 保留人工提示。
        // 不把非零退出码改成成功：错误为 2，收到中断为 130。
        std::cout << zt6::Json{{"success", false},
                               {"status", zt6::interrupted ? "interrupted" : "error"},
                               {"message", e.what()},
                               {"error", e.what()}}
                         .dump(2)
                  << "\n";
        std::cerr << "错误：" << e.what() << "\n";
        return zt6::interrupted ? 130 : 2;
    }
}
