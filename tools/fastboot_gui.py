# -*- coding: utf-8 -*-
"""fastbootCmds 调试界面（PySide6）。

用途：不打开终端也能反复试各种命令；每条命令的完整 argv、退出码、耗时、子进程 PID
都会显示出来。拿 PID 就能在 VS Code 里用「⑤ 附加到运行中的 fastbootCmds」下断点。

单独运行：
    python tools/fastboot_gui.py

无窗口自检（CI / 无桌面环境）：
    python tools/fastboot_gui.py --smoke
"""

from __future__ import annotations

import os
import sys
import time

from PySide6.QtCore import QProcess, QSettings, Qt
from PySide6.QtGui import QFont, QTextCursor
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QFileDialog,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMainWindow,
    QPlainTextEdit,
    QPushButton,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_EXE = os.path.join(ROOT, "fastbootCmds.exe")

PRESETS = [
    "",
    "devices",
    "getvar",
    "getvar all",
    "reboot",
    "reboot bootloader",
    "reboot recovery",
    "erase misc",
    "flash boot",
    "oem recovery lowpoer",
    "flashing unlock",
    "set_active a",
    "boot",
    "continue",
]


def build_argv(exe: str, command: str, image: str, selector: str, hub: int, port: int) -> list[str]:
    """把界面上的字段拼成最终 argv（纯函数，方便单测/自检）。"""
    argv: list[str] = []
    tokens = command.split()
    argv += tokens
    if image.strip():
        argv.append(image.strip())

    # devices 只列设备，不接受 hub index / all
    if tokens and tokens[0] == "devices":
        return argv

    if selector == "all":
        argv.append("all")
    elif selector == "hub":
        argv += [str(hub), str(port)]
    return argv


def decode(raw: bytes) -> str:
    """程序输出可能是 UTF-8 也可能是 GBK，逐个尝试。"""
    for enc in ("utf-8", "gbk"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", "replace")


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("fastbootCmds 调试台")
        self.resize(1040, 760)

        self._buf = b""
        self._started_at = 0.0
        self._settings = QSettings("jwl2626", "fastbootCmdsGUI")

        self.proc = QProcess(self)
        self.proc.setProcessChannelMode(QProcess.MergedChannels)  # stdout + stderr 合并
        self.proc.readyReadStandardOutput.connect(self._on_read)
        self.proc.finished.connect(self._on_finished)
        self.proc.errorOccurred.connect(self._on_error)

        self._build_ui()
        self._load_settings()
        self._refresh_preview()

    # ---------------- UI ----------------
    def _build_ui(self) -> None:
        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)
        root.setContentsMargins(12, 12, 12, 12)
        root.setSpacing(10)

        # 程序路径
        box_exe = QGroupBox("程序")
        row_exe = QHBoxLayout(box_exe)
        self.ed_exe = QLineEdit(DEFAULT_EXE)
        self.ed_exe.editingFinished.connect(self._refresh_preview)
        btn_browse_exe = QPushButton("浏览…")
        btn_browse_exe.clicked.connect(self._pick_exe)
        row_exe.addWidget(self.ed_exe, 1)
        row_exe.addWidget(btn_browse_exe)
        root.addWidget(box_exe)

        # 命令
        box_cmd = QGroupBox("命令")
        grid = QGridLayout(box_cmd)
        self.cb_cmd = QComboBox()
        self.cb_cmd.setEditable(True)
        self.cb_cmd.addItems(PRESETS)
        self.cb_cmd.currentTextChanged.connect(self._refresh_preview)

        self.ed_image = QLineEdit()
        self.ed_image.setPlaceholderText("可选：镜像文件（flash 时用），例如 boot.img")
        self.ed_image.textChanged.connect(self._refresh_preview)
        btn_image = QPushButton("浏览…")
        btn_image.clicked.connect(self._pick_image)

        grid.addWidget(QLabel("命令："), 0, 0)
        grid.addWidget(self.cb_cmd, 0, 1, 1, 3)
        grid.addWidget(QLabel("镜像："), 1, 0)
        grid.addWidget(self.ed_image, 1, 1, 1, 2)
        grid.addWidget(btn_image, 1, 3)

        # 目标设备
        box_dev = QGroupBox("目标设备")
        row_dev = QHBoxLayout(box_dev)
        self.cb_sel = QComboBox()
        self.cb_sel.addItems(["第一台（默认）", "指定 hub index", "全部（all）"])
        self.cb_sel.currentIndexChanged.connect(self._refresh_preview)
        self.sp_hub = QSpinBox()
        self.sp_hub.setRange(1, 16)
        self.sp_hub.setValue(1)
        self.sp_port = QSpinBox()
        self.sp_port.setRange(1, 16)
        self.sp_port.setValue(3)
        self.sp_hub.valueChanged.connect(self._refresh_preview)
        self.sp_port.valueChanged.connect(self._refresh_preview)

        row_dev.addWidget(QLabel("选择："))
        row_dev.addWidget(self.cb_sel)
        row_dev.addSpacing(12)
        row_dev.addWidget(QLabel("hub"))
        row_dev.addWidget(self.sp_hub)
        row_dev.addWidget(QLabel("port"))
        row_dev.addWidget(self.sp_port)
        row_dev.addStretch(1)
        self.chk_clear = QCheckBox("运行前清空输出")
        self.chk_clear.setChecked(True)
        row_dev.addWidget(self.chk_clear)

        root.addWidget(box_cmd)
        root.addWidget(box_dev)

        # 操作
        row_btn = QHBoxLayout()
        self.btn_run = QPushButton("运行  (Ctrl+Enter)")
        self.btn_run.setDefault(True)
        self.btn_run.clicked.connect(self.run_command)
        btn_clear = QPushButton("清空")
        btn_clear.clicked.connect(lambda: self.txt_output.clear())
        btn_copy = QPushButton("复制输出")
        btn_copy.clicked.connect(self._copy_output)
        row_btn.addWidget(self.btn_run)
        row_btn.addWidget(btn_clear)
        row_btn.addWidget(btn_copy)
        row_btn.addStretch(1)
        root.addLayout(row_btn)

        self.lb_preview = QLabel()
        self.lb_preview.setTextInteractionFlags(Qt.TextSelectableByMouse)
        mono = QFont("Consolas", 10)
        mono.setStyleHint(QFont.Monospace)
        self.lb_preview.setFont(mono)
        self.lb_preview.setStyleSheet("color:#555;")
        root.addWidget(self.lb_preview)

        self.txt_output = QPlainTextEdit()
        self.txt_output.setReadOnly(True)
        self.txt_output.setFont(mono)
        self.txt_output.setStyleSheet(
            "QPlainTextEdit{background:#1e1e1e;color:#d4d4d4;border:1px solid #333;}"
        )
        root.addWidget(self.txt_output, 1)

        self.statusBar().showMessage("就绪")

    # ---------------- 行为 ----------------
    def _pick_exe(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "选择 fastbootCmds.exe", ROOT, "可执行文件 (*.exe)")
        if path:
            self.ed_exe.setText(path)
            self._refresh_preview()

    def _pick_image(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "选择镜像", ROOT, "镜像 (*.img *.bin);;所有文件 (*)")
        if path:
            self.ed_image.setText(path)

    def _selector(self) -> str:
        return {1: "hub", 2: "all"}.get(self.cb_sel.currentIndex(), "first")

    def _refresh_preview(self) -> None:
        argv = build_argv(
            self.ed_exe.text(),
            self.cb_cmd.currentText(),
            self.ed_image.text(),
            self._selector(),
            self.sp_hub.value(),
            self.sp_port.value(),
        )
        name = os.path.basename(self.ed_exe.text()) or "fastbootCmds.exe"
        self.lb_preview.setText(f"> {name} " + " ".join(argv))

    def run_command(self) -> None:
        if self.proc.state() != QProcess.NotRunning:
            self.append("[busy] 上一条命令还在跑，等它结束或先停掉。")
            return

        exe = self.ed_exe.text().strip()
        if not os.path.isfile(exe):
            self.append(f"[error] 找不到可执行文件：{exe}")
            return

        argv = build_argv(
            exe,
            self.cb_cmd.currentText(),
            self.ed_image.text(),
            self._selector(),
            self.sp_hub.value(),
            self.sp_port.value(),
        )

        if self.chk_clear.isChecked():
            self.txt_output.clear()

        self._buf = b""
        self._started_at = time.time()
        self.btn_run.setEnabled(False)

        self.append("")  # 空行分隔
        self.append("$ " + " ".join(argv))
        self.append("-" * 60)

        self.proc.setProgram(exe)
        self.proc.setArguments(argv[1:])
        self.proc.setWorkingDirectory(os.path.dirname(exe) or ROOT)
        self.proc.start()

        if not self.proc.waitForStarted(3000):
            self.append(f"[error] 启动失败：{self.proc.errorString()}")
            self.btn_run.setEnabled(True)
            return
        pid = self.proc.processId()
        self.append(f"[pid] {pid}    （VS Code 里用「⑤ 附加到运行中的 fastbootCmds」选这个 PID）")
        self.statusBar().showMessage(f"运行中… PID={pid}")
        self._save_settings()

    def _on_read(self) -> None:
        self._buf += bytes(self.proc.readAllStandardOutput())
        *lines, self._buf = self._buf.split(b"\n")
        for line in lines:
            self.append(decode(line).rstrip("\r"))

    def _on_finished(self, code: int, status) -> None:
        if self._buf:
            self.append(decode(self._buf).rstrip("\r"))
            self._buf = b""
        elapsed = time.time() - self._started_at
        self.append("-" * 60)
        self.append(f"[exit] code={code}  耗时 {elapsed:.2f}s")
        self.statusBar().showMessage(f"结束：code={code}，耗时 {elapsed:.2f}s")
        self.btn_run.setEnabled(True)

    def _on_error(self, err) -> None:
        self.append(f"[process error] {err}: {self.proc.errorString()}")
        self.btn_run.setEnabled(True)

    def append(self, text: str) -> None:
        self.txt_output.appendPlainText(text)
        self.txt_output.moveCursor(QTextCursor.End)

    def _copy_output(self) -> None:
        QApplication.clipboard().setText(self.txt_output.toPlainText())
        self.statusBar().showMessage("已复制到剪贴板")

    def keyPressEvent(self, event) -> None:
        if event.key() in (Qt.Key_Return, Qt.Key_Enter) and event.modifiers() & Qt.ControlModifier:
            self.run_command()
            return
        super().keyPressEvent(event)

    # ---------------- 设置持久化 ----------------
    def _save_settings(self) -> None:
        self._settings.setValue("exe", self.ed_exe.text())
        self._settings.setValue("cmd", self.cb_cmd.currentText())
        self._settings.setValue("image", self.ed_image.text())
        self._settings.setValue("selector", self.cb_sel.currentIndex())
        self._settings.setValue("hub", self.sp_hub.value())
        self._settings.setValue("port", self.sp_port.value())

    def _load_settings(self) -> None:
        self.ed_exe.setText(self._settings.value("exe", DEFAULT_EXE))
        self.cb_cmd.setCurrentText(self._settings.value("cmd", ""))
        self.ed_image.setText(self._settings.value("image", ""))
        self.cb_sel.setCurrentIndex(int(self._settings.value("selector", 0)))
        self.sp_hub.setValue(int(self._settings.value("hub", 1)))
        self.sp_port.setValue(int(self._settings.value("port", 3)))


def smoke() -> int:
    """无窗口自检：不显示界面，跑一次 devices，验证 argv 与进程链路。"""
    app = QApplication(sys.argv)
    win = MainWindow()
    win.cb_cmd.setCurrentText("devices")
    win.run_command()

    deadline = time.time() + 10
    while time.time() < deadline and win.proc.state() != QProcess.NotRunning:
        app.processEvents()
        time.sleep(0.05)
    app.processEvents()

    out = win.txt_output.toPlainText()
    print(out)
    ok = "[exit]" in out and win.proc.state() == QProcess.NotRunning
    print("SMOKE:", "OK" if ok else "FAILED")
    return 0 if ok else 1


def main() -> int:
    if "--smoke" in sys.argv:
        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
        return smoke()

    app = QApplication(sys.argv)
    app.setStyle("Fusion")
    win = MainWindow()
    win.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
