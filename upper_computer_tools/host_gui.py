#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import queue
import shutil
import subprocess
import threading
import time
import tkinter as tk
from datetime import datetime
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
import webbrowser

import host_cli

APP_BG = "#f3efe6"
CARD_BG = "#fffaf2"
BORDER = "#d8cfbf"
TEXT = "#1f2933"
MUTED = "#6b7280"
ACCENT = "#1f6f5f"
ACCENT_ACTIVE = "#175346"
WARN = "#b04b2a"
LOG_BG = "#f9f5ee"

PRESET_COLORS = [
    ("海盐蓝", (76, 158, 245)),
    ("薄荷绿", (56, 178, 121)),
    ("番茄红", (232, 83, 71)),
    ("琥珀黄", (240, 173, 78)),
]

ACTION_TYPE_OPTIONS = [
    ("none", "无动作"),
    ("url", "打开网页"),
    ("open", "打开文件/软件"),
    ("command", "执行命令/脚本"),
]
ACTION_TYPE_LABELS = [label for _, label in ACTION_TYPE_OPTIONS]
ACTION_TYPE_TO_LABEL = dict(ACTION_TYPE_OPTIONS)
ACTION_LABEL_TO_TYPE = {label: key for key, label in ACTION_TYPE_OPTIONS}


def detect_local_tz_minutes() -> int:
    offset = datetime.now().astimezone().utcoffset()
    if offset is None:
        return 480
    return int(offset.total_seconds() // 60)


class KeyboardGuiApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("Mini Keyboard 图形化上位机")
        self.root.geometry("1180x760")
        self.root.minsize(1024, 700)
        self.root.configure(bg=APP_BG)
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

        self.ui_queue: queue.Queue[tuple] = queue.Queue()
        self.is_busy = False
        self.port_meta: dict[str, str] = {}
        self.busy_widgets: list[tk.Widget] = []
        self.readonly_widgets: list[tk.Widget] = []
        self.config_path = Path(__file__).with_name("custom_actions.json")
        self.monitor_lock = threading.RLock()
        self.monitor_stop = threading.Event()
        self.monitor_thread: threading.Thread | None = None
        self.monitor_serial = None
        self.monitor_port: str | None = None
        self.monitor_running = False

        self.port_var = tk.StringVar()
        self.port_hint_var = tk.StringVar(value="先刷新串口，或直接识别设备。")
        self.status_var = tk.StringVar(value="就绪")
        self.clock_var = tk.StringVar()
        self.hex_var = tk.StringVar(value="#4C9EF5")
        self.monitor_state_var = tk.StringVar(value="监听未启动")
        self.monitor_button_var = tk.StringVar(value="启动 USB 监听")
        self.tz_var = tk.IntVar(value=detect_local_tz_minutes())
        self.debug_var = tk.BooleanVar(value=False)
        self.r_var = tk.IntVar(value=76)
        self.g_var = tk.IntVar(value=158)
        self.b_var = tk.IntVar(value=245)
        self.action_slot_vars: dict[int, dict[str, tk.StringVar]] = {}
        self._init_action_vars()

        self._build_styles()
        self._build_layout()
        self._wire_events()
        self._refresh_clock()
        self._refresh_color_preview()
        self.load_action_config()
        self.refresh_system_ports()
        self.root.after(100, self._process_ui_queue)

    def _init_action_vars(self):
        for slot in range(1, 10):
            self.action_slot_vars[slot] = {
                "type": tk.StringVar(value=ACTION_TYPE_TO_LABEL["none"]),
                "target": tk.StringVar(value=""),
            }

    def _build_styles(self):
        style = ttk.Style()
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass

        style.configure("App.TFrame", background=APP_BG)
        style.configure("Card.TFrame", background=CARD_BG)
        style.configure("CardTitle.TLabel", background=CARD_BG, foreground=TEXT, font=("Microsoft YaHei UI", 16, "bold"))
        style.configure("Body.TLabel", background=CARD_BG, foreground=TEXT, font=("Microsoft YaHei UI", 10))
        style.configure("Muted.TLabel", background=CARD_BG, foreground=MUTED, font=("Microsoft YaHei UI", 10))
        style.configure("Accent.TButton", font=("Microsoft YaHei UI", 10, "bold"), padding=(14, 8))
        style.map("Accent.TButton", background=[("active", ACCENT_ACTIVE)], foreground=[("disabled", "#d5d5d5")])
        style.configure("Plain.TButton", font=("Microsoft YaHei UI", 10), padding=(12, 8))
        style.configure("App.TCombobox", padding=6)
        style.configure("App.TSpinbox", padding=4)

    def _build_layout(self):
        container = tk.Frame(self.root, bg=APP_BG, padx=24, pady=20)
        container.pack(fill="both", expand=True)
        container.grid_columnconfigure(0, weight=3)
        container.grid_columnconfigure(1, weight=2)
        container.grid_rowconfigure(1, weight=1)

        header = tk.Frame(container, bg=APP_BG)
        header.grid(row=0, column=0, columnspan=2, sticky="ew", pady=(0, 18))
        header.grid_columnconfigure(0, weight=1)

        title = tk.Label(
            header,
            text="Mini Keyboard 图形化上位机",
            font=("Microsoft YaHei UI", 24, "bold"),
            fg=TEXT,
            bg=APP_BG,
        )
        title.grid(row=0, column=0, sticky="w")

        subtitle = tk.Label(
            header,
            text="串口扫描、握手测试、时间同步、RGB 主题色配置",
            font=("Microsoft YaHei UI", 11),
            fg=MUTED,
            bg=APP_BG,
        )
        subtitle.grid(row=1, column=0, sticky="w", pady=(6, 0))

        self.status_label = tk.Label(
            header,
            textvariable=self.status_var,
            font=("Microsoft YaHei UI", 10, "bold"),
            fg=ACCENT,
            bg=APP_BG,
        )
        self.status_label.grid(row=0, column=1, rowspan=2, sticky="e")

        left = tk.Frame(container, bg=APP_BG)
        left.grid(row=1, column=0, sticky="nsew", padx=(0, 16))
        left.grid_columnconfigure(0, weight=1)
        left.grid_rowconfigure(0, weight=1)

        right = tk.Frame(container, bg=APP_BG)
        right.grid(row=1, column=1, sticky="nsew")
        right.grid_columnconfigure(0, weight=1)
        right.grid_rowconfigure(0, weight=1)

        left_content = self._build_left_scroll_area(left)
        left_content.grid_columnconfigure(0, weight=1)

        self._build_device_card(left_content).grid(row=0, column=0, sticky="ew", pady=(0, 14))
        self._build_time_card(left_content).grid(row=1, column=0, sticky="ew", pady=(0, 14))
        self._build_color_card(left_content).grid(row=2, column=0, sticky="ew", pady=(0, 14))
        self._build_action_card(left_content).grid(row=3, column=0, sticky="ew", pady=(0, 12))

        self._build_log_card(right).grid(row=0, column=0, sticky="nsew")

    def _build_left_scroll_area(self, parent: tk.Widget) -> tk.Frame:
        wrap = tk.Frame(parent, bg=APP_BG)
        wrap.grid(row=0, column=0, sticky="nsew")
        wrap.grid_columnconfigure(0, weight=1)
        wrap.grid_rowconfigure(0, weight=1)

        self.left_canvas = tk.Canvas(
            wrap,
            bg=APP_BG,
            highlightthickness=0,
            bd=0,
            relief="flat",
        )
        self.left_canvas.grid(row=0, column=0, sticky="nsew")

        left_scrollbar = ttk.Scrollbar(wrap, orient="vertical", command=self.left_canvas.yview)
        left_scrollbar.grid(row=0, column=1, sticky="ns")
        self.left_canvas.configure(yscrollcommand=left_scrollbar.set)

        self.left_content = tk.Frame(self.left_canvas, bg=APP_BG)
        self.left_canvas_window = self.left_canvas.create_window((0, 0), window=self.left_content, anchor="nw")

        self.left_content.bind("<Configure>", self._on_left_content_configure)
        self.left_canvas.bind("<Configure>", self._on_left_canvas_configure)
        self.root.bind_all("<MouseWheel>", self._on_global_mousewheel, add="+")
        return self.left_content

    def _on_left_content_configure(self, _event=None):
        self.left_canvas.configure(scrollregion=self.left_canvas.bbox("all"))

    def _on_left_canvas_configure(self, event):
        self.left_canvas.itemconfigure(self.left_canvas_window, width=event.width)
        self.left_canvas.configure(scrollregion=self.left_canvas.bbox("all"))

    def _on_global_mousewheel(self, event):
        widget = self.root.winfo_containing(event.x_root, event.y_root)
        if widget is None:
            return
        if not self._is_child_of(widget, self.left_content):
            return
        if self.left_canvas.yview() == (0.0, 1.0):
            return
        step = -1 * int(event.delta / 120) if event.delta else 0
        if step:
            self.left_canvas.yview_scroll(step, "units")

    @staticmethod
    def _is_child_of(widget: tk.Widget, ancestor: tk.Widget) -> bool:
        current = widget
        while current is not None:
            if current is ancestor:
                return True
            parent_name = current.winfo_parent()
            if not parent_name:
                return False
            current = current.nametowidget(parent_name)
        return False

    def _make_card(self, parent: tk.Widget) -> tk.Frame:
        card = tk.Frame(parent, bg=CARD_BG, highlightbackground=BORDER, highlightthickness=1, bd=0, padx=18, pady=18)
        return card

    def _build_device_card(self, parent: tk.Widget) -> tk.Frame:
        card = self._make_card(parent)
        card.grid_columnconfigure(0, weight=1)
        card.grid_columnconfigure(1, weight=0)
        card.grid_columnconfigure(2, weight=0)

        ttk.Label(card, text="设备连接", style="CardTitle.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Label(card, text="先列出系统串口，再用握手识别真正的键盘设备。", style="Muted.TLabel").grid(
            row=1, column=0, columnspan=3, sticky="w", pady=(6, 14)
        )

        self.port_combo = ttk.Combobox(card, textvariable=self.port_var, state="readonly", style="App.TCombobox")
        self.port_combo.grid(row=2, column=0, sticky="ew", padx=(0, 10))
        self.busy_widgets.append(self.port_combo)
        self.readonly_widgets.append(self.port_combo)

        refresh_btn = ttk.Button(card, text="刷新串口", style="Plain.TButton", command=self.refresh_system_ports)
        refresh_btn.grid(row=2, column=1, sticky="ew", padx=(0, 10))
        self.busy_widgets.append(refresh_btn)

        scan_btn = ttk.Button(card, text="识别键盘", style="Accent.TButton", command=self.scan_keyboard_ports)
        scan_btn.grid(row=2, column=2, sticky="ew")
        self.busy_widgets.append(scan_btn)

        hint = tk.Label(
            card,
            textvariable=self.port_hint_var,
            justify="left",
            anchor="w",
            wraplength=600,
            font=("Microsoft YaHei UI", 10),
            fg=TEXT,
            bg=CARD_BG,
        )
        hint.grid(row=3, column=0, columnspan=3, sticky="ew", pady=(12, 0))

        actions = tk.Frame(card, bg=CARD_BG)
        actions.grid(row=4, column=0, columnspan=3, sticky="ew", pady=(14, 0))
        actions.grid_columnconfigure(0, weight=1)
        actions.grid_columnconfigure(1, weight=1)
        actions.grid_columnconfigure(2, weight=1)

        test_btn = ttk.Button(actions, text="测试连接", style="Plain.TButton", command=self.test_connection)
        test_btn.grid(row=0, column=0, sticky="ew", padx=(0, 10))
        self.busy_widgets.append(test_btn)

        debug_check = ttk.Checkbutton(actions, text="调试模式", variable=self.debug_var)
        debug_check.grid(row=0, column=1, sticky="w")
        self.busy_widgets.append(debug_check)

        info = tk.Label(
            actions,
            text="每次操作都会自动拉起 DTR 并等待设备就绪。",
            font=("Microsoft YaHei UI", 9),
            fg=MUTED,
            bg=CARD_BG,
        )
        info.grid(row=0, column=2, sticky="e")

        return card

    def _build_time_card(self, parent: tk.Widget) -> tk.Frame:
        card = self._make_card(parent)
        card.grid_columnconfigure(0, weight=1)
        card.grid_columnconfigure(1, weight=0)

        ttk.Label(card, text="时间同步", style="CardTitle.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Label(card, text="把当前电脑时间同步到设备，时区可调整。", style="Muted.TLabel").grid(
            row=1, column=0, columnspan=2, sticky="w", pady=(6, 14)
        )

        clock = tk.Label(
            card,
            textvariable=self.clock_var,
            font=("Consolas", 17, "bold"),
            fg=ACCENT,
            bg=CARD_BG,
        )
        clock.grid(row=2, column=0, sticky="w")

        sync_btn = ttk.Button(card, text="同步当前时间", style="Accent.TButton", command=self.sync_time)
        sync_btn.grid(row=2, column=1, sticky="e")
        self.busy_widgets.append(sync_btn)

        tz_row = tk.Frame(card, bg=CARD_BG)
        tz_row.grid(row=3, column=0, columnspan=2, sticky="ew", pady=(16, 0))
        tz_row.grid_columnconfigure(1, weight=0)

        ttk.Label(tz_row, text="时区偏移（分钟）", style="Body.TLabel").grid(row=0, column=0, sticky="w")
        self.tz_spin = ttk.Spinbox(tz_row, from_=-720, to=840, textvariable=self.tz_var, width=8, style="App.TSpinbox")
        self.tz_spin.grid(row=0, column=1, sticky="w", padx=(10, 10))
        self.busy_widgets.append(self.tz_spin)

        ttk.Label(tz_row, text="中国标准时间通常为 480。", style="Muted.TLabel").grid(row=0, column=2, sticky="w")
        return card

    def _build_color_card(self, parent: tk.Widget) -> tk.Frame:
        card = self._make_card(parent)
        card.grid_columnconfigure(0, weight=1)
        card.grid_columnconfigure(1, weight=0)

        ttk.Label(card, text="RGB 主题色", style="CardTitle.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Label(card, text="拖动滑块或直接改数值，再发送到键盘。", style="Muted.TLabel").grid(
            row=1, column=0, columnspan=2, sticky="w", pady=(6, 16)
        )

        preview_wrap = tk.Frame(card, bg=CARD_BG)
        preview_wrap.grid(row=0, column=1, rowspan=2, sticky="ne")

        self.preview_box = tk.Frame(preview_wrap, width=88, height=88, bg="#4C9EF5", highlightbackground=BORDER, highlightthickness=1)
        self.preview_box.pack()
        self.preview_box.pack_propagate(False)

        self.preview_label = tk.Label(preview_wrap, textvariable=self.hex_var, font=("Consolas", 12, "bold"), fg=TEXT, bg=CARD_BG)
        self.preview_label.pack(pady=(8, 0))

        self._build_rgb_row(card, 2, "R", self.r_var, "#D65A5A")
        self._build_rgb_row(card, 3, "G", self.g_var, "#2F9D66")
        self._build_rgb_row(card, 4, "B", self.b_var, "#4177D6")

        preset_row = tk.Frame(card, bg=CARD_BG)
        preset_row.grid(row=5, column=0, columnspan=2, sticky="ew", pady=(14, 0))
        ttk.Label(preset_row, text="预设", style="Body.TLabel").pack(side="left", padx=(0, 10))
        for name, rgb in PRESET_COLORS:
            btn = ttk.Button(preset_row, text=name, style="Plain.TButton", command=lambda value=rgb: self.apply_preset(value))
            btn.pack(side="left", padx=(0, 8))
            self.busy_widgets.append(btn)

        apply_btn = ttk.Button(card, text="发送主题色", style="Accent.TButton", command=self.send_rgb)
        apply_btn.grid(row=6, column=0, columnspan=2, sticky="ew", pady=(18, 0))
        self.busy_widgets.append(apply_btn)

        return card

    def _build_action_card(self, parent: tk.Widget) -> tk.Frame:
        card = self._make_card(parent)
        card.grid_columnconfigure(1, weight=1)

        ttk.Label(card, text="NumLock 自定义功能", style="CardTitle.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Label(
            card,
            text="当前版本先支持 USB CDC 监听。按下 Num Lock 后，设备发来的 slot1~slot9 会在电脑端执行本地动作。",
            style="Muted.TLabel",
        ).grid(row=1, column=0, columnspan=6, sticky="w", pady=(6, 14))

        status_badge = tk.Label(
            card,
            textvariable=self.monitor_state_var,
            font=("Microsoft YaHei UI", 10, "bold"),
            fg=ACCENT,
            bg=CARD_BG,
        )
        status_badge.grid(row=0, column=1, columnspan=3, sticky="e")

        monitor_btn = ttk.Button(card, textvariable=self.monitor_button_var, style="Accent.TButton", command=self.toggle_action_monitor)
        monitor_btn.grid(row=2, column=0, sticky="w")
        self.busy_widgets.append(monitor_btn)

        save_btn = ttk.Button(card, text="保存动作配置", style="Plain.TButton", command=self.save_action_config)
        save_btn.grid(row=2, column=1, sticky="w", padx=(10, 0))
        self.busy_widgets.append(save_btn)

        ttk.Label(
            card,
            text="监听开启后，请保持此程序运行；关闭程序后，动作不会在电脑端触发。",
            style="Muted.TLabel",
        ).grid(row=2, column=2, columnspan=4, sticky="w", padx=(14, 0))

        header = tk.Frame(card, bg=CARD_BG)
        header.grid(row=3, column=0, columnspan=6, sticky="ew", pady=(16, 8))
        header.grid_columnconfigure(2, weight=1)

        tk.Label(header, text="槽位", font=("Microsoft YaHei UI", 10, "bold"), fg=TEXT, bg=CARD_BG, width=6).grid(row=0, column=0, sticky="w")
        tk.Label(header, text="动作类型", font=("Microsoft YaHei UI", 10, "bold"), fg=TEXT, bg=CARD_BG, width=14).grid(row=0, column=1, sticky="w")
        tk.Label(header, text="目标 / 命令", font=("Microsoft YaHei UI", 10, "bold"), fg=TEXT, bg=CARD_BG).grid(row=0, column=2, sticky="w")

        for slot in range(1, 10):
            self._build_action_row(card, 3 + slot, slot)

        return card

    def _build_action_row(self, parent: tk.Widget, row: int, slot: int):
        vars_for_slot = self.action_slot_vars[slot]

        line = tk.Frame(parent, bg=CARD_BG)
        line.grid(row=row, column=0, columnspan=6, sticky="ew", pady=5)
        line.grid_columnconfigure(2, weight=1)

        tk.Label(
            line,
            text=f"slot {slot}",
            width=6,
            font=("Consolas", 11, "bold"),
            fg=TEXT,
            bg=CARD_BG,
        ).grid(row=0, column=0, sticky="w")

        type_combo = ttk.Combobox(
            line,
            textvariable=vars_for_slot["type"],
            values=ACTION_TYPE_LABELS,
            state="readonly",
            width=14,
            style="App.TCombobox",
        )
        type_combo.grid(row=0, column=1, sticky="w", padx=(0, 10))
        self.busy_widgets.append(type_combo)
        self.readonly_widgets.append(type_combo)

        target_entry = ttk.Entry(line, textvariable=vars_for_slot["target"])
        target_entry.grid(row=0, column=2, sticky="ew", padx=(0, 10))
        self.busy_widgets.append(target_entry)

        browse_btn = ttk.Button(line, text="浏览", style="Plain.TButton", command=lambda s=slot: self.browse_action_target(s))
        browse_btn.grid(row=0, column=3, sticky="w", padx=(0, 8))
        self.busy_widgets.append(browse_btn)

        test_btn = ttk.Button(line, text="测试", style="Plain.TButton", command=lambda s=slot: self.test_slot_action(s))
        test_btn.grid(row=0, column=4, sticky="w")
        self.busy_widgets.append(test_btn)

    def _build_rgb_row(self, parent: tk.Widget, row: int, label: str, variable: tk.IntVar, trough: str):
        line = tk.Frame(parent, bg=CARD_BG)
        line.grid(row=row, column=0, columnspan=2, sticky="ew", pady=6)
        line.grid_columnconfigure(1, weight=1)

        tk.Label(line, text=label, width=3, font=("Consolas", 11, "bold"), fg=TEXT, bg=CARD_BG).grid(row=0, column=0, sticky="w")

        scale = tk.Scale(
            line,
            from_=0,
            to=255,
            orient="horizontal",
            resolution=1,
            variable=variable,
            command=lambda _value: self._refresh_color_preview(),
            bg=CARD_BG,
            activebackground=ACCENT,
            highlightthickness=0,
            troughcolor=trough,
            fg=TEXT,
            length=360,
        )
        scale.grid(row=0, column=1, sticky="ew", padx=(8, 12))
        self.busy_widgets.append(scale)

        spin = ttk.Spinbox(line, from_=0, to=255, textvariable=variable, width=5, style="App.TSpinbox")
        spin.grid(row=0, column=2, sticky="e")
        spin.bind("<KeyRelease>", lambda _event: self._refresh_color_preview())
        spin.bind("<<Increment>>", lambda _event: self._refresh_color_preview())
        spin.bind("<<Decrement>>", lambda _event: self._refresh_color_preview())
        self.busy_widgets.append(spin)

    def _build_log_card(self, parent: tk.Widget) -> tk.Frame:
        card = self._make_card(parent)
        card.grid_columnconfigure(0, weight=1)
        card.grid_rowconfigure(1, weight=1)

        ttk.Label(card, text="运行日志", style="CardTitle.TLabel").grid(row=0, column=0, sticky="w")

        log_wrap = tk.Frame(card, bg=LOG_BG, highlightbackground=BORDER, highlightthickness=1)
        log_wrap.grid(row=1, column=0, sticky="nsew", pady=(14, 0))
        log_wrap.grid_columnconfigure(0, weight=1)
        log_wrap.grid_rowconfigure(0, weight=1)

        self.log_text = tk.Text(
            log_wrap,
            bg=LOG_BG,
            fg=TEXT,
            insertbackground=TEXT,
            relief="flat",
            wrap="word",
            font=("Consolas", 10),
            padx=12,
            pady=12,
        )
        self.log_text.grid(row=0, column=0, sticky="nsew")
        self.log_text.configure(state="disabled")

        scroll = ttk.Scrollbar(log_wrap, orient="vertical", command=self.log_text.yview)
        scroll.grid(row=0, column=1, sticky="ns")
        self.log_text.configure(yscrollcommand=scroll.set)
        return card

    def _wire_events(self):
        self.port_combo.bind("<<ComboboxSelected>>", lambda _event: self._update_port_hint())
        for variable in (self.r_var, self.g_var, self.b_var):
            variable.trace_add("write", lambda *_args: self._refresh_color_preview())

    def _refresh_clock(self):
        self.clock_var.set(datetime.now().strftime("%Y-%m-%d %H:%M:%S"))
        self.root.after(1000, self._refresh_clock)

    def _set_status(self, text: str, tone: str = "normal"):
        colors = {
            "normal": ACCENT,
            "busy": TEXT,
            "error": WARN,
        }
        self.status_var.set(text)
        self.status_label.configure(fg=colors.get(tone, ACCENT))

    def _append_log(self, message: str):
        if not message:
            return
        timestamp = time.strftime("%H:%M:%S")
        self.log_text.configure(state="normal")
        for line in message.splitlines():
            self.log_text.insert("end", f"[{timestamp}] {line}\n")
        self.log_text.see("end")
        self.log_text.configure(state="disabled")

    def _enqueue_log(self, message: str):
        self.ui_queue.put(("log", message))

    def _set_busy(self, busy: bool, text: str | None = None):
        self.is_busy = busy
        for widget in self.busy_widgets:
            try:
                if widget in self.readonly_widgets:
                    widget.configure(state="disabled" if busy else "readonly")
                else:
                    widget.configure(state="disabled" if busy else "normal")
            except tk.TclError:
                pass

        if busy and text:
            self._set_status(text, "busy")
        elif not busy:
            self._set_status("就绪", "normal")

    def _apply_monitor_state(self, running: bool, port: str | None, message: str):
        self.monitor_running = running
        self.monitor_port = port if running else None
        self.monitor_state_var.set(message)
        self.monitor_button_var.set("停止 USB 监听" if running else "启动 USB 监听")
        if running:
            self._set_status("USB 自定义监听已启动", "normal")

    def _process_ui_queue(self):
        try:
            while True:
                event = self.ui_queue.get_nowait()
                kind = event[0]
                if kind == "log":
                    self._append_log(event[1])
                elif kind == "status":
                    self._set_status(event[1], event[2])
                elif kind == "busy":
                    self._set_busy(event[1], event[2])
                elif kind == "ports":
                    self._apply_port_values(event[1], event[2])
                elif kind == "hint":
                    self.port_hint_var.set(event[1])
                elif kind == "monitor_state":
                    self._apply_monitor_state(event[1], event[2], event[3])
                elif kind == "slot_trigger":
                    self._handle_slot_trigger(event[1])
                elif kind == "done":
                    success, message = event[1], event[2]
                    self._append_log(message)
                    self._set_status("操作成功" if success else "操作失败", "normal" if success else "error")
        except queue.Empty:
            pass
        self.root.after(100, self._process_ui_queue)

    def _run_async(self, action_name: str, job):
        if self.is_busy:
            self._append_log(f"[!] 当前仍有任务在执行，请稍后再试。")
            return

        self._set_busy(True, f"{action_name}中...")

        def worker():
            try:
                success, message = job()
            except Exception as exc:
                success, message = False, f"[✗] {action_name}异常: {exc}"
            self.ui_queue.put(("busy", False, None))
            self.ui_queue.put(("done", success, message))

        threading.Thread(target=worker, daemon=True).start()

    def _refresh_color_preview(self):
        r, g, b = self._get_rgb_values()
        color = f"#{r:02X}{g:02X}{b:02X}"
        self.hex_var.set(color)
        self.preview_box.configure(bg=color)

    def _get_rgb_values(self) -> tuple[int, int, int]:
        return self._clamp(self.r_var.get()), self._clamp(self.g_var.get()), self._clamp(self.b_var.get())

    @staticmethod
    def _clamp(value: int) -> int:
        return max(0, min(255, int(value)))

    def _update_port_hint(self):
        port = self.port_var.get().strip()
        if not port:
            self.port_hint_var.set("当前还没有选中串口。")
            return
        self.port_hint_var.set(self.port_meta.get(port, f"已选择 {port}"))

    def _apply_port_values(self, ports: list[str], details: dict[str, str]):
        self.port_meta = details
        self.port_combo["values"] = ports
        if ports:
            current = self.port_var.get().strip()
            if current not in ports:
                self.port_var.set(ports[0])
            self._update_port_hint()
        else:
            self.port_var.set("")
            self.port_hint_var.set("没有发现可用串口。")

    def _collect_action_config(self) -> dict:
        slots = {}
        for slot, vars_for_slot in self.action_slot_vars.items():
            action_label = vars_for_slot["type"].get()
            slots[str(slot)] = {
                "type": ACTION_LABEL_TO_TYPE.get(action_label, "none"),
                "target": vars_for_slot["target"].get().strip(),
            }
        return {"version": 1, "slots": slots}

    def load_action_config(self):
        if not self.config_path.exists():
            return

        try:
            data = json.loads(self.config_path.read_text(encoding="utf-8"))
        except Exception as exc:
            self._append_log(f"[✗] 读取动作配置失败: {exc}")
            return

        slots = data.get("slots", {})
        for slot in range(1, 10):
            config = slots.get(str(slot), {})
            action_type = str(config.get("type", "none"))
            target = str(config.get("target", ""))
            self.action_slot_vars[slot]["type"].set(ACTION_TYPE_TO_LABEL.get(action_type, ACTION_TYPE_TO_LABEL["none"]))
            self.action_slot_vars[slot]["target"].set(target)

        self._append_log(f"[*] 已加载动作配置: {self.config_path.name}")

    def save_action_config(self, silent: bool = False) -> bool:
        try:
            self.config_path.write_text(
                json.dumps(self._collect_action_config(), ensure_ascii=False, indent=2),
                encoding="utf-8",
            )
        except Exception as exc:
            if not silent:
                messagebox.showerror("保存失败", f"动作配置保存失败：\n{exc}")
            self._append_log(f"[✗] 动作配置保存失败: {exc}")
            return False

        self._append_log(f"[✓] 动作配置已保存到 {self.config_path.name}")
        if not silent:
            self._set_status("动作配置已保存", "normal")
        return True

    def _get_slot_config(self, slot: int) -> tuple[str, str]:
        vars_for_slot = self.action_slot_vars[slot]
        return ACTION_LABEL_TO_TYPE.get(vars_for_slot["type"].get(), "none"), vars_for_slot["target"].get().strip()

    def browse_action_target(self, slot: int):
        action_type, _target = self._get_slot_config(slot)
        if action_type == "none":
            messagebox.showinfo("选择动作类型", f"请先给 slot {slot} 选择动作类型。")
            return

        if action_type == "url":
            messagebox.showinfo("填写方式", "网页地址请直接手动输入，例如：https://www.example.com")
            return

        path = filedialog.askopenfilename(
            title=f"为 slot {slot} 选择目标文件",
            filetypes=[("所有文件", "*.*")],
        )
        if path:
            self.action_slot_vars[slot]["target"].set(path)

    def test_slot_action(self, slot: int):
        action_type, target = self._get_slot_config(slot)
        self._append_log(f"[*] 手动测试 slot{slot} 动作")
        threading.Thread(
            target=self._execute_config_action,
            args=(slot, action_type, target, "手动测试"),
            daemon=True,
        ).start()

    def _normalize_target_path(self, target: str) -> Path | None:
        cleaned = target.strip().strip('"')
        if not cleaned:
            return None
        candidate = Path(os.path.expandvars(os.path.expanduser(cleaned)))
        if candidate.exists():
            return candidate
        return None

    def _execute_config_action(self, slot: int, action_type: str, target: str, source: str):
        try:
            if action_type == "none":
                self._enqueue_log(f"[*] slot{slot} 未配置动作，来源：{source}")
                return

            if not target:
                self._enqueue_log(f"[!] slot{slot} 已配置为 {ACTION_TYPE_TO_LABEL.get(action_type, action_type)}，但目标为空。")
                return

            if action_type == "url":
                url = target if "://" in target else f"https://{target}"
                webbrowser.open(url)
                self._enqueue_log(f"[✓] slot{slot} 已打开网页：{url}")
                return

            if action_type == "open":
                target_path = self._normalize_target_path(target)
                if target_path is not None:
                    os.startfile(str(target_path))
                    self._enqueue_log(f"[✓] slot{slot} 已打开：{target_path}")
                else:
                    subprocess.Popen(target, shell=True)
                    self._enqueue_log(f"[✓] slot{slot} 已启动：{target}")
                return

            if action_type == "command":
                target_path = self._normalize_target_path(target)
                if target_path is not None:
                    suffix = target_path.suffix.lower()
                    if suffix == ".py":
                        launcher = "py" if shutil.which("py") else "python"
                        subprocess.Popen([launcher, str(target_path)], cwd=str(target_path.parent))
                    elif suffix == ".ps1":
                        subprocess.Popen(
                            ["powershell", "-ExecutionPolicy", "Bypass", "-File", str(target_path)],
                            cwd=str(target_path.parent),
                        )
                    elif suffix in (".bat", ".cmd"):
                        subprocess.Popen(["cmd", "/c", str(target_path)], cwd=str(target_path.parent))
                    else:
                        subprocess.Popen(["cmd", "/c", str(target_path)], cwd=str(target_path.parent))
                    self._enqueue_log(f"[✓] slot{slot} 已执行脚本：{target_path}")
                else:
                    subprocess.Popen(["cmd", "/c", target], cwd=str(self.config_path.parent))
                    self._enqueue_log(f"[✓] slot{slot} 已执行命令：{target}")
                return

            self._enqueue_log(f"[!] slot{slot} 遇到未知动作类型：{action_type}")
        except Exception as exc:
            self._enqueue_log(f"[✗] slot{slot} 执行失败（{source}）: {exc}")

    def _handle_slot_trigger(self, slot: int):
        action_type, target = self._get_slot_config(slot)
        self._append_log(f"[→] 收到自定义触发：slot{slot}")
        threading.Thread(
            target=self._execute_config_action,
            args=(slot, action_type, target, "设备触发"),
            daemon=True,
        ).start()

    def toggle_action_monitor(self):
        if self.monitor_running:
            self.stop_action_monitor()
        else:
            self.start_action_monitor()

    def start_action_monitor(self):
        port = self._require_port()
        if port is None:
            return

        if self.is_busy:
            self._append_log("[!] 当前仍有任务在执行，请稍后再启动监听。")
            return

        if self.monitor_thread is not None and self.monitor_thread.is_alive():
            self._append_log("[!] 监听线程正在运行或尚未退出，请稍后再试。")
            return

        self.save_action_config(silent=True)
        debug = self.debug_var.get()
        normalized_port = host_cli.normalize_port(port)
        self.monitor_stop = threading.Event()
        self.monitor_state_var.set(f"正在启动监听：{normalized_port}")
        self._append_log(f"[*] 正在启动 USB 自定义监听，端口 {normalized_port}")

        self.monitor_thread = threading.Thread(
            target=self._monitor_worker,
            args=(normalized_port, debug, self.monitor_stop),
            daemon=True,
        )
        self.monitor_thread.start()

    def stop_action_monitor(self):
        if self.monitor_thread is None or not self.monitor_thread.is_alive():
            self._apply_monitor_state(False, None, "监听未启动")
            self._append_log("[*] USB 自定义监听当前未启动。")
            return

        self.monitor_state_var.set("正在停止监听...")
        self.monitor_stop.set()
        self._append_log("[*] 正在停止 USB 自定义监听...")

    def _monitor_worker(self, port: str, debug: bool, stop_event: threading.Event):
        ser = None
        try:
            ser = host_cli.open_serial_port(port, log=self._enqueue_log)
            ser.timeout = 0.5

            with self.monitor_lock:
                if not host_cli.handshake(ser, debug=debug, log=self._enqueue_log):
                    self.ui_queue.put(("monitor_state", False, None, "监听启动失败"))
                    return
                self.monitor_serial = ser

            self.ui_queue.put(("monitor_state", True, port, f"监听中：{port}"))
            self._enqueue_log("[*] USB 自定义监听已启动，等待 slot1~slot9 触发。")

            while not stop_event.is_set():
                with self.monitor_lock:
                    result = host_cli.read_frame(ser, debug=False, log=None)

                if result is None:
                    continue

                _, payload = result
                message = host_cli.decode_response(payload)
                slot = host_cli.extract_action_trigger_slot(message)
                if slot is not None:
                    self.ui_queue.put(("slot_trigger", slot))
                else:
                    self._enqueue_log(f"[*] 监听收到未识别帧: {message}")
        except Exception as exc:
            self._enqueue_log(f"[✗] USB 自定义监听异常: {exc}")
        finally:
            with self.monitor_lock:
                if self.monitor_serial is ser:
                    self.monitor_serial = None
                if ser is not None:
                    try:
                        ser.close()
                    except Exception:
                        pass
            self.ui_queue.put(("monitor_state", False, None, "监听未启动"))
            self._enqueue_log("[*] USB 自定义监听已停止。")

    def _run_serial_action(self, port: str, action):
        normalized_port = host_cli.normalize_port(port)
        if self.monitor_running and self.monitor_port == normalized_port and self.monitor_serial is not None:
            with self.monitor_lock:
                previous_timeout = getattr(self.monitor_serial, "timeout", host_cli.SERIAL_TIMEOUT)
                self.monitor_serial.timeout = host_cli.SERIAL_TIMEOUT
                try:
                    return action(self.monitor_serial)
                finally:
                    self.monitor_serial.timeout = previous_timeout
        return self._open_and_run(normalized_port, action)

    def refresh_system_ports(self):
        try:
            host_cli.ensure_pyserial()
            ports = list(host_cli.serial.tools.list_ports.comports())
        except RuntimeError as exc:
            messagebox.showerror("缺少依赖", str(exc))
            self._append_log(f"[✗] {exc}")
            self._set_status("缺少 pyserial", "error")
            return

        devices = []
        details: dict[str, str] = {}
        for item in ports:
            label = item.description or "未知串口设备"
            suffix = []
            if item.vid is not None:
                suffix.append(f"VID=0x{item.vid:04X}")
            if item.pid is not None:
                suffix.append(f"PID=0x{item.pid:04X}")
            detail = f"{label} | {' '.join(suffix)}".strip()
            devices.append(item.device)
            details[item.device] = detail

        self._apply_port_values(devices, details)
        self._append_log(f"[*] 系统串口刷新完成，共发现 {len(devices)} 个端口。")
        self._set_status("串口列表已更新", "normal")

    def scan_keyboard_ports(self):
        if self.monitor_running:
            self._append_log("[!] USB 自定义监听运行中，请先停止监听再执行识别键盘。")
            return

        def job():
            self._enqueue_log("[*] 开始握手识别设备...")
            found = host_cli.cmd_scan(log=self._enqueue_log)

            details = dict(self.port_meta)
            if not details:
                try:
                    ports = list(host_cli.serial.tools.list_ports.comports())
                except Exception:
                    ports = []
                for item in ports:
                    details[item.device] = item.description or "未知串口设备"

            for port in found:
                base = details.get(port, "已识别设备")
                details[port] = f"{base} | 已通过握手识别"

            self.ui_queue.put(("ports", found or list(details.keys()), details))
            if found:
                self.ui_queue.put(("hint", f"识别到 {len(found)} 个可响应设备：{', '.join(found)}"))
                return True, f"[✓] 识别完成，找到设备：{', '.join(found)}"
            self.ui_queue.put(("hint", "未识别到可响应的键盘设备，请检查 USB 连接和模式开关。"))
            return False, "[✗] 未识别到设备。"

        self._run_async("识别键盘", job)

    def _require_port(self) -> str | None:
        port = self.port_var.get().strip()
        if not port:
            messagebox.showwarning("未选择串口", "请先选择一个串口。")
            return None
        return port

    def _open_and_run(self, port: str, action):
        ser = None
        try:
            ser = host_cli.open_serial_port(host_cli.normalize_port(port), log=self._enqueue_log)
            return action(ser)
        finally:
            if ser is not None:
                try:
                    ser.close()
                    self._enqueue_log("[*] 串口已关闭")
                except Exception:
                    pass

    def test_connection(self):
        port = self._require_port()
        if port is None:
            return

        debug = self.debug_var.get()

        def job():
            success = self._run_serial_action(
                port,
                lambda ser: host_cli.handshake(ser, debug=debug, log=self._enqueue_log),
            )
            return success, f"[{'✓' if success else '✗'}] {port} 握手{'成功' if success else '失败'}。"

        self._run_async("测试连接", job)

    def sync_time(self):
        port = self._require_port()
        if port is None:
            return

        debug = self.debug_var.get()
        tz_min = int(self.tz_var.get())

        def job():
            success = self._run_serial_action(
                port,
                lambda ser: host_cli.cmd_time(ser, tz_min, debug=debug, log=self._enqueue_log),
            )
            return success, f"[{'✓' if success else '✗'}] 时间同步{'完成' if success else '失败'}，端口 {port}。"

        self._run_async("时间同步", job)

    def apply_preset(self, rgb: tuple[int, int, int]):
        self.r_var.set(rgb[0])
        self.g_var.set(rgb[1])
        self.b_var.set(rgb[2])
        self._refresh_color_preview()

    def send_rgb(self):
        port = self._require_port()
        if port is None:
            return

        debug = self.debug_var.get()
        r, g, b = self._get_rgb_values()
        self.r_var.set(r)
        self.g_var.set(g)
        self.b_var.set(b)
        hex_color = f"#{r:02X}{g:02X}{b:02X}"

        def job():
            success = self._run_serial_action(
                port,
                lambda ser: host_cli.cmd_rgb(ser, r, g, b, debug=debug, log=self._enqueue_log),
            )
            return success, f"[{'✓' if success else '✗'}] 主题色 {hex_color} {'已发送' if success else '发送失败'}，端口 {port}。"

        self._run_async("发送主题色", job)

    def on_close(self):
        self.monitor_stop.set()
        with self.monitor_lock:
            if self.monitor_serial is not None:
                try:
                    self.monitor_serial.close()
                except Exception:
                    pass
                self.monitor_serial = None
        self.root.destroy()


def main():
    root = tk.Tk()
    KeyboardGuiApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
