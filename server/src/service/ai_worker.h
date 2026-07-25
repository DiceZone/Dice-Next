#pragma once
// C#89：AI 后台工作线程 —— AI 网关是同步 curl（最长 30s），此前直接跑在适配器消息
// 线程上，一次超时就让全部指令失效 30 秒。所有可能调大模型的路径（对话/NPC/图像
// 识别/润色/翻译/向量检索）改为投递到这里执行。
//
// 用**单线程串行队列**而非线程池：既让消息管线永不阻塞，又保证 AI 任务彼此不并发
//（工具调用会经指令路由掷骰/查卡，路由内部有按条消息的一次性状态，不宜多线程互踩）。
// 队列有界：AI 持续超时堆积时直接丢弃新任务（对话不回、润色由调用方回退原文），
// 绝不反压消息线程。

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace dice::aiwork {

class Worker {
public:
    static Worker& instance() {
        static Worker w;
        return w;
    }

    /// 入队执行；队列已满返回 false（调用方自行降级：丢弃或原地执行）。
    bool post(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lk(m_);
            if (q_.size() >= kMaxQueue) return false;
            q_.push_back(std::move(job));
            if (!started_) {   // 首次使用才起线程；进程生命周期内常驻
                started_ = true;
                std::thread([this] { run(); }).detach();
            }
        }
        cv_.notify_one();
        return true;
    }

private:
    static constexpr std::size_t kMaxQueue = 16;

    void run() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this] { return !q_.empty(); });
                job = std::move(q_.front());
                q_.pop_front();
            }
            try { job(); } catch (...) {}
        }
    }

    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    bool started_ = false;
};

}  // namespace dice::aiwork
