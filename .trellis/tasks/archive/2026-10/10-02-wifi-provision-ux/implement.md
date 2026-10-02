# 实施顺序

先把与 ESP-IDF 无关的合并和表单解析定下来，再改热点页面。页面生成失败不应挡住手动提交。

## Checklist

1. 在 `xhs_logic` 增加扫描结果合并：空名称丢弃、同名保留更强 RSSI、按 RSSI 降序、最多 12 条。用 `tests/test_xhs_logic.c` 覆盖打乱顺序、重复名称、空名称和超过 12 条。
2. 解析 `ssid_manual`：非空则替换 `ssid`。两者都空则失败。把 `XHS_SETUP_BODY_MAX` 改为 800，并保留「超过上限即拒绝」的测试。
3. 在 `xhs_net` 的网络任务里做一次非阻塞扫描。`SCAN_DONE` 后取最多 32 条记录，合并进缓存，释放临时数组。连接尝试期间不扫描。
4. 用分块响应替换 `PORTAL_PAGE` 和 `/status` 的纯文本。`/?rescan=1` 只置重新扫描标志。探测 GET 不扫描。HTML 转义 SSID。密码不写日志、不回显。
5. 改 `docs/xhs-dashboard.md`、`docs/xhs-dashboard.zh_CN.md`、`backend/README.md`、`backend/README.zh_CN.md` 中对配网页的描述。

## Validation

迭代时跑主机测试。交付前跑：

```bash
./tools/validate.sh --static
```

本机有 ESP-IDF 5.5.3 时再跑 `./tools/validate.sh --firmware`。编译成功不算板级通过。

板上未覆盖项：手机打开列表、重新扫描时热点仍可打开页面、隐藏网络走手动输入、错误密码可回到列表、正确密码后热点关闭。

## Risky files

- `main/xhs_net.c`：扫描和 HTTP 在同一块无线电上。扫描只能由网络任务发起，不能放进 `portal_get`。
- `main/xhs_logic.h`：正文上限变大后，确认 `setup_post` 的栈缓冲仍在 8192 字节的 httpd 栈里。
- `main/xhs_logic.c`：`ssid_manual` 必须在 `xhs_setup_form_ok` 之前生效，且不能把密码留在失败路径的结构体里。

## Rollback points

- 第 2 步之后，主机测试已固定表单行为，固件仍是旧页面。
- 第 4 步如果页面内存不够，保留第 2 步的解析，页面退回只含手动输入的同一套样式，不把扫描留在 HTTP 处理函数里。
