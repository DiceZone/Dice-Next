#pragma once
// AI 后台工作线程 —— AI 网关是同步 curl（最长 30s），此前直接跑在适配器消息
// 线程上，一次超时就让全部指令失效 30 秒。所有可能调大模型的路径（对话/NPC/图像
// 识别/润色/翻译/向量检索）改为投递到这里执行。
//
// 用**单线程串行队列**而非线程池：既让消息管线永不阻塞，又保证 AI 任务彼此不并发
//（工具调用会经指令路由掷骰/查卡，路由内部有按条消息的一次性状态，不宜多线程互踩）。
// 队列有界：AI 持续超时堆积时直接丢弃新任务（对话不回、润色由调用方回退原文），
// 绝不反压消息线程。

#include <condition_variable>
#include <cstddef>
#include <chrono>
#include <map>
#include <functional>
#include <mutex>
#include <thread>
#include "../common/logger.h"

namespace dice::aiwork {

class Worker {
public:
    static Worker& instance() {
        static Worker w;
        return w;
    }

    /// 入队执行；队列已满返回 false（调用方自行降级：丢弃或原地执行）。
    bool post(std::function<void()> job) {
        return postAt(std::chrono::steady_clock::now(), std::move(job));
    }

    using Clock = std::chrono::steady_clock;
    bool postAt(Clock::time_point due, std::function<void()> job) {
        std::lock_guard lock(m_);
        if (stopping_ || q_.size() >= 16) return false;
        q_.emplace(due, std::move(job));
        if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
        cv_.notify_one();
        return true;
    }

    void stop() {
        {
            std::lock_guard lock(m_);
            stopping_ = true;
            q_.clear();
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    ~Worker() { stop(); }

private:
    void run() {
        std::unique_lock lock(m_);
        while (!stopping_) {
            if (q_.empty()) { cv_.wait(lock); continue; }
            auto due = q_.begin()->first;
            if (due > Clock::now()) { cv_.wait_until(lock, due); continue; }
            auto job = std::move(q_.begin()->second);
            q_.erase(q_.begin());
            lock.unlock();
            try { job(); } catch (...) { DICE_LOG_EXCEPTION("AI background job"); }
            lock.lock();
        }
    }
    std::mutex m_;
    std::condition_variable cv_;
    std::multimap<Clock::time_point, std::function<void()>> q_;
    std::thread thread_;
    bool stopping_ = false;
};

}  // namespace dice::aiwork
