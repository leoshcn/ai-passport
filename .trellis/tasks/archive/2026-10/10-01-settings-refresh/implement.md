# 实施顺序

先改 `xhs_logic` 的枚举和按键规则并固定主机测试，再改设置画面的四行布局。不要新增网络接口。

## Checklist

1. 在 `XHS_ITEM_PERIOD` 与 `XHS_ITEM_REPROVISION` 之间加入立即刷新，并让 `XHS_ITEM_COUNT` 仍表示项数。
2. 导航态一次确定选中该项时切到仪表盘；仅 `config_ok` 时置 `request_fetch`。不进入调整态，不置 `save_settings` 或 `request_reprovision`。
3. 调整态碰到这一行时，上下键不改自动更新和频率。
4. 更新 `tests/test_xhs_logic.c`：从自动更新向下两格是立即刷新，三格才是重新配网。覆盖已配网的一次确定、未配网的一次确定，以及另外三项的原手势。
5. `xhs_ui.c` 按设计画出四行，文案字面量包含「立即刷新」，右侧为空。提示语位置保持可读。
6. `tests/test_xhs_app.py` 的界面文案清单加入「立即刷新」。字库已含这四个字，缺字测试失败时才重新生成字库。
7. 更新 `main/main.c` 开头的按键说明。拉取仍走现有 `request_fetch` 分支。

## Validation

迭代时编译并运行 `tests/test_xhs_logic.c` 所在的主机测试，以及 `tests/test_xhs_app.py`。交付前跑：

```bash
./tools/validate.sh --static
```

有 ESP-IDF 5.5.3 时再跑 `./tools/validate.sh --firmware`。编译成功不算板级通过。真机还需看：四行和提示语是否同时完整，「立即刷新」是否一次确定回到数据页，已配网时数字是否随后更新，未配网时是否不发请求，重新配网是否仍要第二次确定。

## Risky files

- `main/xhs_logic.c`：枚举插入会改变「再按两次向下就是重新配网」的现有测试。调整态若把新行当成频率，上下键会把每天改成每小时。
- `main/xhs_ui.c`：行高未压缩时，第四行盖住「长按确认键返回」。
- `tests/test_xhs_logic.c`：与进行中的待机任务改同一文件。本任务只加设置项用例，不加入双击或背光断言。

## Rollback points

- 第 4 步之后，状态规则已由主机测试固定，画面仍可以是旧的三行。
- 第 5 步若真机排版溢出，只回调卡片的 y 和高度，不改拉取门闩。
