from __future__ import annotations

import sys
import time
from pathlib import Path

import psutil
from PyQt6.QtCore import QProcess, QSettings, QTimer
from PyQt6.QtGui import QFont
from PyQt6.QtWidgets import (
    QApplication,
    QFileDialog,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QMainWindow,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QStyle,
    QVBoxLayout,
    QWidget,
    QLineEdit,
)


def runtime_root() -> Path:
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parents[2]


def format_duration(seconds: float) -> str:
    total = max(0, int(seconds))
    hours, remainder = divmod(total, 3600)
    minutes, seconds = divmod(remainder, 60)
    return f"{hours:02d}:{minutes:02d}:{seconds:02d}"


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("Skynet Manager")
        self.resize(1000, 700)

        self.settings = QSettings("futzhj", "SkynetManager")
        self.process = QProcess(self)
        self.process.setProcessChannelMode(QProcess.ProcessChannelMode.MergedChannels)
        self.started_at: float | None = None
        self.monitored_process: psutil.Process | None = None
        self._last_exit_code: int | None = None
        self.stop_requested = False

        self._build_ui()
        self._restore_settings()

        self.process.started.connect(self._process_started)
        self.process.readyReadStandardOutput.connect(self._read_output)
        self.process.finished.connect(self._process_finished)
        self.process.errorOccurred.connect(self._process_error)

        self.stats_timer = QTimer(self)
        self.stats_timer.setInterval(1000)
        self.stats_timer.timeout.connect(self._update_stats)
        self.stats_timer.start()
        self._set_running_ui(False)

    def _build_ui(self) -> None:
        root = QWidget(self)
        layout = QVBoxLayout(root)

        paths = QGridLayout()
        paths.setColumnStretch(1, 1)

        self.executable_edit = QLineEdit()
        self.executable_edit.setPlaceholderText("Path to skynet.exe")
        browse_executable = QPushButton("Browse")
        browse_executable.setIcon(self.style().standardIcon(QStyle.StandardPixmap.SP_DirOpenIcon))
        browse_executable.clicked.connect(self._browse_executable)
        paths.addWidget(QLabel("Executable"), 0, 0)
        paths.addWidget(self.executable_edit, 0, 1)
        paths.addWidget(browse_executable, 0, 2)

        self.config_edit = QLineEdit()
        self.config_edit.setPlaceholderText("Path to a Skynet config")
        browse_config = QPushButton("Browse")
        browse_config.setIcon(self.style().standardIcon(QStyle.StandardPixmap.SP_DirOpenIcon))
        browse_config.clicked.connect(self._browse_config)
        paths.addWidget(QLabel("Config"), 1, 0)
        paths.addWidget(self.config_edit, 1, 1)
        paths.addWidget(browse_config, 1, 2)
        layout.addLayout(paths)

        controls = QHBoxLayout()
        self.start_button = QPushButton("Start")
        self.start_button.setIcon(self.style().standardIcon(QStyle.StandardPixmap.SP_MediaPlay))
        self.start_button.clicked.connect(self.start_process)
        self.stop_button = QPushButton("Stop")
        self.stop_button.setIcon(self.style().standardIcon(QStyle.StandardPixmap.SP_MediaStop))
        self.stop_button.clicked.connect(self.stop_process)
        self.restart_button = QPushButton("Restart")
        self.restart_button.setIcon(self.style().standardIcon(QStyle.StandardPixmap.SP_BrowserReload))
        self.restart_button.clicked.connect(self.restart_process)
        self.clear_button = QPushButton("Clear log")
        self.clear_button.setIcon(self.style().standardIcon(QStyle.StandardPixmap.SP_TrashIcon))
        self.clear_button.clicked.connect(lambda: self.log.clear())
        self.save_button = QPushButton("Save log")
        self.save_button.setIcon(self.style().standardIcon(QStyle.StandardPixmap.SP_DialogSaveButton))
        self.save_button.clicked.connect(self._save_log)
        for button in (self.start_button, self.stop_button, self.restart_button, self.clear_button, self.save_button):
            controls.addWidget(button)
        controls.addStretch(1)
        layout.addLayout(controls)

        status = QHBoxLayout()
        self.status_label = QLabel("Stopped")
        self.pid_label = QLabel("-")
        self.runtime_label = QLabel("00:00:00")
        self.cpu_label = QLabel("-")
        self.memory_label = QLabel("-")
        for title, value in (
            ("Status", self.status_label),
            ("PID", self.pid_label),
            ("Uptime", self.runtime_label),
            ("CPU", self.cpu_label),
            ("Memory", self.memory_label),
        ):
            status.addWidget(QLabel(f"{title}:"))
            status.addWidget(value)
            status.addSpacing(18)
        status.addStretch(1)
        layout.addLayout(status)

        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setLineWrapMode(QPlainTextEdit.LineWrapMode.NoWrap)
        self.log.setFont(QFont("Consolas", 10))
        layout.addWidget(self.log, 1)

        self.setCentralWidget(root)

    def _restore_settings(self) -> None:
        root = runtime_root()
        local_executable = root / "skynet.exe"
        local_config = root / "examples" / "config"
        if local_executable.is_file() and local_config.is_file():
            executable = str(local_executable)
            config = str(local_config)
        else:
            executable = self.settings.value("executable", str(local_executable), str)
            config = self.settings.value("config", str(local_config), str)
        self.executable_edit.setText(executable)
        self.config_edit.setText(config)

    def _save_settings(self) -> None:
        self.settings.setValue("executable", self.executable_edit.text().strip())
        self.settings.setValue("config", self.config_edit.text().strip())

    def _browse_executable(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self,
            "Select Skynet executable",
            str(Path(self.executable_edit.text()).parent),
            "Executable (*.exe);;All files (*)",
        )
        if path:
            self.executable_edit.setText(path)

    def _browse_config(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self,
            "Select Skynet config",
            str(Path(self.config_edit.text()).parent),
            "Config files (*);;All files (*)",
        )
        if path:
            self.config_edit.setText(path)

    def _append_log(self, text: str) -> None:
        self.log.appendPlainText(text.rstrip("\r\n"))
        scrollbar = self.log.verticalScrollBar()
        scrollbar.setValue(scrollbar.maximum())

    def start_process(self) -> None:
        if self.process.state() != QProcess.ProcessState.NotRunning:
            return

        executable = Path(self.executable_edit.text().strip()).resolve()
        config = Path(self.config_edit.text().strip()).resolve()
        if not executable.is_file():
            QMessageBox.critical(self, "Start failed", f"Executable not found:\n{executable}")
            return
        if not config.is_file():
            QMessageBox.critical(self, "Start failed", f"Config not found:\n{config}")
            return

        working_directory = executable.parent
        try:
            config_argument = config.relative_to(working_directory)
        except ValueError:
            QMessageBox.critical(
                self,
                "Start failed",
                "The config must be inside the Skynet runtime directory.",
            )
            return

        self._save_settings()
        self._last_exit_code = None
        self.stop_requested = False
        self.started_at = time.monotonic()
        self.process.setWorkingDirectory(str(working_directory))
        self._append_log(f"$ {executable} {config_argument}")
        self.status_label.setText("Starting")
        self._set_running_ui(True)
        self.process.start(str(executable), [str(config_argument)])

    def stop_process(self) -> None:
        if self.process.state() == QProcess.ProcessState.NotRunning:
            return
        self.stop_requested = True
        self._append_log("Stopping process...")
        self.process.terminate()
        if not self.process.waitForFinished(1500):
            self._append_log("Process did not exit; terminating it.")
            self.process.kill()
            self.process.waitForFinished(1500)

    def restart_process(self) -> None:
        if self.process.state() != QProcess.ProcessState.NotRunning:
            self.stop_process()
        self.start_process()

    def _process_started(self) -> None:
        self.monitored_process = psutil.Process(self.process.processId())
        self.monitored_process.cpu_percent(None)
        self.status_label.setText("Running")
        self._append_log(f"Started (PID {self.process.processId()})")

    def _read_output(self) -> None:
        data = bytes(self.process.readAllStandardOutput())
        if data:
            self._append_log(data.decode("utf-8", errors="replace"))

    def _process_finished(self, exit_code: int, _exit_status: QProcess.ExitStatus) -> None:
        stopped_by_user = self.stop_requested
        self.stop_requested = False
        self._last_exit_code = exit_code
        self.started_at = None
        self.monitored_process = None
        if stopped_by_user:
            self.status_label.setText("Stopped")
            self._append_log("Process stopped.")
        else:
            self.status_label.setText(f"Exited ({exit_code})")
            self._append_log(f"Process exited with code {exit_code}.")
        self._set_running_ui(False)

    def _process_error(self, error: QProcess.ProcessError) -> None:
        if self.stop_requested and error == QProcess.ProcessError.Crashed:
            return
        if error == QProcess.ProcessError.FailedToStart:
            self.started_at = None
            self.status_label.setText("Failed to start")
            self._set_running_ui(False)
        self._append_log(f"Process error: {error.name}")

    def _update_stats(self) -> None:
        if self.process.state() == QProcess.ProcessState.NotRunning:
            self.pid_label.setText("-")
            self.runtime_label.setText("00:00:00")
            self.cpu_label.setText("-")
            self.memory_label.setText("-")
            return

        pid = self.process.processId()
        self.pid_label.setText(str(pid))
        if self.started_at is not None:
            self.runtime_label.setText(format_duration(time.monotonic() - self.started_at))
        try:
            process = self.monitored_process or psutil.Process(pid)
            self.monitored_process = process
            self.cpu_label.setText(f"{process.cpu_percent(None):.1f}%")
            self.memory_label.setText(f"{process.memory_info().rss / 1024 / 1024:.1f} MB")
        except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
            self.cpu_label.setText("-")
            self.memory_label.setText("-")

    def _set_running_ui(self, running: bool) -> None:
        self.start_button.setEnabled(not running)
        self.stop_button.setEnabled(running)
        self.restart_button.setEnabled(running)

    def _save_log(self) -> None:
        path, _ = QFileDialog.getSaveFileName(self, "Save log", "skynet.log", "Log files (*.log);;All files (*)")
        if path:
            Path(path).write_text(self.log.toPlainText(), encoding="utf-8")

    def closeEvent(self, event) -> None:  # noqa: N802
        self._save_settings()
        if self.process.state() != QProcess.ProcessState.NotRunning:
            self.stop_process()
        event.accept()


def main() -> int:
    app = QApplication(sys.argv)
    app.setApplicationName("Skynet Manager")
    app.setOrganizationName("futzhj")
    window = MainWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
