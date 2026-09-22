# ws-server

一个从零实现的 Linux C++ 高并发 HTTP 服务器：不依赖任何第三方网络库，自己实现
事件循环、协议解析、定时器、内存池和异步日志。

目标不是"跑分"，而是把 Linux 网络编程里那几个真正会被追问的点（epoll 的触发模式、
跨线程唤醒、连接生命周期、定时器堆、日志不阻塞主链路）都落到能解释清楚的代码上。

---

## 当前状态（重要，别跳过）

| 项 | 状态 |
|---|---|
| 编译（g++ 13.3.0 / `-Wall -Wextra`） | ✅ 零错误零告警 |
| 单元测试（GoogleTest，6 个 suite / 43 个用例） | ✅ 全部通过 |
| 功能冒烟（静态文件 / echo / 400 / pipelining / 分片 body / 空闲回收） | ✅ 全部通过 |
| 压测（wrk，含 nginx 对照） | ✅ 已跑，见 `docs/BENCHMARKS.md` |
| 空闲定时器改为「每连接仅一个待触发条目」 | ⚠️ **已改代码，尚未重新编译验证** |

最后一行是本文档写就时留下的状态：那次改动（`Connection::armIdleTimer()` /
`touch()`）改完后环境里的 shell 就不可用了，**没有重新编译和跑测试**。
重新验证只需要一条命令，见下面「构建与运行」。

---

## 架构

主从 Reactor（main-sub reactor）：

```
                    ┌──────────────────────────────┐
   新连接 ────────► │  主 Reactor（主线程）          │
                    │  只有 listen fd，只做 accept   │
                    └───────────────┬──────────────┘
                                    │ 轮询分发 + eventfd 唤醒
              ┌─────────────────────┼─────────────────────┐
              ▼                     ▼                     ▼
      ┌───────────────┐     ┌───────────────┐     ┌───────────────┐
      │ 子 Reactor 1  │     │ 子 Reactor 2  │     │ 子 Reactor N  │
      │ epoll + 定时器 │     │ epoll + 定时器 │     │ epoll + 定时器 │
      │ 一批连接       │     │ 一批连接       │     │ 一批连接       │
      └───────────────┘     └───────────────┘     └───────────────┘
```

`-t 0` 时退化成**单 Reactor**（主线程既 accept 也处理连接 IO）。
两种模式共用同一份代码，只差一个命令行参数 —— 这样做架构对比实验时自变量是干净的。

```
include/ws/          头文件
src/
  buffer.cpp         读写缓冲区（readv 一次读尽、空间回收复用）
  logger.cpp         异步日志（阻塞队列 + 双缓冲）
  memory_pool.cpp    定长块内存池 + 线程本地 size class
  timer.cpp          最小堆定时器（懒删除取消）
  epoller.cpp        epoll 薄封装
  event_loop.cpp     子 Reactor：epoll + eventfd + 定时器 + 连接表
  http.cpp           HTTP 请求/响应对象与序列化
  http_parser.cpp    HTTP/1.1 有限状态机解析器
  connection.cpp     单条连接：读→解析→响应→写
  server.cpp         主 Reactor：accept、分发、静态文件与业务端点
  main.cpp           命令行入口、信号处理
tests/               GoogleTest 用例
www/                 静态文件根目录
docs/BENCHMARKS.md   压测方法与结果
```

---

## 关键设计决策（面试就聊这些）

### 1. 新连接必须通过 eventfd 交给子 Reactor，不能直接 `epoll_ctl`

主线程 accept 到 fd 后，不能自己去调 `epoll_ctl` 把它塞进子 Reactor 的 epoll 实例：

- `epoll_ctl` 不是线程安全的；
- 即使塞进去了，目标线程此刻很可能正阻塞在 `epoll_wait` 上，不会立刻看到新 fd。

所以走 `eventfd`：主线程往里面写 8 字节把子线程唤醒，子线程醒来后在**自己的线程里**
执行 `pending_` 队列中的任务（注册 fd、装定时器）。所有涉及该 epoll 实例的操作
因此只发生在 loop 线程内 —— 这也正是 `EventLoop` 里那些数据成员一律不加锁的原因。

> 追问「为什么用 eventfd 而不是 pipe」：eventfd 只有一个 fd、内核里就是个计数器，
> 读写各一次系统调用，比 pipe 少一个 fd、少一次缓冲区拷贝。

### 2. 监听 fd 用 LT，连接用 ET

- **监听 socket 用水平触发**：一次没 accept 完，下次 `epoll_wait` 还会继续报可读，
  不会漏连接，代码也更简单。
- **连接用边缘触发**：配合 `handleRead()` 里"读到 EAGAIN 为止"的循环，
  一个事件就能把一批数据全部处理掉，减少 `epoll_wait` 轮次。
  ET 的前提是 fd 非阻塞 —— 所以 accept 用的是 `accept4(..., SOCK_NONBLOCK)`。

> 追问「ET 模式下漏读会怎样」：socket 缓冲区里残留数据后，**不会**再收到可读通知，
> 这条连接就永久卡住了。所以 ET 的读必须循环到 EAGAIN，这是 ET 最容易踩的坑。

### 3. 空闲定时器：每个连接同时只有一个待触发条目（改过一版）

**错误做法**（很多人第一版会这么写，也是这个项目最初的实现）：每次收到数据就
`cancel` 掉旧定时器、再 `add` 一个新的。问题是取消是懒删除 —— 被取消的条目仍留在堆里，
要等到 60 秒超时才被弹出。在 1 万 QPS 下，堆会以每秒 1 万个的速度堆积废弃条目，
60 秒后接近 80 万个：内存随**请求数**无界增长，`push` 的 `O(log n)` 也一起变差。

**现在的做法**：每个连接同时只有一个待触发定时器。

- 收到数据时只更新一个时间戳（`touch()`），完全不碰定时器；
- 定时器到期时先看时间戳：真的空闲了才关连接，否则**直接重排一次**
  （旧条目已经被弹出，不需要 cancel）。

代价是每个连接每 `idle_timeout` 会多跑一次回调，收益是定时器堆的大小只跟
**连接数**有关，跟请求数无关。

`GET /stats` 里的 `pending_timers` 就是用来观察这一点的指标。

### 4. `send()` 里先直接试写一次

`send()` 把响应 append 进输出缓冲区后立刻调一次 `handleWrite()`：

- 写完了 → 不注册 `EPOLLOUT`，整个请求处理过程**一次 `epoll_ctl` 都不需要**；
- 没写完（对端窗口满）→ 才挂上 `EPOLLOUT` 等可写事件。

小响应场景下这是热路径上省掉两次系统调用（一加一减 `epoll_ctl`）。

### 5. HTTP 解析是状态机，不是一个正则

```
REQUEST_LINE ──► HEADERS ──► [BODY] ──► COMPLETE
     │              │           │
     └──────────────┴───────────┴──► ERROR
```

三个状态对应报文三段。每次 `parse()` 从**上次停下的状态**继续，数据没到齐就返回
`INCOMPLETE`。一个请求可能被拆成十几个 TCP 段陆续到达，每次都从头重扫是 `O(n²)`。
状态机只把已经解析掉的部分消费掉，天然支持增量。

同一个 `parse()` 内部用循环推进状态，所以"请求已完整到达"（绝大多数情况）时一次调用走完全程。

### 6. 异步日志用双缓冲

- `current_`：业务线程正在追加的缓冲区
- `to_write_`：日志线程正在落盘的那一份

两者由一把锁保护着 `swap()` 换手。落盘期间业务线程继续写 `current_`，不会阻塞在磁盘 IO 上。
时间戳和级别在**调用线程**上格式化好，所以日志行记录的是事件发生的时刻，
而不是落盘线程稍后处理的时刻 —— 排查并发问题时这个差别很关键。

### 7. 内存池

每次请求都要分配释放一批小对象（缓冲区节点、头部字符串）。glibc malloc 每条路径
都要加锁、查 bin，可能触发 `brk`/`mmap`。

`MemoryPool` 一次向系统要一大块（chunk），切成等长 block 串成空闲链表，
`allocate`/`deallocate` 退化成两次指针操作。按 size class 存放**线程本地**池，
所以同线程内的分配完全不加锁。

`tests/test_memory_pool.cpp` 里有一条断言给出可复现的数字：
`MemoryPool(128, 512)` 发出 10000 个块只对应 **20 次**系统分配。

---

## 构建与运行

```bash
# 依赖（Ubuntu 24.04）
sudo apt-get install -y build-essential cmake libgtest-dev

# 构建
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

# 单元测试
./build/ws_tests

# 运行：4 个子 Reactor，静态文件根目录 ./www，空闲 60 秒回收
./build/ws_server -p 8080 -t 4 -d ./www -i 60 -l /tmp/ws.log

# 单 Reactor 模式（用于架构对比）
./build/ws_server -p 8080 -t 0 -d ./www -i 60 -l /tmp/ws.log
```

命令行参数：

| 参数 | 说明 | 默认 |
|---|---|---|
| `-p, --port` | 监听端口，0 表示由内核分配 | 8080 |
| `-t, --threads` | 子 Reactor 线程数，**0 = 单 Reactor** | 0 |
| `-d, --doc-root` | 静态文件根目录 | `./www` |
| `-i, --idle` | 空闲连接回收秒数，0 = 关闭 | 60 |
| `-m, --max-conn` | 最大并发连接数 | 20000 |
| `-l, --log` | 日志文件，`-` 表示 stdout | `-` |
| `-v, --log-level` | trace / debug / info / warn / error | info |

### 端点

| 路径 | 说明 |
|---|---|
| `GET /` | 静态文件（默认 `www/index.html`） |
| `GET /hello` | 固定的内存内响应，用于压测时隔离网络栈开销 |
| `GET /stats` | 运行统计 JSON（含 `pending_timers`） |
| `POST /echo` | 回显请求体 |
| 其他 | 目录穿越返回 403，文件不存在返回 404，方法不支持返回 405 |

### 手工验证清单

```bash
curl -i http://127.0.0.1:8080/hello
curl -i -X POST -d 'hello' http://127.0.0.1:8080/echo
curl -s http://127.0.0.1:8080/stats
curl -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8080/nope      # 404
curl -o /dev/null -w '%{http_code}\n' -X DELETE http://127.0.0.1:8080/ # 405
```

---

## 已知限制（面试被问到就直说，别硬撑）

1. **不支持 `Transfer-Encoding: chunked`**。解析到会明确返回 400/501，而不是静默解析错。
   实现它需要把 body 状态再拆成 `chunk-size / chunk-data / trailer` 三个子状态。
2. **没有 HTTP/2、没有 TLS**。这两件事各自都是独立的大工程。
3. **静态文件没有 `sendfile`/mmap**，是读进用户态再写的，多一次拷贝。
4. **没有做 writev 攒批**：多个小响应分开写。可以用 `writev` 把响应头和 body 合并成一次系统调用。
5. **`SO_REUSEPORT` 多进程模式没做**。多进程各自 accept 是另一条扩展路线（nginx 的做法）。
6. **没有超时重传 / 业务线程池**。当前所有业务逻辑在 IO 线程里跑，
   CPU 密集型的业务会拖慢同一 loop 上的其他连接 —— 正确做法是再接一个计算线程池。
