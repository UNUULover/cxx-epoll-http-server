# ---- build stage ----
FROM ubuntu:24.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src

# 镜像里不跑单测（测试用的 GoogleTest 只在开发环境需要），保持运行镜像干净。
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DWS_BUILD_TESTS=OFF \
    && cmake --build build -j"$(nproc)"

# ---- runtime stage ----
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        libstdc++6 curl ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /src/build/ws_server /app/ws_server
COPY www /app/www

EXPOSE 8080

HEALTHCHECK --interval=10s --timeout=3s --start-period=3s --retries=3 \
    CMD curl -fsS http://127.0.0.1:8080/healthz || exit 1

# 容器里日志写 stdout（12-factor）：仍然走异步日志的同一条路径，只是 fd 从文件换成 fd 1。
ENTRYPOINT ["/app/ws_server"]
CMD ["-p", "8080", "-t", "4", "-d", "/app/www", "-i", "60", "-l", "-"]
