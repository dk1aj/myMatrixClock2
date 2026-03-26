import sys
import re
import socket
import struct
import time
from datetime import datetime

import serial
import serial.tools.list_ports

from PySide6.QtCore import QTimer, Qt
from PySide6.QtWidgets import (
    QApplication,
    QWidget,
    QLabel,
    QPushButton,
    QComboBox,
    QLineEdit,
    QVBoxLayout,
    QHBoxLayout,
    QMessageBox,
    QRadioButton,
    QButtonGroup,
)

NTP_SERVER_HOST = "fritz.box"
NTP_SERVER_PORT = 123
NTP_QUERY_INTERVAL_SECONDS = 30
NTP_SOCKET_TIMEOUT_SECONDS = 1.0
NTP_TIMESTAMP_DELTA = 2208988800


class SerialClockSender(QWidget):
    def __init__(self):
        super().__init__()

        self.serial_port = None
        self.serial_read_buffer = ""
        self.last_ntp_sync_monotonic = 0.0
        self.last_ntp_local_datetime = None
        self.ntp_sync_error = None

        self.setWindowTitle("Serial Time Sender")
        self.setFixedSize(700, 620)

        self.ntp_time_label = QLabel("Waiting for fritz.box NTP...")
        self.ntp_time_label.setAlignment(Qt.AlignCenter)
        self.ntp_time_label.setStyleSheet("""
            font-size: 34px;
            font-weight: bold;
            border: 2px solid #444;
            padding: 12px;
            background-color: black;
            color: #00FF00;
        """)

        self.rtc_time_label = QLabel("No RTC data")
        self.rtc_time_label.setAlignment(Qt.AlignCenter)
        self.rtc_time_label.setStyleSheet("""
            font-size: 28px;
            font-weight: bold;
            border: 2px solid #444;
            padding: 12px;
            background-color: #101820;
            color: #FFD166;
        """)

        self.utc_time_label = QLabel("--:--:--")
        self.utc_time_label.setAlignment(Qt.AlignCenter)
        self.utc_time_label.setStyleSheet("""
            font-size: 30px;
            font-weight: bold;
            border: 2px solid #444;
            padding: 12px;
            background-color: #1b1f3b;
            color: #7FDBFF;
        """)

        self.port_combo = QComboBox()
        self.port_combo.setMinimumHeight(32)

        self.refresh_button = QPushButton("Refresh COM Ports")
        self.refresh_button.setMinimumHeight(32)

        self.baud_edit = QLineEdit("9600")
        self.baud_edit.setMinimumHeight(32)

        self.open_button = QPushButton("Open Port")
        self.open_button.setMinimumHeight(40)

        self.close_button = QPushButton("Close Port")
        self.close_button.setMinimumHeight(40)
        self.close_button.setEnabled(False)

        self.send_button = QPushButton("SEND TIME")
        self.send_button.setMinimumHeight(80)
        self.send_button.setStyleSheet("""
            font-size: 22px;
            font-weight: bold;
        """)
        self.send_button.setEnabled(False)

        self.status_label = QLabel("Ready.")
        self.status_label.setStyleSheet("font-size: 14px;")

        self.radio_system_time = QRadioButton("Use Fritz!Box NTP time")
        self.radio_manual_time = QRadioButton("Set time manually")
        self.radio_system_time.setChecked(True)

        self.time_mode_group = QButtonGroup(self)
        self.time_mode_group.addButton(self.radio_system_time)
        self.time_mode_group.addButton(self.radio_manual_time)

        self.manual_time_edit = QLineEdit()
        self.manual_time_edit.setPlaceholderText("YYYY-MM-DD HH:MM:SS")
        self.manual_time_edit.setEnabled(False)
        self.manual_time_edit.setMinimumHeight(32)

        self.use_displayed_time_button = QPushButton("Use displayed NTP time")
        self.use_displayed_time_button.setEnabled(False)
        self.use_displayed_time_button.setMinimumHeight(32)

        port_layout = QHBoxLayout()
        port_layout.addWidget(QLabel("COM Port:"))
        port_layout.addWidget(self.port_combo)
        port_layout.addWidget(self.refresh_button)

        baud_layout = QHBoxLayout()
        baud_layout.addWidget(QLabel("Baudrate:"))
        baud_layout.addWidget(self.baud_edit)

        port_button_layout = QHBoxLayout()
        port_button_layout.addWidget(self.open_button)
        port_button_layout.addWidget(self.close_button)

        mode_layout = QVBoxLayout()
        mode_layout.addWidget(QLabel("Time source:"))
        mode_layout.addWidget(self.radio_system_time)
        mode_layout.addWidget(self.radio_manual_time)

        manual_layout = QHBoxLayout()
        manual_layout.addWidget(QLabel("Time value:"))
        manual_layout.addWidget(self.manual_time_edit)
        manual_layout.addWidget(self.use_displayed_time_button)

        main_layout = QVBoxLayout()
        main_layout.addWidget(QLabel("Anzeige 1: Zeitquelle Fritz!Box NTP"))
        main_layout.addWidget(self.ntp_time_label)
        main_layout.addWidget(QLabel("Anzeige 2: Zeitquelle RTC aus dem Geraet via Serial"))
        main_layout.addWidget(self.rtc_time_label)
        main_layout.addWidget(QLabel("Anzeige 3: Zeitquelle UTC aus dem Geraet via Serial"))
        main_layout.addWidget(self.utc_time_label)
        main_layout.addLayout(port_layout)
        main_layout.addLayout(baud_layout)
        main_layout.addLayout(port_button_layout)
        main_layout.addLayout(mode_layout)
        main_layout.addLayout(manual_layout)
        main_layout.addWidget(self.send_button)
        main_layout.addWidget(self.status_label)

        self.setLayout(main_layout)

        self.clock_timer = QTimer(self)
        self.clock_timer.timeout.connect(self.update_clock)
        self.clock_timer.start(200)

        self.refresh_button.clicked.connect(self.refresh_ports)
        self.open_button.clicked.connect(self.open_port)
        self.close_button.clicked.connect(self.close_port)
        self.send_button.clicked.connect(self.send_time)

        self.radio_system_time.toggled.connect(self.update_time_mode)
        self.radio_manual_time.toggled.connect(self.update_time_mode)
        self.use_displayed_time_button.clicked.connect(self.copy_displayed_time_to_manual)

        self.refresh_ports()
        self.sync_ntp_time(force=True)
        self.update_clock()
        self.update_time_mode()

    def update_clock(self):
        self.sync_ntp_time()
        self.update_ntp_time_display()
        self.poll_serial_port()

    def sync_ntp_time(self, force=False):
        now_monotonic = time.monotonic()
        if not force and self.last_ntp_local_datetime is not None:
            if now_monotonic - self.last_ntp_sync_monotonic < NTP_QUERY_INTERVAL_SECONDS:
                return

        try:
            self.last_ntp_local_datetime = self.fetch_ntp_time_from_fritz_box()
            self.last_ntp_sync_monotonic = now_monotonic
            self.ntp_sync_error = None
        except OSError as e:
            self.ntp_sync_error = str(e)

    def fetch_ntp_time_from_fritz_box(self):
        ntp_packet = bytearray(48)
        ntp_packet[0] = 0x1B

        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as ntp_socket:
            ntp_socket.settimeout(NTP_SOCKET_TIMEOUT_SECONDS)
            ntp_socket.sendto(ntp_packet, (NTP_SERVER_HOST, NTP_SERVER_PORT))
            response_packet, _ = ntp_socket.recvfrom(48)

        if len(response_packet) < 48:
            raise OSError("Incomplete NTP response from fritz.box")

        transmit_timestamp = struct.unpack("!I", response_packet[40:44])[0]
        unix_timestamp = transmit_timestamp - NTP_TIMESTAMP_DELTA
        return datetime.fromtimestamp(unix_timestamp)

    def get_current_ntp_time(self):
        if self.last_ntp_local_datetime is None:
            return None

        elapsed_seconds = time.monotonic() - self.last_ntp_sync_monotonic
        return self.last_ntp_local_datetime.timestamp() + elapsed_seconds

    def update_ntp_time_display(self):
        current_ntp_timestamp = self.get_current_ntp_time()
        if current_ntp_timestamp is None:
            self.ntp_time_label.setText("NTP unavailable")
            return

        current_ntp_time = datetime.fromtimestamp(current_ntp_timestamp)
        self.ntp_time_label.setText(current_ntp_time.strftime("%Y-%m-%d %H:%M:%S"))

    def poll_serial_port(self):
        if self.serial_port is None or not self.serial_port.is_open:
            return

        try:
            bytes_waiting = self.serial_port.in_waiting
        except (serial.SerialException, OSError) as e:
            QMessageBox.critical(self, "Serial Error", str(e))
            self.status_label.setText("Serial read failed.")
            self.close_port()
            return

        if bytes_waiting <= 0:
            return

        try:
            incoming_data = self.serial_port.read(bytes_waiting).decode("ascii", errors="ignore")
        except (serial.SerialException, OSError) as e:
            QMessageBox.critical(self, "Serial Error", str(e))
            self.status_label.setText("Serial read failed.")
            self.close_port()
            return

        self.serial_read_buffer += incoming_data

        while "\n" in self.serial_read_buffer:
            raw_line, self.serial_read_buffer = self.serial_read_buffer.split("\n", 1)
            self.handle_serial_line(raw_line.strip())

    def handle_serial_line(self, line):
        if not line:
            return

        rtc_line_match = re.match(
            r"^(\d{1,2}:\d{2}:\d{2} \d{1,2}\.\d{1,2}\.\d{4} .+ wday=\d+)$",
            line,
        )
        if rtc_line_match:
            self.rtc_time_label.setText(rtc_line_match.group(1))
            return

        utc_line_match = re.match(r"^UTC (\d{1,2}:\d{2}:\d{2}) \d{1,2}\.\d{1,2}\.\d{4}$", line)
        if utc_line_match:
            self.utc_time_label.setText(utc_line_match.group(1))
            return

        rtc_set_match = re.match(r"^RTC set to: (\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})$", line)
        if rtc_set_match:
            self.rtc_time_label.setText(rtc_set_match.group(1)[11:])

    def refresh_ports(self):
        self.port_combo.clear()
        ports = list(serial.tools.list_ports.comports())

        for port in ports:
            desc = port.description if port.description else "No description"
            text = f"{port.device} - {desc}"
            self.port_combo.addItem(text, port.device)

        if ports:
            self.status_label.setText("COM ports updated.")
        else:
            self.status_label.setText("No COM ports found.")

    def update_time_mode(self):
        manual_mode = self.radio_manual_time.isChecked()
        self.manual_time_edit.setEnabled(manual_mode)
        self.use_displayed_time_button.setEnabled(manual_mode)

        if manual_mode:
            self.status_label.setText("Manual time mode active.")
        else:
            self.status_label.setText("Fritz!Box NTP time mode active.")

    def copy_displayed_time_to_manual(self):
        current_ntp_timestamp = self.get_current_ntp_time()
        if current_ntp_timestamp is None:
            self.manual_time_edit.clear()
            return

        current_ntp_time = datetime.fromtimestamp(current_ntp_timestamp)
        self.manual_time_edit.setText(current_ntp_time.strftime("%Y-%m-%d %H:%M:%S"))

    def open_port(self):
        if self.serial_port is not None and self.serial_port.is_open:
            self.status_label.setText("Port already open.")
            return

        if self.port_combo.count() == 0:
            QMessageBox.critical(self, "Error", "No COM port available.")
            return

        port_name = self.port_combo.currentData()

        try:
            baudrate = int(self.baud_edit.text().strip())
        except ValueError:
            QMessageBox.critical(self, "Error", "Invalid baudrate.")
            return

        try:
            self.serial_port = serial.Serial(port_name, baudrate, timeout=1)
            self.serial_read_buffer = ""
            self.status_label.setText(f"Port opened: {port_name} @ {baudrate} baud")

            self.open_button.setEnabled(False)
            self.close_button.setEnabled(True)
            self.send_button.setEnabled(True)

            self.port_combo.setEnabled(False)
            self.baud_edit.setEnabled(False)
            self.refresh_button.setEnabled(False)

        except serial.SerialException as e:
            self.serial_port = None
            QMessageBox.critical(self, "Serial Error", str(e))
            self.status_label.setText("Failed to open port.")

    def close_port(self):
        if self.serial_port is not None:
            try:
                if self.serial_port.is_open:
                    self.serial_port.close()
            except Exception:
                pass

        self.serial_port = None
        self.serial_read_buffer = ""
        self.status_label.setText("Port closed.")
        self.rtc_time_label.setText("No RTC data")
        self.utc_time_label.setText("--:--:--")

        self.open_button.setEnabled(True)
        self.close_button.setEnabled(False)
        self.send_button.setEnabled(False)

        self.port_combo.setEnabled(True)
        self.baud_edit.setEnabled(True)
        self.refresh_button.setEnabled(True)

    def get_time_to_send(self):
        if self.radio_system_time.isChecked():
            current_ntp_timestamp = self.get_current_ntp_time()
            if current_ntp_timestamp is None:
                raise ValueError("No NTP time available from fritz.box.")

            current_ntp_time = datetime.fromtimestamp(current_ntp_timestamp)
            return current_ntp_time.strftime("%Y-%m-%d %H:%M:%S")

        manual_text = self.manual_time_edit.text().strip()
        if not manual_text:
            raise ValueError("No manual time entered.")

        try:
            parsed = datetime.strptime(manual_text, "%Y-%m-%d %H:%M:%S")
            return parsed.strftime("%Y-%m-%d %H:%M:%S")
        except ValueError:
            raise ValueError("Invalid format. Use YYYY-MM-DD HH:MM:SS")

    def send_time(self):
        if self.serial_port is None or not self.serial_port.is_open:
            QMessageBox.critical(self, "Error", "Port is not open.")
            return

        try:
            time_value = self.get_time_to_send()
        except ValueError as e:
            QMessageBox.critical(self, "Error", str(e))
            return

        message = time_value + "\r\n"

        try:
            self.serial_port.write(message.encode("ascii"))
            self.status_label.setText(f"Sent: {time_value}")
        except serial.SerialException as e:
            QMessageBox.critical(self, "Serial Error", str(e))
            self.status_label.setText("Send failed.")
            self.close_port()

    def closeEvent(self, event):
        self.close_port()
        event.accept()


if __name__ == "__main__":
    app = QApplication(sys.argv)
    window = SerialClockSender()
    window.show()
    sys.exit(app.exec())
