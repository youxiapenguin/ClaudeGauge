#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
TrafficMonitor 插件用的探针：在 WSL 里抓 claude /usage，解析后只输出 key=value。
DLL 插件每隔几分钟调用一次：  wsl python3 tm_usage_probe.py
输出（UTF-8，每行一项）：
    SESSION_PCT=7
    SESSION_RESET=10:10pm
    WEEKALL_PCT=16
    WEEKALL_RESET=Jun 5, 7pm
    WEEKSONNET_PCT=0
    WEEKSONNET_RESET=
    OK=1
最后一行 OK=1 表示抓取成功；失败则 OK=0 且带 ERR=...。
"""
import os, pty, time, select, struct, fcntl, termios, sys, re

COLS, ROWS = 150, 50

def ensure_pyte():
    """没装 pyte 就自动装；智能选源：国内优先清华/阿里、国外优先官方，互相兜底。"""
    try:
        import pyte  # noqa
        return
    except ImportError:
        pass
    import subprocess

    def _is_china():
        try:
            tz = open("/etc/timezone").read().strip()
            if any(k in tz for k in ("Shanghai", "Chongqing", "Urumqi", "Harbin", "PRC")):
                return True
        except Exception:
            pass
        try:
            return time.strftime("%z") == "+0800"
        except Exception:
            return False

    tuna = ["-i", "https://pypi.tuna.tsinghua.edu.cn/simple"]
    ali  = ["-i", "https://mirrors.aliyun.com/pypi/simple"]
    plans = ([tuna, ali, []] if _is_china() else [[], tuna])   # 国内：镜像优先；国外：官方优先
    base = [sys.executable, "-m", "pip", "install", "--user", "--break-system-packages",
            "--quiet", "--default-timeout", "12", "pyte"]
    for extra in plans:
        try:
            subprocess.run(base + extra, timeout=90, check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            import importlib
            importlib.invalidate_caches()
            import pyte  # noqa
            return
        except Exception:
            continue
    # 都失败也不在这里报错；capture() 里 import pyte 会抛 -> 上层输出 OK=0


def capture():
    ensure_pyte()
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"
        os.environ["COLUMNS"] = str(COLS); os.environ["LINES"] = str(ROWS)
        os.execvp("claude", ["claude"])
        os._exit(1)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))
    import pyte
    screen = pyte.Screen(COLS, ROWS)
    stream = pyte.ByteStream(screen)

    def drain(seconds):
        end = time.time() + seconds
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.2)
            if r:
                try:
                    data = os.read(fd, 65536)
                except OSError:
                    return
                if not data:
                    return
                stream.feed(data)

    drain(9)
    os.write(fd, b"/usage")
    time.sleep(1.0)
    os.write(fd, b"\r")
    drain(8)
    try:
        os.write(fd, b"\x1b")
        time.sleep(0.3)
        os.kill(pid, 9)
    except OSError:
        pass
    return "\n".join(screen.display)


def parse(text):
    lines = [l.rstrip() for l in text.splitlines()]
    def grab(label):
        for i, line in enumerate(lines):
            if label.lower() in line.lower():
                pct, reset = None, ""
                for w in lines[i + 1:i + 4]:
                    if "current " in w.lower():
                        break
                    m = re.search(r'(\d+)\s*%\s*used', w)
                    if m and pct is None:
                        pct = int(m.group(1))
                    r = re.search(r'Resets\s+(.+?)(?:\s*\(|$)', w)
                    if r and not reset:
                        reset = r.group(1).strip()
                return pct, reset
        return None, ""
    return grab("Current session"), grab("week (all models)"), grab("week (Sonnet only)")


def detect_plan():
    """从 ~/.claude/.credentials.json 读套餐等级（只取等级字段，不碰 token）。"""
    import json, os
    try:
        p = os.path.expanduser("~/.claude/.credentials.json")
        with open(p, "r", encoding="utf-8") as f:
            d = json.load(f)
        o = d.get("claudeAiOauth", d)
        tier = (o.get("rateLimitTier") or "").lower()
        sub = (o.get("subscriptionType") or "").lower()
        if "max_20x" in tier: return "Claude Max 20x"
        if "max_5x" in tier:  return "Claude Max 5x"
        if sub == "pro":      return "Claude Pro"
        return "Claude Max"
    except Exception:
        return "Claude Max"


_MONTHS = {'jan':1,'feb':2,'mar':3,'apr':4,'may':5,'jun':6,
           'jul':7,'aug':8,'sep':9,'oct':10,'nov':11,'dec':12}

def remain_minutes(reset_str):
    """把 '10:10pm' / 'Jun 5, 7pm' 这种重置时刻算成"距现在还剩多少分钟"。算不出返回 -1。"""
    import datetime
    if not reset_str:
        return -1
    s = reset_str.strip()
    now = datetime.datetime.now()
    # A: 只有时间 "10:10pm" / "7pm"
    m = re.match(r'^(\d{1,2})(?::(\d{2}))?\s*([ap]m)$', s, re.I)
    if m:
        hh, mm, ap = int(m.group(1)), int(m.group(2) or 0), m.group(3).lower()
        if ap == 'pm' and hh != 12: hh += 12
        if ap == 'am' and hh == 12: hh = 0
        t = now.replace(hour=hh, minute=mm, second=0, microsecond=0)
        if t <= now: t += datetime.timedelta(days=1)
        return int((t - now).total_seconds() // 60)
    # B: "Jun 5, 7pm" / "Jun 5, 6:59pm"
    m = re.match(r'^([A-Za-z]{3,})\s+(\d{1,2}),\s*(\d{1,2})(?::(\d{2}))?\s*([ap]m)$', s, re.I)
    if m:
        mo = _MONTHS.get(m.group(1)[:3].lower(), now.month)
        day, hh, mm, ap = int(m.group(2)), int(m.group(3)), int(m.group(4) or 0), m.group(5).lower()
        if ap == 'pm' and hh != 12: hh += 12
        if ap == 'am' and hh == 12: hh = 0
        try:
            t = datetime.datetime(now.year, mo, day, hh, mm)
        except ValueError:
            return -1
        if t <= now:
            try: t = datetime.datetime(now.year + 1, mo, day, hh, mm)
            except ValueError: return -1
        return int((t - now).total_seconds() // 60)
    return -1


def main():
    out = sys.stdout
    out.write("PLAN=" + detect_plan() + "\n")
    try:
        text = capture()
        s, wa, ws = parse(text)
        if s[0] is None:
            raise RuntimeError("usage panel not parsed")
        out.write(f"SESSION_PCT={s[0]}\n")
        out.write(f"SESSION_RESET={s[1]}\n")
        out.write(f"WEEKALL_PCT={wa[0] if wa[0] is not None else ''}\n")
        out.write(f"WEEKALL_RESET={wa[1]}\n")
        out.write(f"WEEKSONNET_PCT={ws[0] if ws[0] is not None else ''}\n")
        out.write(f"WEEKSONNET_RESET={ws[1]}\n")
        out.write(f"SESSION_REMAIN={remain_minutes(s[1])}\n")
        out.write(f"WEEKALL_REMAIN={remain_minutes(wa[1])}\n")
        out.write(f"WEEKSONNET_REMAIN={remain_minutes(ws[1])}\n")
        out.write("OK=1\n")
    except Exception as e:
        out.write("OK=0\n")
        out.write("ERR=" + str(e).replace("\n", " ")[:120] + "\n")

if __name__ == "__main__":
    main()
