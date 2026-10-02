# Settings refresh action

The settings screen is a three-item list on a 240×320 panel. Rows are 208×56 cards at y=64, 128, and 192. The hint sits at y=268 (`main/xhs_ui.c`). A fourth 56 px card starting at y=256 ends at y=312 and covers that hint. Four rows fit if each card is about 46 px tall with a 6 px gap, starting at y=56: the last card ends at y=258, and the existing hint can stay at y=268.

Menu order in `xhs_item_t` is also the up/down order. `XHS_ITEM_COUNT` is the modulus. Inserting the new action between period and reprovision keeps reprovision last. `tests/test_xhs_logic.c` currently reaches reprovision with two DOWN clicks from the default selection; that becomes three.

OK in navigation always enters edit mode (`main/xhs_logic.c`). Reprovision is the only item that consumes a second OK as a command. The new row must not enter edit mode: one OK in navigation sets the view to the dashboard and, when `config_ok` is already true, sets `request_fetch`. `main.c` already calls `xhs_net_request_fetch()` when that flag is set and the net task is up, then calls `present()`. No new net entry point is required.

`config_ok` is false during hotspot, joining, and pairing. Dashboard OK already refuses to fetch in that state. Returning to the dashboard without the flag matches that gate. Long-press from the dashboard still returns through `xhs_view_for_phase()`, so the hotspot screen remains reachable.

The glyphs U+7ACB, U+5373, U+5237, and U+65B0 are already in `assets/fonts/xhs_font_16.c`. `tests/test_xhs_app.py` rebuilds the required CJK set from `main/xhs_ui.c` string literals, so the new label must be a literal in that file.

`10-01-power-standby` is in progress and also edits `main/xhs_logic.c` and `tests/test_xhs_logic.c`. It does not add settings rows. This task must not add double-click or backlight behavior.
