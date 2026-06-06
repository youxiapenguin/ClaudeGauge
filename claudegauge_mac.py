#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ClaudeGauge for macOS —— 菜单栏显示 Claude Code 用量

依赖：
    pip3 install rumps pyte
前提：
    macOS 上已安装并登录过 Claude Code（`claude` 命令行），能联网。

运行：
    python3 claudegauge_mac.py
（cg_probe.py 必须和本文件在同一目录。）

菜单栏会显示当前会话用量（如 “Claude 13%”），点开下拉看三项 + 重置时间。
Mac 是原生跑 claude，不需要 WSL；探针 cg_probe.py 通用。
"""
import os
import time
import threading
import subprocess

try:
    import rumps
except ImportError:
    raise SystemExit("缺少 rumps，请先运行：pip3 install rumps pyte")

PROBE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cg_probe.py")
REFRESH_OK = 600      # 成功后 10 分钟刷一次
REFRESH_FAIL = 60     # 失败后 60 秒重试（自动重连）


class ClaudeGauge(rumps.App):
    def __init__(self):
        super().__init__("Claude …", quit_button=rumps.MenuItem("退出"))
        self.m_plan = rumps.MenuItem("Claude")
        self.m_session = rumps.MenuItem("会话(5h)：--")
        self.m_week = rumps.MenuItem("本周：--")
        self.m_sonnet = rumps.MenuItem("Sonnet：--")
        self.menu = [self.m_plan, None, self.m_session, self.m_week, self.m_sonnet, None]
        self._data = {}
        threading.Thread(target=self._worker, daemon=True).start()

    # 后台线程：跑探针，把结果存起来（耗时操作不放主线程，避免菜单卡顿）
    def _worker(self):
        while True:
            kv = self._probe()
            self._data = kv
            time.sleep(REFRESH_OK if kv.get("OK") == "1" else REFRESH_FAIL)

    def _probe(self):
        try:
            out = subprocess.run(
                ["python3", PROBE], capture_output=True, text=True, timeout=150
            ).stdout
        except Exception:
            return {}
        kv = {}
        for line in out.splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                kv[k] = v
        return kv

    # 主线程定时把缓存刷到界面
    @rumps.timer(2)
    def _tick(self, _):
        kv = self._data
        if not kv:
            return
        s = kv.get("SESSION_PCT", "")
        w = kv.get("WEEKALL_PCT", "")
        n = kv.get("WEEKSONNET_PCT", "")
        sr = kv.get("SESSION_RESET", "")
        wr = kv.get("WEEKALL_RESET", "")
        nr = kv.get("WEEKSONNET_RESET", "")
        ok = kv.get("OK") == "1"

        self.title = (f"Claude {s}%" if (ok and s != "") else "Claude --")
        self.m_plan.title = kv.get("PLAN", "Claude")
        self.m_session.title = f"会话(5h)：{s or '--'}%" + (f"   重置 {sr}" if sr else "")
        self.m_week.title = f"本周：{w or '--'}%" + (f"   重置 {wr}" if wr else "")
        self.m_sonnet.title = f"Sonnet：{n or '--'}%" + (f"   重置 {nr}" if nr else "")


if __name__ == "__main__":
    ClaudeGauge().run()
