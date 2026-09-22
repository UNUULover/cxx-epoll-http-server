# 下一步：验证与收尾清单

按顺序做。第 1 步是**必须**的，其它按需。

---

## 1. 重新编译并跑全部测试（必做，一条命令）

2026-09-22 的最后一次代码改动（空闲定时器改为"每连接仅一个待触发条目" + `/stats`
新增 `pending_timers`）**没有编译验证过** —— 改完之后那台机器上的 shell 就不能用了。
在 VM 里执行：

```bash
cd ~/webserver && rm -rf build \
  && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  && cmake --build build -j"$(nproc)" \
  && ./build/ws_tests --gtest_color=no 2>&1 | tail -15
```

期望：编译零告警，测试 **43 个用例全部通过**（如果定时器改动有编译错误，报错行会直接指出来，
把报错发我）。

冒烟测试：

```bash
cd ~/webserver && ./build/ws_server -p 18080 -t 2 -d ./www -i 5 -l /tmp/s.log &
sleep 1
curl -s -o /dev/null -w 'hello=%{http_code}\n' http://127.0.0.1:18080/hello
curl -s -o /dev/null -w 'echo=%{http_code}\n' -X POST -d 'x' http://127.0.0.1:18080/echo
curl -s http://127.0.0.1:18080/stats; echo
# 等 6 秒，确认空闲连接被回收（timeouts_closed 应该 +1）
sleep 6
curl -s http://127.0.0.1:18080/stats; echo
kill %1
```

---

## 2. 验证定时器那个改动的实际效果

这是那次改动的验收指标。**改完之后**，`pending_timers` 应该约等于连接数，
不随请求数增长：

```bash
cd ~/webserver && ./build/ws_server -p 18081 -t 2 -d ./www -i 60 -l /tmp/t.log &
sleep 1
curl -s http://127.0.0.1:18081/stats; echo          # pending_timers 约 1
wrk -t2 -c100 -d15s http://127.0.0.1:18081/hello >/dev/null
curl -s http://127.0.0.1:18081/stats; echo          # pending_timers 应该还是 100 上下
kill %1
```

如果这个数涨到几万，说明改动没生效（或改错了），需要回看 `Connection::armIdleTimer()`。

> 想看"改之前会怎样"的话，把 `handleRead()` 里的 `touch()` 换回"每次 cancel + addAfter"
> 再跑一遍同样命令 —— 那个数会跟着请求数线性上涨。这个对比很适合面试时讲。

---

## 3. 验证 Docker 与 CI，然后才能加那条简历

`Dockerfile` / `docker-compose.yml` / `deploy/nginx.conf` / `.github/workflows/ci.yml`
都写好了，但**一次都没实际跑过**。跑通之前不要往简历上写这两条。

```bash
# CI 的等价命令（本地跑一遍）
cd ~/webserver && ./build/ws_tests --gtest_color=no

# Docker（VM 里需要装 docker；或者在你笔记本上装 Docker Desktop 再跑）
docker compose up --build -d
curl -s http://127.0.0.1:8080/stats            # 经 nginx 反代打到后端实例
docker compose logs --tail 20 lb
docker compose down
```

跑通之后，可以在简历项目一里加一条：

> · **工程化**：GoogleTest 43 个用例 / 6 个测试套件全部通过，CMake 构建在 -Wall -Wextra 下零告警；
> 提供 Dockerfile 与 docker-compose（Nginx 反向代理 + 3 个服务实例）以及 GitHub Actions CI

---

## 4. 想要一个能打的架构对比，需要换环境

现在的 2 vCPU 环境下，**单 Reactor 和主从 Reactor 测不出差别**，原因是 `-t 2` 时
服务端有 3 条线程（1 accept + 2 子 Reactor）+ wrk 2 条线程，全挤在 2 个核上，是超订的。

要做出有意义的对比，任选其一：

- **把 VM 提到 4 vCPU**（宿主机 20 核，够用）。做法：关机 → 虚拟机设置 → 处理器改成 4 → 开机。
  之后 `-t 0` 和 `-t 2` 才有可比性。
- **把压测端挪到另一台机器**（最理想）。VM 网络改成桥接模式，在另一台机器上跑 wrk，
  这样服务端独占 VM 的两个核。

拿到数据后更新 `docs/BENCHMARKS.md` 第二节，并把简历里"对照实验"那条改成真正的对比结论。

---

## 5. 简历本身还有 5 个事实要你自己确认

1. **专业排名**：前 19% 还是前 20%？原文两个都写了，只留一个。
2. **HCIA 的方向**（Datacom / openEuler / GaussDB…）—— 你是投华为的人，这张证书没写方向很浪费。
3. **项目一的时间**：现在写的是 2026.06 – 2026.09，按实际改。
4. **项目二（驾驶后视镜）的起始月份**。
5. **六级过了吗**？只有 CET-4 在大厂简历里偏弱。

另外简历里那个 `【确认架构…】` 标记已经删掉了 —— 因为项目已经真做出来了，
主从 Reactor + eventfd 唤醒是实际实现，不再是待确认的说法。

---

## 6. 把代码交出去

建议把它开源，简历上放 GitHub 链接（面试官会点开看）。

```bash
cd ~/webserver
git init
git add .
git commit -m "ws-server: a from-scratch Linux C++ epoll HTTP server"
# 推到 GitHub 后在简历顶部加一行
```

**推之前检查**：`www/` 里不要放真实密码或内网地址；`bench/` 已经在 `.gitignore` 里。
