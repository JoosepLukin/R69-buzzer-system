import json
import os
from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Tuple

import tkinter as tk
from tkinter import ttk, filedialog, messagebox

import matplotlib
matplotlib.use("TkAgg")
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg, NavigationToolbar2Tk
from matplotlib.figure import Figure
import matplotlib.image as mpimg


# -----------------------------
# Data structures
# -----------------------------

@dataclass
class Device:
    key: str                  # device key shown/used in GUI (e.g. "1", "2", "M")
    role: str = "slave"        # slave/master/other
    desc: str = ""
    x: Optional[float] = None  # image coords
    y: Optional[float] = None


@dataclass
class LogRecord:
    line_no: int
    raw: str
    obj: Optional[Dict[str, Any]]


# -----------------------------
# Helpers
# -----------------------------

def safe_int(v: Any) -> Optional[int]:
    try:
        if v is None:
            return None
        return int(v)
    except Exception:
        return None


def safe_float(v: Any) -> Optional[float]:
    try:
        if v is None:
            return None
        return float(v)
    except Exception:
        return None


def load_log_file(path: str) -> List[LogRecord]:
    records: List[LogRecord] = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for i, line in enumerate(f, start=1):
            s = line.rstrip("\r\n")
            obj = None
            if s.startswith("{") and s.endswith("}"):
                try:
                    obj = json.loads(s)
                except Exception:
                    obj = None
            records.append(LogRecord(line_no=i, raw=s, obj=obj))
    return records


def ms_str_from_us(us: Optional[int]) -> str:
    if us is None:
        return "-"
    return f"{us/1000.0:.3f}"


# -----------------------------
# Main App
# -----------------------------

class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("ESP-NOW Log Mapper + Timeline")
        self.geometry("1320x820")

        # state
        self.log_path: Optional[str] = None
        self.log_records: List[LogRecord] = []
        self.image_path: Optional[str] = None
        self.bg_image = None

        self.devices: Dict[str, Device] = {}
        self.last_stats: Dict[str, Dict[str, Any]] = {}

        # placement state
        self.place_mode = tk.BooleanVar(value=False)
        self.placing_device_key: Optional[str] = None

        # settings
        self.range_mode = tk.StringVar(value="line")  # "line" or "time"
        self.line_start_var = tk.IntVar(value=1)
        self.line_end_var = tk.IntVar(value=1)
        self.t_start_var = tk.StringVar(value="")
        self.t_end_var = tk.StringVar(value="")

        self.offline_after_s_var = tk.DoubleVar(value=2.0)
        self.rtt_outlier_max_ms_var = tk.StringVar(value="")  # optional outlier drop

        # Which field defines device ID in log lines
        self.id_field_var = tk.StringVar(value="auto")  # auto/responder_id/slave_index/target_id/id

        # overlay settings
        self.show_ids_var = tk.BooleanVar(value=True)
        self.show_desc_var = tk.BooleanVar(value=True)
        self.metric_var = tk.StringVar(value="avg_ms")  # none/avg_ms/last_ms/ok_count/fail_count/last_seen
        self.point_size_var = tk.IntVar(value=55)
        self.font_size_var = tk.IntVar(value=10)

        # build UI
        self._build_menu()
        self._build_layout()

        self._redraw_all()

    # -------------------------
    # UI
    # -------------------------

    def _build_menu(self):
        menubar = tk.Menu(self)
        filem = tk.Menu(menubar, tearoff=0)
        filem.add_command(label="Load Log...", command=self.load_log)
        filem.add_command(label="Load Floorplan Image...", command=self.load_image)
        filem.add_separator()
        filem.add_command(label="Save Layout...", command=self.save_layout)
        filem.add_command(label="Load Layout...", command=self.load_layout)
        filem.add_separator()
        filem.add_command(label="Export Annotated Image...", command=self.export_annotated_image)
        filem.add_separator()
        filem.add_command(label="Exit", command=self.destroy)
        menubar.add_cascade(label="File", menu=filem)
        self.config(menu=menubar)

    def _build_layout(self):
        paned = ttk.PanedWindow(self, orient=tk.HORIZONTAL)
        paned.pack(fill=tk.BOTH, expand=True)

        left = ttk.Frame(paned, padding=8)
        paned.add(left, weight=0)

        right = ttk.Frame(paned, padding=8)
        paned.add(right, weight=1)

        # ---------- LEFT: LOG ----------
        lf_log = ttk.LabelFrame(left, text="Log", padding=8)
        lf_log.pack(fill=tk.X, padx=4, pady=4)

        self.log_label = ttk.Label(lf_log, text="No log loaded")
        self.log_label.pack(anchor="w")

        btns = ttk.Frame(lf_log)
        btns.pack(fill=tk.X, pady=6)
        ttk.Button(btns, text="Load Log…", command=self.load_log).pack(side=tk.LEFT)
        ttk.Button(btns, text="Reload", command=self.reload_log).pack(side=tk.LEFT, padx=6)

        # ---------- LEFT: RANGE ----------
        lf_range = ttk.LabelFrame(left, text="Range selection", padding=8)
        lf_range.pack(fill=tk.X, padx=4, pady=4)

        row0 = ttk.Frame(lf_range); row0.pack(fill=tk.X)
        ttk.Radiobutton(row0, text="By lines", variable=self.range_mode, value="line",
                        command=self.update_stats_and_ui).pack(side=tk.LEFT)
        ttk.Radiobutton(row0, text="By t_ms", variable=self.range_mode, value="time",
                        command=self.update_stats_and_ui).pack(side=tk.LEFT, padx=10)

        row1 = ttk.Frame(lf_range); row1.pack(fill=tk.X, pady=4)
        ttk.Label(row1, text="Start line").pack(side=tk.LEFT)
        ttk.Entry(row1, width=8, textvariable=self.line_start_var).pack(side=tk.LEFT, padx=6)
        ttk.Label(row1, text="End line").pack(side=tk.LEFT, padx=(10, 0))
        ttk.Entry(row1, width=8, textvariable=self.line_end_var).pack(side=tk.LEFT, padx=6)

        row2 = ttk.Frame(lf_range); row2.pack(fill=tk.X, pady=4)
        ttk.Label(row2, text="t_ms start").pack(side=tk.LEFT)
        ttk.Entry(row2, width=10, textvariable=self.t_start_var).pack(side=tk.LEFT, padx=6)
        ttk.Label(row2, text="t_ms end").pack(side=tk.LEFT, padx=(10, 0))
        ttk.Entry(row2, width=10, textvariable=self.t_end_var).pack(side=tk.LEFT, padx=6)

        row3 = ttk.Frame(lf_range); row3.pack(fill=tk.X, pady=4)
        ttk.Label(row3, text="Offline after (s)").pack(side=tk.LEFT)
        ttk.Entry(row3, width=6, textvariable=self.offline_after_s_var).pack(side=tk.LEFT, padx=6)
        ttk.Label(row3, text="Drop RTT > (ms)").pack(side=tk.LEFT, padx=(10, 0))
        ttk.Entry(row3, width=8, textvariable=self.rtt_outlier_max_ms_var).pack(side=tk.LEFT, padx=6)

        row4 = ttk.Frame(lf_range); row4.pack(fill=tk.X, pady=4)
        ttk.Label(row4, text="Device ID field").pack(side=tk.LEFT)
        ttk.Combobox(
            row4, width=14, state="readonly", textvariable=self.id_field_var,
            values=["auto", "responder_id", "slave_index", "target_id", "id"]
        ).pack(side=tk.LEFT, padx=6)
        ttk.Button(row4, text="Apply", command=self.update_stats_and_ui).pack(side=tk.LEFT, padx=6)

        # ---------- LEFT: STATS ----------
        lf_stats = ttk.LabelFrame(left, text="Per-device stats (selected range)", padding=8)
        lf_stats.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

        cols = ("id", "ok", "fail", "avg_ms", "min_ms", "max_ms", "last_ms", "last_t_ms")
        self.stats_tree = ttk.Treeview(lf_stats, columns=cols, show="headings", height=10)
        for c in cols:
            self.stats_tree.heading(c, text=c)
            self.stats_tree.column(c, width=90, anchor="center")
        self.stats_tree.column("id", width=70, anchor="center")
        self.stats_tree.pack(fill=tk.BOTH, expand=True)

        # ---------- LEFT: DEVICES ----------
        lf_dev = ttk.LabelFrame(left, text="Devices on image", padding=8)
        lf_dev.pack(fill=tk.BOTH, expand=False, padx=4, pady=4)

        topdev = ttk.Frame(lf_dev); topdev.pack(fill=tk.X)
        ttk.Button(topdev, text="Load Image…", command=self.load_image).pack(side=tk.LEFT)
        ttk.Button(topdev, text="Export Image…", command=self.export_annotated_image).pack(side=tk.LEFT, padx=6)

        devcols = ("key", "role", "desc", "x", "y")
        self.dev_tree = ttk.Treeview(lf_dev, columns=devcols, show="headings", height=7, selectmode="browse")
        for c in devcols:
            self.dev_tree.heading(c, text=c)
            self.dev_tree.column(c, width=95, anchor="center")
        self.dev_tree.column("desc", width=220, anchor="w")
        self.dev_tree.pack(fill=tk.X, pady=6)
        self.dev_tree.bind("<<TreeviewSelect>>", lambda e: self._on_device_select())

        self.dev_key_var = tk.StringVar(value="")
        self.dev_role_var = tk.StringVar(value="slave")
        self.dev_desc_var = tk.StringVar(value="")

        form = ttk.Frame(lf_dev); form.pack(fill=tk.X, pady=2)

        r1 = ttk.Frame(form); r1.pack(fill=tk.X, pady=2)
        ttk.Label(r1, text="ID/key").pack(side=tk.LEFT)
        ttk.Entry(r1, width=10, textvariable=self.dev_key_var).pack(side=tk.LEFT, padx=6)
        ttk.Label(r1, text="Role").pack(side=tk.LEFT, padx=(10, 0))
        ttk.Combobox(r1, width=10, textvariable=self.dev_role_var,
                     values=["slave", "master", "other"], state="readonly").pack(side=tk.LEFT, padx=6)

        r2 = ttk.Frame(form); r2.pack(fill=tk.X, pady=2)
        ttk.Label(r2, text="Description").pack(side=tk.LEFT)
        ttk.Entry(r2, textvariable=self.dev_desc_var).pack(side=tk.LEFT, padx=6, fill=tk.X, expand=True)

        actions = ttk.Frame(lf_dev); actions.pack(fill=tk.X, pady=6)

        ttk.Button(actions, text="Add/Update", command=self.add_or_update_device).pack(side=tk.LEFT)
        ttk.Button(actions, text="Remove", command=self.remove_device).pack(side=tk.LEFT, padx=6)

        ttk.Separator(lf_dev, orient=tk.HORIZONTAL).pack(fill=tk.X, pady=4)

        place_row = ttk.Frame(lf_dev); place_row.pack(fill=tk.X)
        ttk.Checkbutton(place_row, text="Place mode (click image)", variable=self.place_mode,
                        command=self._on_place_mode_toggle).pack(side=tk.LEFT)
        ttk.Button(place_row, text="Place selected", command=self.enable_place_selected).pack(side=tk.LEFT, padx=6)
        ttk.Button(place_row, text="Stop placing", command=self.disable_place_mode).pack(side=tk.LEFT, padx=6)

        # ---------- LEFT: OVERLAY ----------
        lf_overlay = ttk.LabelFrame(left, text="Overlay settings", padding=8)
        lf_overlay.pack(fill=tk.X, padx=4, pady=4)

        rowo1 = ttk.Frame(lf_overlay); rowo1.pack(fill=tk.X, pady=2)
        ttk.Checkbutton(rowo1, text="Show IDs", variable=self.show_ids_var,
                        command=self._redraw_all).pack(side=tk.LEFT)
        ttk.Checkbutton(rowo1, text="Show descriptions", variable=self.show_desc_var,
                        command=self._redraw_all).pack(side=tk.LEFT, padx=10)

        rowo2 = ttk.Frame(lf_overlay); rowo2.pack(fill=tk.X, pady=2)
        ttk.Label(rowo2, text="Metric label").pack(side=tk.LEFT)
        ttk.Combobox(
            rowo2, width=12, textvariable=self.metric_var,
            values=["none", "avg_ms", "last_ms", "ok_count", "fail_count", "last_seen"],
            state="readonly"
        ).pack(side=tk.LEFT, padx=6)
        ttk.Button(rowo2, text="Refresh", command=self._redraw_all).pack(side=tk.LEFT, padx=6)

        rowo3 = ttk.Frame(lf_overlay); rowo3.pack(fill=tk.X, pady=2)
        ttk.Label(rowo3, text="Point size").pack(side=tk.LEFT)
        ttk.Spinbox(rowo3, from_=10, to=300, textvariable=self.point_size_var, width=6,
                    command=self._redraw_all).pack(side=tk.LEFT, padx=6)
        ttk.Label(rowo3, text="Font size").pack(side=tk.LEFT, padx=(10, 0))
        ttk.Spinbox(rowo3, from_=6, to=24, textvariable=self.font_size_var, width=6,
                    command=self._redraw_all).pack(side=tk.LEFT, padx=6)

        # ---------- RIGHT: MAP + TIMELINE ----------
        # Map figure
        self.map_fig = Figure(figsize=(7, 5), dpi=100)
        self.map_ax = self.map_fig.add_subplot(111)
        self.map_ax.set_axis_off()

        self.map_canvas = FigureCanvasTkAgg(self.map_fig, master=right)
        self.map_canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)

        self.toolbar = NavigationToolbar2Tk(self.map_canvas, right)
        self.toolbar.update()

        self.map_canvas.mpl_connect("button_press_event", self._on_map_click)

        # Timeline figure
        self.time_fig = Figure(figsize=(7, 2.2), dpi=100)
        self.time_ax = self.time_fig.add_subplot(111)

        self.time_canvas = FigureCanvasTkAgg(self.time_fig, master=right)
        self.time_canvas.get_tk_widget().pack(fill=tk.BOTH, expand=False)

        # status bar
        self.status_var = tk.StringVar(value="Load a log and image. Select a device, edit description, Add/Update. Enable Place mode and click the map.")
        ttk.Label(right, textvariable=self.status_var).pack(fill=tk.X, pady=(6, 0))

    # -------------------------
    # ID picking (configurable)
    # -------------------------

    def pick_device_key(self, obj: Dict[str, Any]) -> Optional[str]:
        mode = self.id_field_var.get()
        if mode != "auto":
            v = obj.get(mode)
            if v is None:
                return None
            return str(v)

        # auto priority
        for k in ("responder_id", "slave_index", "target_id", "id"):
            v = obj.get(k)
            if v is None:
                continue
            return str(v)
        return None

    # -------------------------
    # Log + Image actions
    # -------------------------

    def load_log(self):
        path = filedialog.askopenfilename(
            title="Select log file",
            filetypes=[("Text/Log files", "*.txt *.log *.ndjson *.*"), ("All files", "*.*")]
        )
        if not path:
            return
        self.log_path = path
        self._load_log_internal(path)

    def reload_log(self):
        if not self.log_path:
            messagebox.showinfo("Reload", "No log loaded yet.")
            return
        self._load_log_internal(self.log_path)

    def _load_log_internal(self, path: str):
        try:
            self.log_records = load_log_file(path)
        except Exception as e:
            messagebox.showerror("Error", f"Failed to load log:\n{e}")
            return

        self.log_label.config(text=f"Loaded: {os.path.basename(path)}  ({len(self.log_records)} lines)")

        self.line_start_var.set(1)
        self.line_end_var.set(max(1, len(self.log_records)))

        # guess time range
        t_vals = []
        for r in self.log_records:
            if r.obj and "t_ms" in r.obj:
                t = safe_int(r.obj.get("t_ms"))
                if t is not None:
                    t_vals.append(t)
        if t_vals:
            self.t_start_var.set(str(min(t_vals)))
            self.t_end_var.set(str(max(t_vals)))
        else:
            self.t_start_var.set("")
            self.t_end_var.set("")

        self.update_stats_and_ui()
        self.status_var.set(f"Loaded log: {os.path.basename(path)}")

    def load_image(self):
        path = filedialog.askopenfilename(
            title="Select floorplan image",
            filetypes=[("Image files", "*.png *.jpg *.jpeg *.bmp *.tif *.tiff"), ("All files", "*.*")]
        )
        if not path:
            return
        try:
            self.bg_image = mpimg.imread(path)
            self.image_path = path
            self.status_var.set(f"Loaded image: {os.path.basename(path)}")
            self._redraw_all()
        except Exception as e:
            messagebox.showerror("Error", f"Failed to load image:\n{e}")

    # -------------------------
    # Layout save/load
    # -------------------------

    def save_layout(self):
        path = filedialog.asksaveasfilename(
            title="Save layout as JSON",
            defaultextension=".json",
            filetypes=[("JSON files", "*.json")]
        )
        if not path:
            return
        data = {
            "image_path": self.image_path,
            "id_field": self.id_field_var.get(),
            "devices": {
                k: {"role": d.role, "desc": d.desc, "x": d.x, "y": d.y}
                for k, d in self.devices.items()
            },
            "overlay": {
                "show_ids": self.show_ids_var.get(),
                "show_desc": self.show_desc_var.get(),
                "metric": self.metric_var.get(),
                "point_size": self.point_size_var.get(),
                "font_size": self.font_size_var.get(),
                "offline_after_s": self.offline_after_s_var.get(),
                "rtt_outlier_max_ms": self.rtt_outlier_max_ms_var.get(),
            }
        }
        try:
            with open(path, "w", encoding="utf-8") as f:
                json.dump(data, f, indent=2)
            self.status_var.set(f"Saved layout: {os.path.basename(path)}")
        except Exception as e:
            messagebox.showerror("Error", f"Failed to save layout:\n{e}")

    def load_layout(self):
        path = filedialog.askopenfilename(title="Load layout JSON", filetypes=[("JSON files", "*.json")])
        if not path:
            return
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
        except Exception as e:
            messagebox.showerror("Error", f"Failed to load layout:\n{e}")
            return

        self.id_field_var.set(str(data.get("id_field", "auto")))

        devs = data.get("devices", {})
        for k, v in devs.items():
            self.devices[str(k)] = Device(
                key=str(k),
                role=v.get("role", "slave"),
                desc=v.get("desc", ""),
                x=v.get("x", None),
                y=v.get("y", None),
            )

        ov = data.get("overlay", {})
        self.show_ids_var.set(bool(ov.get("show_ids", True)))
        self.show_desc_var.set(bool(ov.get("show_desc", True)))
        self.metric_var.set(str(ov.get("metric", "avg_ms")))
        self.point_size_var.set(int(ov.get("point_size", 55)))
        self.font_size_var.set(int(ov.get("font_size", 10)))
        if "offline_after_s" in ov:
            self.offline_after_s_var.set(float(ov.get("offline_after_s", 2.0)))
        if "rtt_outlier_max_ms" in ov:
            self.rtt_outlier_max_ms_var.set(str(ov.get("rtt_outlier_max_ms", "")))

        imgp = data.get("image_path")
        if imgp and os.path.exists(imgp):
            try:
                self.bg_image = mpimg.imread(imgp)
                self.image_path = imgp
            except Exception:
                pass

        self._refresh_device_table()
        self.update_stats_and_ui()
        self._redraw_all()
        self.status_var.set(f"Loaded layout: {os.path.basename(path)}")

    # -------------------------
    # Stats computation
    # -------------------------

    def _iter_selected_records(self) -> List[LogRecord]:
        if not self.log_records:
            return []

        mode = self.range_mode.get()
        line_start = max(1, int(self.line_start_var.get()))
        line_end = min(len(self.log_records), int(self.line_end_var.get()))
        if line_start > line_end:
            line_start, line_end = line_end, line_start
            self.line_start_var.set(line_start)
            self.line_end_var.set(line_end)

        t_start = safe_int(self.t_start_var.get().strip()) if self.t_start_var.get().strip() else None
        t_end = safe_int(self.t_end_var.get().strip()) if self.t_end_var.get().strip() else None
        if t_start is not None and t_end is not None and t_start > t_end:
            t_start, t_end = t_end, t_start
            self.t_start_var.set(str(t_start))
            self.t_end_var.set(str(t_end))

        out: List[LogRecord] = []
        for rec in self.log_records:
            if mode == "line":
                if line_start <= rec.line_no <= line_end:
                    out.append(rec)
            else:
                if rec.obj is None:
                    continue
                t_ms = safe_int(rec.obj.get("t_ms"))
                if t_ms is None:
                    continue
                if t_start is not None and t_ms < t_start:
                    continue
                if t_end is not None and t_ms > t_end:
                    continue
                out.append(rec)
        return out

    def update_stats_and_ui(self):
        selected = self._iter_selected_records()

        rtt_max_ms = safe_float(self.rtt_outlier_max_ms_var.get().strip()) if self.rtt_outlier_max_ms_var.get().strip() else None

        stats: Dict[str, Dict[str, Any]] = {}
        for rec in selected:
            if rec.obj is None:
                continue
            obj = rec.obj
            status = obj.get("status")
            key = self.pick_device_key(obj)
            if key is None:
                continue

            st = stats.setdefault(key, {
                "ok": 0,
                "fail": 0,
                "rtt_us_list": [],
                "last_t_ms": None,
                "last_rtt_us": None,
            })

            t_ms = safe_int(obj.get("t_ms"))
            if t_ms is not None:
                if st["last_t_ms"] is None or t_ms > st["last_t_ms"]:
                    st["last_t_ms"] = t_ms

            if status == "OK":
                rtt_us = safe_int(obj.get("rtt_us"))
                if rtt_us is None:
                    continue
                if rtt_max_ms is not None and rtt_us > int(rtt_max_ms * 1000.0):
                    continue
                st["ok"] += 1
                st["rtt_us_list"].append(rtt_us)
                st["last_rtt_us"] = rtt_us
            elif status in ("PING_FAILED", "SEND_FAIL", "TIMEOUT"):
                st["fail"] += 1

        # finalize min/avg/max
        for k, st in stats.items():
            rtts = st["rtt_us_list"]
            if rtts:
                st["min_us"] = min(rtts)
                st["max_us"] = max(rtts)
                st["avg_us"] = int(sum(rtts) / len(rtts))
            else:
                st["min_us"] = None
                st["max_us"] = None
                st["avg_us"] = None

        self.last_stats = stats

        # auto-add devices seen in logs (do not overwrite existing desc/pos)
        for k in stats.keys():
            if k not in self.devices:
                self.devices[k] = Device(key=k, role="slave", desc="")

        self._refresh_device_table()
        self._refresh_stats_table()
        self._redraw_all()

    # -------------------------
    # Tables
    # -------------------------

    def _refresh_stats_table(self):
        for item in self.stats_tree.get_children():
            self.stats_tree.delete(item)

        def sort_key(k: str):
            try:
                return (0, int(k))
            except Exception:
                return (1, k)

        for k in sorted(self.last_stats.keys(), key=sort_key):
            st = self.last_stats[k]
            self.stats_tree.insert("", tk.END, values=(
                k,
                st.get("ok", 0),
                st.get("fail", 0),
                ms_str_from_us(st.get("avg_us")),
                ms_str_from_us(st.get("min_us")),
                ms_str_from_us(st.get("max_us")),
                ms_str_from_us(st.get("last_rtt_us")),
                st.get("last_t_ms", "-"),
            ))

    def _refresh_device_table(self, keep_selection: Optional[str] = None):
        sel = keep_selection
        if sel is None:
            current = self.dev_tree.selection()
            sel = current[0] if current else None

        for item in self.dev_tree.get_children():
            self.dev_tree.delete(item)

        def sort_key(k: str):
            try:
                return (0, int(k))
            except Exception:
                return (1, k)

        for k in sorted(self.devices.keys(), key=sort_key):
            d = self.devices[k]
            self.dev_tree.insert("", tk.END, iid=k, values=(
                d.key, d.role, d.desc,
                "-" if d.x is None else f"{d.x:.1f}",
                "-" if d.y is None else f"{d.y:.1f}",
            ))

        if sel and sel in self.devices:
            self.dev_tree.selection_set(sel)

    # -------------------------
    # Device editor fixes
    # -------------------------

    def _on_device_select(self):
        sel = self.dev_tree.selection()
        if not sel:
            return
        key = sel[0]
        d = self.devices.get(key)
        if not d:
            return
        self.dev_key_var.set(d.key)
        self.dev_role_var.set(d.role)
        self.dev_desc_var.set(d.desc)

    def add_or_update_device(self):
        # If ID/key entry is empty, update selected device
        key = self.dev_key_var.get().strip()
        selected = self.dev_tree.selection()
        if not key:
            if selected:
                key = selected[0]
                self.dev_key_var.set(key)
            else:
                messagebox.showinfo("Device", "Enter a device ID/key (e.g. 1, 2, 3, M) or select one from the list.")
                return

        role = self.dev_role_var.get().strip() or "slave"
        desc = self.dev_desc_var.get().strip()

        if key in self.devices:
            d = self.devices[key]
            d.role = role
            d.desc = desc
        else:
            self.devices[key] = Device(key=key, role=role, desc=desc)

        self._refresh_device_table(keep_selection=key)
        self._redraw_all()
        self.status_var.set(f"Updated device '{key}'")

    def remove_device(self):
        sel = self.dev_tree.selection()
        if not sel:
            messagebox.showinfo("Remove", "Select a device to remove.")
            return
        key = sel[0]
        if key in self.devices:
            del self.devices[key]
        if self.placing_device_key == key:
            self.placing_device_key = None
            self.place_mode.set(False)
        self._refresh_device_table()
        self._redraw_all()

    # -------------------------
    # Placement mode fixes
    # -------------------------

    def _on_place_mode_toggle(self):
        if not self.place_mode.get():
            self.placing_device_key = None
            self.status_var.set("Place mode OFF.")
            self._redraw_all()

    def enable_place_selected(self):
        sel = self.dev_tree.selection()
        if not sel:
            messagebox.showinfo("Place", "Select a device first.")
            return
        key = sel[0]
        self.place_mode.set(True)
        self.placing_device_key = key
        self.status_var.set(f"Place mode ON: click the map to place/move device '{key}'")
        self._redraw_all()

    def disable_place_mode(self):
        self.place_mode.set(False)
        self.placing_device_key = None
        self.status_var.set("Place mode OFF.")
        self._redraw_all()

    # -------------------------
    # Map + Timeline drawing
    # -------------------------

    def _redraw_all(self):
        self._draw_map()
        self._draw_timeline()

    def _draw_map(self):
        self.map_ax.clear()
        self.map_ax.set_axis_off()

        if self.bg_image is not None:
            self.map_ax.imshow(self.bg_image, origin="upper")
            h, w = self.bg_image.shape[0], self.bg_image.shape[1]
            # keep pixel-like coordinates stable
            self.map_ax.set_xlim(0, w)
            self.map_ax.set_ylim(h, 0)
        else:
            self.map_ax.text(0.5, 0.5, "Load a floorplan image (File → Load Floorplan Image)",
                             ha="center", va="center", transform=self.map_ax.transAxes)

        # Draw devices
        ps = self.point_size_var.get()
        fs = self.font_size_var.get()
        metric = self.metric_var.get()

        # Compute "online" relative to end of selected range (t_ms)
        t_max = None
        for st in self.last_stats.values():
            t = st.get("last_t_ms")
            if t is not None:
                t_max = t if t_max is None else max(t_max, t)

        offline_after_ms = int(self.offline_after_s_var.get() * 1000.0)

        marker_map = {"slave": "o", "master": "s", "other": "D"}

        for k, d in self.devices.items():
            if d.x is None or d.y is None:
                continue

            st = self.last_stats.get(k)
            online = False
            if st and t_max is not None and st.get("last_t_ms") is not None:
                online = (t_max - st["last_t_ms"]) <= offline_after_ms

            marker = marker_map.get(d.role, "o")

            if online:
                self.map_ax.scatter([d.x], [d.y], s=ps, marker=marker)
            else:
                # offline hollow marker
                self.map_ax.scatter([d.x], [d.y], s=ps, marker=marker, facecolors="none")

            parts = []
            if self.show_ids_var.get():
                parts.append(str(d.key))
            if self.show_desc_var.get() and d.desc:
                parts.append(d.desc)

            if metric != "none" and st is not None:
                if metric == "avg_ms":
                    parts.append(f"avg {ms_str_from_us(st.get('avg_us'))} ms")
                elif metric == "last_ms":
                    parts.append(f"last {ms_str_from_us(st.get('last_rtt_us'))} ms")
                elif metric == "ok_count":
                    parts.append(f"ok {st.get('ok', 0)}")
                elif metric == "fail_count":
                    parts.append(f"fail {st.get('fail', 0)}")
                elif metric == "last_seen":
                    parts.append(f"t {st.get('last_t_ms')}")

            label = " | ".join(parts) if parts else str(d.key)
            self.map_ax.text(d.x + 6, d.y - 6, label, fontsize=fs, va="top")

        if self.place_mode.get() and self.placing_device_key:
            self.map_ax.text(0.01, 0.99, f"Placing: {self.placing_device_key} (click to set)",
                             transform=self.map_ax.transAxes, ha="left", va="top", fontsize=10)

        self.map_fig.tight_layout()
        self.map_canvas.draw_idle()

    def _draw_timeline(self):
        self.time_ax.clear()
        self.time_ax.set_title("Online/Offline timeline (based on OK messages and offline_after)")
        self.time_ax.set_xlabel("t_ms")
        self.time_ax.set_ylabel("device")

        selected = self._iter_selected_records()
        offline_after_ms = int(self.offline_after_s_var.get() * 1000.0)

        # gather OK times per device
        ok_times: Dict[str, List[int]] = {}
        all_t: List[int] = []

        for rec in selected:
            if rec.obj is None:
                continue
            obj = rec.obj
            if obj.get("status") != "OK":
                continue
            key = self.pick_device_key(obj)
            if key is None:
                continue
            t_ms = safe_int(obj.get("t_ms"))
            if t_ms is None:
                continue
            ok_times.setdefault(key, []).append(t_ms)
            all_t.append(t_ms)

        if not all_t:
            self.time_ax.text(0.5, 0.5, "No OK points found in selected range.",
                              transform=self.time_ax.transAxes, ha="center", va="center")
            self.time_fig.tight_layout()
            self.time_canvas.draw_idle()
            return

        t_min = min(all_t)
        t_max = max(all_t)

        # Build online segments per device:
        # Each OK extends online until t + offline_after_ms; overlapping extensions merge.
        def build_segments(times: List[int]) -> List[Tuple[int, int]]:
            times = sorted(times)
            segs: List[Tuple[int, int]] = []
            cur_s = None
            cur_e = None
            for t in times:
                s = t
                e = t + offline_after_ms
                if cur_s is None:
                    cur_s, cur_e = s, e
                else:
                    if s <= cur_e:
                        cur_e = max(cur_e, e)
                    else:
                        segs.append((cur_s, cur_e))
                        cur_s, cur_e = s, e
            if cur_s is not None:
                segs.append((cur_s, cur_e))
            # clip to range
            clipped = []
            for s, e in segs:
                clipped.append((max(s, t_min), min(e, t_max)))
            return clipped

        # Sort device keys nicely
        keys = list(ok_times.keys())

        def sort_key(k: str):
            try:
                return (0, int(k))
            except Exception:
                return (1, k)

        keys.sort(key=sort_key)

        # Plot each device as horizontal segments at y=index
        yticks = []
        ylabels = []
        for yi, k in enumerate(keys):
            segs = build_segments(ok_times[k])
            for s, e in segs:
                if e > s:
                    self.time_ax.plot([s, e], [yi, yi], linewidth=8)  # default color cycle
            yticks.append(yi)
            ylabels.append(k)

        self.time_ax.set_yticks(yticks)
        self.time_ax.set_yticklabels(ylabels)
        self.time_ax.set_ylim(-1, len(keys))
        self.time_ax.set_xlim(t_min, t_max)

        self.time_fig.tight_layout()
        self.time_canvas.draw_idle()

    # -------------------------
    # Map click handler (fixed)
    # -------------------------

    def _on_map_click(self, event):
        if not self.place_mode.get():
            return
        if self.toolbar and getattr(self.toolbar, "mode", ""):
            # If pan/zoom is enabled, matplotlib may steal clicks
            self.status_var.set("Disable Pan/Zoom in the toolbar to place markers.")
            return
        if self.placing_device_key is None:
            self.status_var.set("Select a device and click 'Place selected' first.")
            return
        if event.inaxes != self.map_ax:
            return
        if event.xdata is None or event.ydata is None:
            self.status_var.set("Click inside the image area.")
            return

        key = self.placing_device_key
        d = self.devices.get(key)
        if d is None:
            return

        d.x = float(event.xdata)
        d.y = float(event.ydata)

        self._refresh_device_table(keep_selection=key)
        self._redraw_all()
        self.status_var.set(f"Placed '{key}' at x={d.x:.1f}, y={d.y:.1f}")

    # -------------------------
    # Export annotated image
    # -------------------------

    def export_annotated_image(self):
        if self.bg_image is None:
            messagebox.showinfo("Export", "Load an image first.")
            return
        path = filedialog.asksaveasfilename(
            title="Export annotated image",
            defaultextension=".png",
            filetypes=[("PNG files", "*.png"), ("All files", "*.*")]
        )
        if not path:
            return

        try:
            h, w = self.bg_image.shape[0], self.bg_image.shape[1]
            dpi = 100
            fig = Figure(figsize=(w / dpi, h / dpi), dpi=dpi)
            ax = fig.add_subplot(111)
            ax.set_axis_off()
            ax.imshow(self.bg_image, origin="upper")
            ax.set_xlim(0, w)
            ax.set_ylim(h, 0)

            # Reuse same overlay drawing as map
            ps = self.point_size_var.get()
            fs = self.font_size_var.get()
            metric = self.metric_var.get()
            offline_after_ms = int(self.offline_after_s_var.get() * 1000.0)

            t_max = None
            for st in self.last_stats.values():
                t = st.get("last_t_ms")
                if t is not None:
                    t_max = t if t_max is None else max(t_max, t)

            marker_map = {"slave": "o", "master": "s", "other": "D"}

            for k, d in self.devices.items():
                if d.x is None or d.y is None:
                    continue
                st = self.last_stats.get(k)
                online = False
                if st and t_max is not None and st.get("last_t_ms") is not None:
                    online = (t_max - st["last_t_ms"]) <= offline_after_ms

                marker = marker_map.get(d.role, "o")
                if online:
                    ax.scatter([d.x], [d.y], s=ps, marker=marker)
                else:
                    ax.scatter([d.x], [d.y], s=ps, marker=marker, facecolors="none")

                parts = []
                if self.show_ids_var.get():
                    parts.append(str(d.key))
                if self.show_desc_var.get() and d.desc:
                    parts.append(d.desc)
                if metric != "none" and st is not None:
                    if metric == "avg_ms":
                        parts.append(f"avg {ms_str_from_us(st.get('avg_us'))} ms")
                    elif metric == "last_ms":
                        parts.append(f"last {ms_str_from_us(st.get('last_rtt_us'))} ms")
                    elif metric == "ok_count":
                        parts.append(f"ok {st.get('ok', 0)}")
                    elif metric == "fail_count":
                        parts.append(f"fail {st.get('fail', 0)}")
                    elif metric == "last_seen":
                        parts.append(f"t {st.get('last_t_ms')}")
                label = " | ".join(parts) if parts else str(d.key)
                ax.text(d.x + 6, d.y - 6, label, fontsize=fs, va="top")

            fig.savefig(path, dpi=dpi, bbox_inches="tight", pad_inches=0)
            self.status_var.set(f"Exported: {os.path.basename(path)}")
        except Exception as e:
            messagebox.showerror("Error", f"Failed to export image:\n{e}")


if __name__ == "__main__":
    App().mainloop()
