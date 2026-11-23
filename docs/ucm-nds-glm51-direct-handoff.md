# UCM NDS GLM-5.1 Direct 适配交接

> 日期：2026-11-23
>
> 私仓分支：`feat/nds-store-glm51-direct`
>
> 基线：`fix/pynds-kv-ipc-types` 的 `ecafa1a`
>
> 当前状态：第一版原生实现已完成，WSL 编译和 fake 回归通过；真实 NDS 链接、盘框 I/O 和 GLM 端到端验证尚未执行。
>
> 原始设计：根仓库 `docs/ucm-nds-glm51-direct-adaptation.md`。

## 1. 本轮范围与结论

本轮在 UCM 私仓实现了 GLM-5.1、vLLM-Ascend、direct whole-block 场景的原生 `Cache|Nds` 后端，复用 Cache2 既有设备布局和 D2H/H2D 路径，不重写 Connector 或模型数据布局。

依赖边界已经确定：

- **不需要 Python `pynds_kv` 包，也不在热路径经过 Python/GIL。**
- `libndsstore.so` 直接调用 `libndskv.so` 中的 C++ `NDS::initMulti/isExists/batchGet/batchPut` 80-bit 接口。
- 默认 `BUILD_UCM_NDS=OFF`，普通 UCM 构建不依赖 NDS。
- 开启时通过 `NDS_ROOT` 或 `NDS_INCLUDE_DIR`、`NDS_LIBRARY_DIR` 提供头文件和库目录；这些路径不写入源码或 YAML。
- 运行配置只包含 `nds_config_path`，不包含 `.so` 路径。`libndskv.so` 及其 SDK 依赖通过 `$ORIGIN` 或 `LD_LIBRARY_PATH` 发现。
- `NDS::` 接口暴露 `std::vector`，生产构建时必须保证编译器、libstdc++ 和 `_GLIBCXX_USE_CXX11_ABI` 与 `libndskv.so` 一致。

没有修改 pynds 源码，也没有修改 NDS SDK shutdown。

## 2. 已实现内容

### 2.1 原生 NdsStore

新增 `ucm/store/nds/`：

- `KeyEncoder`：`"UCM_NDS_KEY_V1" || 16B block || shard_index_u64_le`，SHA256 后取前 10 字节，拆成 `key_low` 和 `key_high`。
- `NdsStore`：实现 `StoreV1` 的 Lookup、LookupOnPrefix、LookupOnReverse、Load、Dump、Check、Wait、CheckHealth。
- 普通 Lookup 按 block 独立查询；Prefix 使用 block-major shard 前缀；Reverse 从尾部查找第一个完整 block。
- `TransManager`：Load/Dump 独立 worker、队列背压、异步任务完成与失败传播。
- NDS 非零返回和 C++ 异常都会使任务失败；Load 失败不会修改目标 slot，也不会让 Cache2 标 Ready。
- `INdsClient` 隔离真实 NDS 与 fake client；`RealNdsClient` 将异常归一成非零返回。

### 2.2 Direct MR 与 staging

设 `S=shard_size`、`A=lcm(read_batch, write_batch)`：

- Direct 要求 `value_size==S` 且 `S % A == 0`。
- Direct 地址必须属于当前 Store 注册的 span，不能跨 MR 或跨 slot。
- 不满足 direct 条件时，`direct` 策略显式进入 staging，`direct_only` 直接报错。
- Staging 使用 `V=align_up(S, A)`，wrapper per-nsid `value_size` 必须等于 V。
- Dump：有效 payload 拷贝到 staging 后写 NDS；Load：NDS 成功后才把前 S 字节复制回 Cache2 slot。
- staging 是进程共享 MR，包含独立 control 区域供 warmup/health 使用，不占业务 Cache2 slot。
- `nds_wrapper.conf` 从 `nds_config.conf` 同目录读取，支持全局和 `value_size_kb_nsid<N>` 配置。

### 2.3 Cache2 host span 导出

新增 `Detail::HostBufferProvider/Consumer` 和 `HostBufferSpan`：

- `Cache2::Store` 实现 Provider，`NdsStore` 实现 Consumer。
- 导出所有 rank 的真实 memfd host 地址、实际映射长度、slotBytes 和 slotCount，不只导出本 rank。
- Pipeline 只连接相邻的原始 Store，健康包装器不妨碍连接。
- Cache2 发往 backend 的 task 已显式设置 `memoryType=DDR`，不再依赖默认 HBM 猜测。

### 2.4 注册、健康和退出生命周期

- 多 Store 先收集 span，之后全进程只调用一次 `NDS::initMulti`。
- warmup 成功后才进入 ACTIVE；初始化失败会锁存，不伪装成 cache miss。
- ACTIVE 后禁止新增业务 MR；重复 finalize 幂等。
- 同一 nsid 不允许两个 valueSize。
- Scheduler lookup-only 只使用 control/staging MR，禁止 Load/Dump。
- HostBufferConsumer 的健康包装器延迟到 finalize 后启动，避免初始化竞态。
- Pipeline 销毁时先停止健康探测并 drain NDS，再释放 Cache2 映射。
- `NdsRuntime`、staging 和 `libndskv.so` 采用进程级生命周期，防止 Python/`dlclose` 收尾时提前释放 SDK 仍依赖的资源。
- **ACTIVE 后只允许在进程退出阶段销毁 Cache|Nds，不支持同进程热销毁或热重建。** SDK 的 `destroy_all_connection()` 存在，但 poll 线程、detached reconnect 和重复 init 状态尚未闭合，本轮没有把它当成可靠 shutdown API 使用。

### 2.5 Connector 接入

- 注册 `Cache|Nds` builder。
- `UcmPipelineStore.finalize_registration()` 暴露给引擎。
- 当前 vLLM Connector 每侧只创建一个 Store，因此在 `_create_store()` 返回前 finalize。
- Scheduler 按 GLM direct whole-block 的 `N=1` 补齐 `shard_size=block_size`。
- 若未来扩展多 Pool，需要把 finalize 移到“本进程全部 Pool/Store 创建完成之后”，不能简单在每个 Pool 初始化后各自 finalize；多 shard Scheduler 也需要显式传递真实 shard 几何，不能沿用当前的 N=1 假设。

## 3. 主要文件

| 文件 | 作用 |
|---|---|
| `ucm/store/nds/cc/nds_store.*` | StoreV1 接口与参数校验 |
| `ucm/store/nds/cc/nds_runtime.*` | 进程级注册状态机、direct/staging 路由、warmup/health |
| `ucm/store/nds/cc/nds_client.*` | 真实 NDS C++ 接口薄封装 |
| `ucm/store/nds/cc/key_encoder.*` | 确定性 80-bit key |
| `ucm/store/nds/cc/trans_manager.*`、`trans_task.h` | 异步任务、worker、失败传播 |
| `ucm/store/nds/cc/staging_arena.*` | 共享 staging/control MR 与 lease |
| `ucm/store/nds/cc/nds_config.*` | wrapper 几何配置解析 |
| `ucm/store/detail/host_buffer.h` | Provider/Consumer 公共接口 |
| `ucm/store/cache/v2/` | Cache2 host span 导出、DDR 标记 |
| `ucm/store/pipeline/` | Cache|Nds、Attach/Finalize、析构前 drain |
| `ucm/integration/vllm/ucm_connector.py` | Store 创建后 finalize、Scheduler N=1 几何 |
| `examples/ucm_config_ndsstore.yaml` | 运行配置示例，不含 `.so` 路径 |
| `ucm/store/test/case/nds/` | fake client、C++ 测试和 link-only Real adapter |

构建辅助改动：

- `BUILD_UCM_NDS`：生产 NDS 插件开关。
- `BUILD_UCM_NDS_FAKE_TESTS`：无 `libndskv.so` 时构建 NDS core/fake 测试。
- `BUILD_UCM_PIPELINE`：默认 ON，可在纯原生 fake 测试中关闭 Pipeline。
- `ndscore` 是 OBJECT library，生产 `libndsstore.so` 会包含 `MakeNdsStore` 导出符号。
- 给项目原有纯头文件 OBJECT 库显式设置 `LINKER_LANGUAGE=CXX`，解决 CMake 4.4 配置错误。

## 4. 已完成验证

### 4.1 WSL 原生构建与 fake 测试

环境：Ubuntu 24.04 WSL、Zig 0.17.1（CMake 识别 Clang 22.1.8）、CMake/Ninja。没有安装 CUDA、NPU 或 NDS 运行时。

结果：

- `ndsstore.test` 编译、链接成功。
- `ctest -R Nds`：**12/12 通过**。
- `libcachestore.so` 编译、链接成功。
- `ucmpipelinestore` 编译、链接成功。
- 真实 `NDS::` 客户端 `nds_client.cc` 单独编译成功。

覆盖：SHA256 测试向量、direct 冷写热读、Lookup/Prefix/Reverse、Load 失败保护、多 Store 一次 init、Scheduler lookup-only、非对齐 staging、warmup/control、nsid valueSize 冲突、staging lease。

本地增量构建目录：`analysis_tmp/ucm-nds-build`。本地工具在 `.venv-wsl`，未进入 Git。

```bash
# 已有 WSL 构建目录中复跑
python .agents/skills/wsl-linux-testing/scripts/wsl_run.py --shell -- \
  'export PATH=/mnt/c/project/L3e2e/.venv-wsl/bin:$PATH; \
   cmake --build analysis_tmp/ucm-nds-build --target ndsstore.test -j 4; \
   ctest --test-dir analysis_tmp/ucm-nds-build -R Nds --output-on-failure'
```

### 4.2 Python 回归

均使用 `--noconftest` 隔离全局可选 `pynvml` 依赖：

- `test/test_load_task_options.py`：**10/10 通过**。
- `test/test_cp_topology.py`：**28/28 通过**，包括新增的 Scheduler finalize 和 N=1 shard_size 用例。
- `py_compile`、YAML 解析和 `git diff --check` 通过。

```bash
python .agents/skills/wsl-linux-testing/scripts/wsl_run.py --shell -- \
  '.venv-wsl/bin/python -m pytest -q --noconftest \
   open_source/_git_ucm/test/test_load_task_options.py \
   open_source/_git_ucm/test/test_cp_topology.py'
```

## 5. 尚未验证与服务器执行清单

### 5.1 生产构建

先确保 pynds 已生成 `vllm/NDS_bin/libndskv.so`，且 UCM 与 pynds 的 C++ ABI 一致：

```bash
cd /path/to/unified-cache-management
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DRUNTIME_ENVIRONMENT=ascend \
  -DBUILD_UCM_NDS=ON \
  -DBUILD_UNIT_TESTS=ON \
  -DNDS_ROOT=/path/to/pynds_kv
cmake --build build --target ndsstore cachestore ucmpipelinestore ndsstore.test -j"$(nproc)"
ctest --test-dir build -R Nds --output-on-failure
```

本轮只完成了真实客户端的编译，**没有完成 `libndsstore.so` 与真实 `libndskv.so` 的最终链接**。

### 5.2 Direct 冷写热读

1. 按实际 S 配置盘框 `valueSize` 和 wrapper `value_size_kb_nsid<N>`。
2. 先使用 `nds_mr_policy=direct_only`，证明 direct 几何和 MR 命中，排除隐式 staging。
3. 运行冷写、热读，确认输出与 Posix/全重算基线一致。
4. 再切到 `direct`，验证特殊地址能明确回退而不是失败。

### 5.3 重启、并发和故障

- 进程重启后的前缀命中和输出一致性。
- TP8、多请求并发、首 miss、中间 miss、部分 shard 缺失。
- NDS 错误、盘框断连与恢复；Load 失败不得标 Ready 或继续 H2D。
- staging 满、队列满、关闭时仍有任务。
- 不在持续运行的进程中销毁并重建 NDS client；退出时按 drain 顺序验证。

### 5.4 128K 性能

分别记录：全重算 prefill、剩余计算、NDS 读取、D2H/H2D、Connector 调度、排队和最终 TTFT。56% token 命中并不等价于 TTFT 改善 60%，需要按实测分解判断。

## 6. 新对话建议开场

```text
请继续 UCM 私仓 open_source/_git_ucm 的 feat/nds-store-glm51-direct 分支，先读 docs/ucm-nds-glm51-direct-handoff.md。

当前已完成 Cache|Nds 第一版原生实现：不依赖 Python pynds_kv，直接调用 libndskv.so 的 NDS:: 80-bit C++ API；Cache2 host span 导出、一次 initMulti、direct/staging、Connector finalize、异步任务和 fake 测试均已落地。WSL 的 12 个 NDS C++ 用例和 38 个 Python 用例全部通过，Cache2/Pipeline 与真实客户端编译通过。

尚未完成真实 libndskv.so 链接、盘框 I/O 和 GLM 端到端验证。请先核对当前分支和文档，再在服务器执行生产构建、direct_only 冷写热读、重启命中、TP8 并发与故障测试；不要改写成 Python backend，不要绕过 Cache2 direct 布局，也不要把现有 SDK destroy_all_connection() 当作可靠热重建接口。
```
