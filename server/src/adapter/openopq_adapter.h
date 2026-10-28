#pragma once

// 内置登录（实验性）：由 DN 自己拉起并管理 openopq，经它本机端口上的 OneBot v11
// 正向 WS（/onebot/v11/ws）收发。消息、群管理等全部沿用 OneBotV11Adapter；这里只多管
// 一个子进程。
//
// 默认走正向 WS：openopq v1.3.0-rc.1 根本没有反向连接，只在自己的端口上提供正向 WS，
// 之后的版本也保留了这个入口。反向 WS（openopq 连回 DN，DN 只听本机、握手须带令牌）
// 留作可切换的方式，给支持它的客户端用。

#include "onebot_v11_adapter.h"
#include "../service/openopq_runtime.h"

namespace dice {

class OpenOpqAdapter : public OneBotV11Adapter {
public:
    using OneBotV11Adapter::OneBotV11Adapter;

    std::string version() const override { return "openopq"; }

    bool configure(const json& config) override {
        params_.adapterId = id();
        params_.apiKey = config.value("heartApiKey", std::string());
        params_.account = config.value("account", std::string());
        params_.consolePort = config.value("consolePort", 0);
        params_.transport = config.value("transport", std::string(openopq::kTransportForward));
        if (!openopq::validTransport(params_.transport)) params_.transport = openopq::kTransportForward;
        params_.token = config.value("accessToken", std::string());
        try { params_.wsPort = std::stoi(config.value("endpoint", std::string())); } catch (...) { params_.wsPort = 0; }
        json base = config;
        base["managedPeer"] = true;
        if (params_.transport == openopq::kTransportReverse) {
            base["connectionMode"] = "reverse_ws";
            base["listenLoopback"] = true;
            base["requireToken"] = true;
        } else {
            base["connectionMode"] = "forward_ws";
            base["endpoint"] = "ws://127.0.0.1:" + std::to_string(params_.consolePort);
            base["wsPath"] = "/onebot/v11/ws";
            base["accessToken"] = "";
        }
        return OneBotV11Adapter::configure(base);
    }

    bool start() override {
        if (!openopq::validApiKey(params_.apiKey) || !openopq::validAccount(params_.account) ||
            params_.consolePort <= 0) {
            return false;
        }
        if (params_.transport == openopq::kTransportReverse) {
            if (params_.token.empty() || params_.wsPort <= 0) return false;
            // 先开反向 WS 监听，openopq 登录后才会连进来
            if (!OneBotV11Adapter::start()) return false;
            openopq::Runtime::instance().launch(params_);
            return true;
        }
        // 先拉起进程；正向 WS 连不上时每 3 秒重试，openopq 起来就接上
        openopq::Runtime::instance().launch(params_);
        return OneBotV11Adapter::start();
    }

    void stop() override {
        openopq::Runtime::instance().stop(params_.adapterId);
        OneBotV11Adapter::stop();
    }

private:
    openopq::LaunchParams params_;
};

}  // namespace dice
