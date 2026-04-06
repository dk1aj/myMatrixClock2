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
    QPlainTextEdit,
    QGridLayout,
    QGroupBox,
)

NTP_SERVER_HOST = "fritz.box"
NTP_SERVER_PORT = 123
NTP_QUERY_INTERVAL_SECONDS = 30
NTP_SOCKET_TIMEOUT_SECONDS = 1.0
NTP_TIMESTAMP_DELTA = 2208988800
ESP32_SPI_TEST_CASES = [
    ("winter", "Winter", "test winter", "0x01 accepted"),
    ("summer", "Summer", "test summer", "0x01 accepted"),
    ("dst-start-before", "DST Start Before", "test dst-start-before", "0x01 accepted"),
    ("dst-start-at", "DST Start At", "test dst-start-at", "0x01 accepted"),
    ("dst-end-before", "DST End Before", "test dst-end-before", "0x01 accepted"),
    ("dst-end-at", "DST End At", "test dst-end-at", "0x01 accepted"),
    ("invalid-date", "Invalid Date", "test invalid-date", "0x02 parse-error"),
    ("invalid-format", "Invalid Format", "test invalid-format", "0x02 parse-error"),
    ("invalid-terminator", "Invalid Terminator", "invalid", "0x02 parse-error"),
]
ESP32_SPI_TEST_EXPECTATIONS = {key: expected for key, _, _, expected in ESP32_SPI_TEST_CASES}


class SerialClockSender(QWidget):
    def __init__(self):
        """Initialisiert die komplette Anwendung und ihren Laufzeitzustand.

        Die Methode baut Hauptfenster, Monitorfenster und Testfenster auf,
        initialisiert alle seriellen Zustandsvariablen und richtet die
        periodischen GUI-Abläufe ein. Ziel ist, dass nach Abschluss von
        ``__init__`` keine weitere Startlogik außerhalb der Klasse nötig ist.
        """
        super().__init__()

        self.serial_port = None
        self.serial_read_buffer = ""
        self.monitor_serial_port = None
        self.monitor_read_buffer = ""
        self.last_ntp_sync_monotonic = 0.0
        self.last_ntp_local_datetime = None
        self.ntp_sync_error = None
        self.test_result_labels = {}
        self.test_actual_reply_labels = {}
        self.suite_active = False
        self.suite_seen_tests = set()
        self.monitor_window = None
        self.monitor_window_position_locked = False
        self.test_window = None
        self.test_window_position_locked = False

        self.setWindowTitle("Serial Time Sender")
        self.setFixedSize(760, 620)

        self.ntp_time_label = QLabel("Waiting for fritz.box NTP...")
        self.ntp_time_label.setAlignment(Qt.AlignCenter)
        self.ntp_time_label.setStyleSheet("""
            font-size: 16px;
            font-weight: bold;
            border: 2px solid #444;
            padding: 12px;
            background-color: black;
            color: #00FF00;
        """)

        self.rtc_time_label = QLabel("No RTC data")
        self.rtc_time_label.setAlignment(Qt.AlignCenter)
        self.rtc_time_label.setStyleSheet("""
            font-size: 16px;
            font-weight: bold;
            border: 2px solid #444;
            padding: 12px;
            background-color: #101820;
            color: #FFD166;
        """)

        self.utc_time_label = QLabel("--:--:--")
        self.utc_time_label.setAlignment(Qt.AlignCenter)
        self.utc_time_label.setStyleSheet("""
            font-size: 16px;
            font-weight: bold;
            border: 2px solid #444;
            padding: 12px;
            background-color: #1b1f3b;
            color: #7FDBFF;
        """)

        self.port_combo = QComboBox()
        self.port_combo.setMinimumHeight(32)
        self.monitor_port_combo = QComboBox()
        self.monitor_port_combo.setMinimumHeight(32)

        self.refresh_button = QPushButton("Refresh COM Ports")
        self.refresh_button.setMinimumHeight(32)
        self.monitor_refresh_button = QPushButton("Refresh COM Ports")
        self.monitor_refresh_button.setMinimumHeight(32)

        self.baud_edit = QLineEdit("9600")
        self.baud_edit.setMinimumHeight(32)
        self.monitor_baud_edit = QLineEdit("115200")
        self.monitor_baud_edit.setMinimumHeight(32)

        self.open_button = QPushButton("Open Port")
        self.open_button.setMinimumHeight(40)

        self.close_button = QPushButton("Close Port")
        self.close_button.setMinimumHeight(40)
        self.close_button.setEnabled(False)

        self.monitor_open_button = QPushButton("Open Monitor")
        self.monitor_open_button.setMinimumHeight(40)

        self.monitor_close_button = QPushButton("Close Monitor")
        self.monitor_close_button.setMinimumHeight(40)
        self.monitor_close_button.setEnabled(False)

        self.clear_monitor_button = QPushButton("Clear Monitor")
        self.clear_monitor_button.setMinimumHeight(32)

        self.monitor_output = QPlainTextEdit()
        self.monitor_output.setReadOnly(True)
        self.monitor_output.setMaximumBlockCount(500)
        self.monitor_output.setPlaceholderText("ESP32 serial output will appear here...")
        self.monitor_output.setCenterOnScroll(True)

        self.esp32_test_combo = QComboBox()
        self.esp32_test_combo.setMinimumHeight(32)
        for key, label, command, _expected_reply in ESP32_SPI_TEST_CASES:
            if key == "invalid-terminator":
                continue
            self.esp32_test_combo.addItem(label, command)

        self.esp32_run_selected_button = QPushButton("Run Selected Test")
        self.esp32_run_selected_button.setMinimumHeight(36)
        self.esp32_run_all_button = QPushButton("Run Full Suite")
        self.esp32_run_all_button.setMinimumHeight(36)
        self.esp32_invalid_button = QPushButton("Send Invalid Frame")
        self.esp32_invalid_button.setMinimumHeight(36)
        self.esp32_now_button = QPushButton("Send Current NTP Once")
        self.esp32_now_button.setMinimumHeight(36)
        self.esp32_help_button = QPushButton("Show ESP32 Help")
        self.esp32_help_button.setMinimumHeight(36)

        self.esp32_manual_send_button = QPushButton("Send Manual To ESP32")
        self.esp32_manual_send_button.setMinimumHeight(36)
        self.esp32_displayed_send_button = QPushButton("Send Displayed NTP To ESP32")
        self.esp32_displayed_send_button.setMinimumHeight(36)

        self.esp32_command_edit = QLineEdit()
        self.esp32_command_edit.setPlaceholderText("ESP32 command, e.g. send 2026-01-15 12:34:56")
        self.esp32_command_edit.setMinimumHeight(32)
        self.esp32_command_button = QPushButton("Send Command")
        self.esp32_command_button.setMinimumHeight(36)
        self.clear_test_results_button = QPushButton("Clear Test Results")
        self.clear_test_results_button.setMinimumHeight(36)

        self.test_suite_summary_label = QLabel("No suite run.")
        self.test_suite_summary_label.setAlignment(Qt.AlignCenter)
        self.test_suite_summary_label.setStyleSheet("""
            font-size: 15px;
            font-weight: bold;
            border: 2px solid #444;
            padding: 8px;
            background-color: #1f2430;
            color: #d8dee9;
        """)

        self.time_tx_label = QLabel("No TIME_TX data")
        self.time_tx_label.setAlignment(Qt.AlignCenter)
        self.time_tx_label.setStyleSheet("""
            font-size: 16px;
            font-weight: bold;
            border: 2px solid #444;
            padding: 8px;
            background-color: #202020;
            color: #F6C177;
        """)

        self.time_info_label = QLabel("No TIME_INFO data")
        self.time_info_label.setAlignment(Qt.AlignLeft | Qt.AlignTop)
        self.time_info_label.setWordWrap(True)
        self.time_info_label.setStyleSheet("""
            font-size: 14px;
            font-weight: bold;
            border: 2px solid #444;
            padding: 8px;
            background-color: #18222d;
            color: #8BD3DD;
        """)

        self.esp32_spi_status_history = QPlainTextEdit()
        self.esp32_spi_status_history.setReadOnly(True)
        self.esp32_spi_status_history.setMaximumBlockCount(300)
        self.esp32_spi_status_history.setPlaceholderText("ESP32 SPI status history...")
        self.esp32_spi_status_history.setCenterOnScroll(True)

        self.send_button = QPushButton("SEND TIME")
        self.send_button.setMinimumHeight(80)
        self.send_button.setStyleSheet("""
            font-size: 22px;
            font-weight: bold;
        """)
        self.send_button.setEnabled(False)

        self.show_monitor_window_button = QPushButton("Show ESP32 Monitor")
        self.show_monitor_window_button.setMinimumHeight(36)
        self.show_test_window_button = QPushButton("Show SPI Test Suite")
        self.show_test_window_button.setMinimumHeight(36)

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

        monitor_port_layout = QHBoxLayout()
        monitor_port_layout.addWidget(QLabel("COM Port:"))
        monitor_port_layout.addWidget(self.monitor_port_combo)
        monitor_port_layout.addWidget(self.monitor_refresh_button)

        monitor_baud_layout = QHBoxLayout()
        monitor_baud_layout.addWidget(QLabel("Baudrate:"))
        monitor_baud_layout.addWidget(self.monitor_baud_edit)

        port_button_layout = QHBoxLayout()
        port_button_layout.addWidget(self.open_button)
        port_button_layout.addWidget(self.close_button)

        monitor_button_layout = QHBoxLayout()
        monitor_button_layout.addWidget(self.monitor_open_button)
        monitor_button_layout.addWidget(self.monitor_close_button)
        monitor_button_layout.addWidget(self.clear_monitor_button)

        teensy_comm_layout = QVBoxLayout()
        teensy_comm_layout.addWidget(QLabel("Teensy COM"))
        teensy_comm_layout.addLayout(port_layout)
        teensy_comm_layout.addLayout(baud_layout)
        teensy_comm_layout.addLayout(port_button_layout)

        monitor_comm_layout = QVBoxLayout()
        monitor_comm_layout.addWidget(QLabel("ESP32 Monitor COM"))
        monitor_comm_layout.addLayout(monitor_port_layout)
        monitor_comm_layout.addLayout(monitor_baud_layout)
        monitor_comm_layout.addLayout(monitor_button_layout)

        mode_layout = QVBoxLayout()
        mode_layout.addWidget(QLabel("Time source:"))
        mode_layout.addWidget(self.radio_system_time)
        mode_layout.addWidget(self.radio_manual_time)

        manual_layout = QHBoxLayout()
        manual_layout.addWidget(QLabel("Time value:"))
        manual_layout.addWidget(self.manual_time_edit)
        manual_layout.addWidget(self.use_displayed_time_button)

        esp32_test_group = QGroupBox("ESP32 SPI Tests")
        esp32_test_layout = QVBoxLayout()

        esp32_test_select_layout = QHBoxLayout()
        esp32_test_select_layout.addWidget(QLabel("Canned test:"))
        esp32_test_select_layout.addWidget(self.esp32_test_combo)
        esp32_test_select_layout.addWidget(self.esp32_run_selected_button)

        esp32_test_button_row = QHBoxLayout()
        esp32_test_button_row.addWidget(self.esp32_run_all_button)
        esp32_test_button_row.addWidget(self.esp32_invalid_button)
        esp32_test_button_row.addWidget(self.esp32_now_button)
        esp32_test_button_row.addWidget(self.esp32_help_button)

        esp32_send_button_row = QHBoxLayout()
        esp32_send_button_row.addWidget(self.esp32_manual_send_button)
        esp32_send_button_row.addWidget(self.esp32_displayed_send_button)

        esp32_command_layout = QHBoxLayout()
        esp32_command_layout.addWidget(self.esp32_command_edit)
        esp32_command_layout.addWidget(self.esp32_command_button)

        esp32_results_layout = QGridLayout()
        esp32_results_layout.addWidget(QLabel("Test"), 0, 0)
        esp32_results_layout.addWidget(QLabel("Expected"), 0, 1)
        esp32_results_layout.addWidget(QLabel("Actual"), 0, 2)
        esp32_results_layout.addWidget(QLabel("Status"), 0, 3)

        for row, (key, label, _command, expected_reply) in enumerate(ESP32_SPI_TEST_CASES, start=1):
            expected_label = QLabel(expected_reply)
            actual_label = QLabel("not run")
            result_label = QLabel("pending")
            actual_label.setWordWrap(True)
            result_label.setAlignment(Qt.AlignCenter)
            self.test_actual_reply_labels[key] = actual_label
            self.test_result_labels[key] = result_label
            self.apply_test_result_style(result_label, "pending")

            esp32_results_layout.addWidget(QLabel(label), row, 0)
            esp32_results_layout.addWidget(expected_label, row, 1)
            esp32_results_layout.addWidget(actual_label, row, 2)
            esp32_results_layout.addWidget(result_label, row, 3)

        esp32_test_layout.addLayout(esp32_test_select_layout)
        esp32_test_layout.addLayout(esp32_test_button_row)
        esp32_test_layout.addLayout(esp32_send_button_row)
        esp32_test_layout.addLayout(esp32_command_layout)
        esp32_test_layout.addWidget(self.test_suite_summary_label)
        esp32_test_layout.addLayout(esp32_results_layout)
        esp32_test_layout.addWidget(self.clear_test_results_button)
        esp32_test_group.setLayout(esp32_test_layout)

        display_grid = QGridLayout()
        display_grid.addWidget(QLabel("NTP"), 0, 0)
        display_grid.addWidget(QLabel("RTC"), 0, 1)
        display_grid.addWidget(QLabel("UTC"), 0, 2)
        display_grid.addWidget(self.ntp_time_label, 1, 0)
        display_grid.addWidget(self.rtc_time_label, 1, 1)
        display_grid.addWidget(self.utc_time_label, 1, 2)

        main_layout = QVBoxLayout()
        main_layout.addLayout(display_grid)
        main_layout.addLayout(teensy_comm_layout)
        main_layout.addLayout(mode_layout)
        main_layout.addLayout(manual_layout)

        aux_window_button_layout = QHBoxLayout()
        aux_window_button_layout.addWidget(self.show_monitor_window_button)
        aux_window_button_layout.addWidget(self.show_test_window_button)
        main_layout.addLayout(aux_window_button_layout)
        main_layout.addWidget(self.send_button)
        main_layout.addWidget(self.status_label)

        self.setLayout(main_layout)
        self.create_monitor_window(monitor_comm_layout)
        self.create_test_window(esp32_test_group)

        self.clock_timer = QTimer(self)
        self.clock_timer.timeout.connect(self.update_clock)
        self.clock_timer.start(200)

        self.refresh_button.clicked.connect(self.refresh_ports)
        self.monitor_refresh_button.clicked.connect(self.refresh_ports)
        self.open_button.clicked.connect(self.open_port)
        self.close_button.clicked.connect(self.close_port)
        self.monitor_open_button.clicked.connect(self.open_monitor_port)
        self.monitor_close_button.clicked.connect(self.close_monitor_port)
        self.clear_monitor_button.clicked.connect(self.monitor_output.clear)
        self.send_button.clicked.connect(self.send_time)
        self.show_monitor_window_button.clicked.connect(self.show_monitor_window)
        self.show_test_window_button.clicked.connect(self.show_test_window)
        self.esp32_run_selected_button.clicked.connect(self.run_selected_esp32_test)
        self.esp32_run_all_button.clicked.connect(lambda: self.send_monitor_command("test", "Started ESP32 SPI test suite."))
        self.esp32_invalid_button.clicked.connect(lambda: self.send_monitor_command("invalid", "Sent invalid SPI frame from ESP32."))
        self.esp32_now_button.clicked.connect(lambda: self.send_monitor_command("now", "Requested one current ESP32 time transfer."))
        self.esp32_help_button.clicked.connect(lambda: self.send_monitor_command("help", "Requested ESP32 command help."))
        self.esp32_manual_send_button.clicked.connect(self.send_manual_time_to_esp32)
        self.esp32_displayed_send_button.clicked.connect(self.send_displayed_time_to_esp32)
        self.esp32_command_button.clicked.connect(self.send_custom_esp32_command)
        self.clear_test_results_button.clicked.connect(self.reset_test_results)

        self.radio_system_time.toggled.connect(self.update_time_mode)
        self.radio_manual_time.toggled.connect(self.update_time_mode)
        self.use_displayed_time_button.clicked.connect(self.copy_displayed_time_to_manual)

        self.refresh_ports()
        self.sync_ntp_time(force=True)
        self.update_clock()
        self.update_time_mode()
        self.update_monitor_command_controls()
        QTimer.singleShot(0, self.show_monitor_window)
        QTimer.singleShot(0, self.show_test_window)

    def create_monitor_window(self, monitor_comm_layout):
        """Erzeugt das separate ESP32-Monitorfenster.

        Das Fenster verwendet dieselben Widgets, die auch vom Hauptfenster
        verwaltet werden. Dadurch bleibt der Monitorzustand zentral in dieser
        Klasse und muss nicht zwischen mehreren Controllern synchronisiert
        werden.
        """
        self.monitor_window = QWidget()
        self.monitor_window.setAttribute(Qt.WA_DeleteOnClose, False)
        self.monitor_window.setWindowTitle("ESP32 Monitor")
        self.monitor_window.resize(900, 760)

        monitor_summary_grid = QGridLayout()
        monitor_summary_grid.addWidget(QLabel("TIME_TX"), 0, 0)
        monitor_summary_grid.addWidget(QLabel("TIME_INFO"), 0, 1)
        monitor_summary_grid.addWidget(self.time_tx_label, 1, 0)
        monitor_summary_grid.addWidget(self.time_info_label, 1, 1)
        monitor_summary_grid.setColumnStretch(0, 1)
        monitor_summary_grid.setColumnStretch(1, 1)

        monitor_layout = QVBoxLayout()
        monitor_layout.addLayout(monitor_comm_layout)
        monitor_layout.addLayout(monitor_summary_grid)
        monitor_layout.addWidget(QLabel("ESP32 SPI Status History"))
        monitor_layout.addWidget(self.esp32_spi_status_history)
        monitor_layout.addWidget(QLabel("ESP32 Serial Monitor"))
        monitor_layout.addWidget(self.monitor_output)
        self.monitor_window.setLayout(monitor_layout)

    def create_test_window(self, esp32_test_group):
        """Erzeugt das separate Fenster fuer die SPI-Test-Suite.

        Die Suite-Ansicht ist bewusst ausgelagert, damit laengere Testlaeufe
        beobachtet werden koennen, ohne dass das Hauptfenster mit zu vielen
        Statusfeldern ueberladen wird.
        """
        self.test_window = QWidget()
        self.test_window.setAttribute(Qt.WA_DeleteOnClose, False)
        self.test_window.setWindowTitle("ESP32 SPI Test Suite")
        self.test_window.resize(900, 760)

        test_layout = QVBoxLayout()
        test_layout.addWidget(esp32_test_group)
        self.test_window.setLayout(test_layout)

    def place_aux_window(self, window, preferred_x, preferred_y):
        """Platziert ein Zusatzfenster moeglichst nah an der Hauptansicht.

        Die Zielposition wird auf die verfuegbare Bildschirmflaeche begrenzt.
        Damit bleiben Hilfsfenster auch nach Monitorwechseln oder veraenderten
        Aufloesungen sichtbar.
        """
        if window is None:
            return

        screen = self.screen()
        if screen is None:
            screen = QApplication.primaryScreen()

        if screen is None:
            window.move(preferred_x, preferred_y)
            return

        available = screen.availableGeometry()
        max_x = available.x() + max(0, available.width() - window.width())
        max_y = available.y() + max(0, available.height() - window.height())

        target_x = max(available.x(), min(preferred_x, max_x))
        target_y = max(available.y(), min(preferred_y, max_y))
        window.move(target_x, target_y)

    def position_monitor_window(self):
        """Berechnet und fixiert die Position des Monitorfensters.

        Die Position wird nur einmal automatisch gesetzt. Danach bleibt das
        Fenster an der vom Benutzer gewaehlten Stelle.
        """
        if self.monitor_window is None or self.monitor_window_position_locked:
            return

        self.place_aux_window(self.monitor_window, self.x() + self.width() + 24, self.y())
        self.monitor_window_position_locked = True

    def position_test_window(self):
        """Berechnet und fixiert die Position des Testfensters.

        Das Verhalten entspricht dem Monitorfenster: automatische Erstplatzierung,
        danach keine erneute Zwangsverschiebung mehr.
        """
        if self.test_window is None or self.test_window_position_locked:
            return

        preferred_x = self.x() + self.width() + 48
        preferred_y = self.y() + 40
        self.place_aux_window(self.test_window, preferred_x, preferred_y)
        self.test_window_position_locked = True

    def show_monitor_window(self):
        """Zeigt das Monitorfenster an und holt es nach vorne.

        Vor dem Anzeigen wird bei Bedarf die Startposition bestimmt, damit das
        Fenster auch unmittelbar nach dem Programmstart sinnvoll platziert ist.
        """
        if self.monitor_window is None:
            return

        self.position_monitor_window()
        self.monitor_window.show()
        self.monitor_window.raise_()
        self.monitor_window.activateWindow()

    def show_test_window(self):
        """Zeigt das Testfenster an und holt es nach vorne.

        Das Fenster wird bewusst separat aktiviert, damit Tests und Monitorlog
        parallel sichtbar bleiben.
        """
        if self.test_window is None:
            return

        self.position_test_window()
        self.test_window.show()
        self.test_window.raise_()
        self.test_window.activateWindow()

    def update_clock(self):
        """Fuehrt den zentralen periodischen GUI-Tick aus.

        In fester Reihenfolge werden NTP-Zeit, sichtbare Anzeigen und beide
        seriellen Kanaele aktualisiert. Die feste Reihenfolge erleichtert
        spaetere Fehlersuche, weil Seiteneffekte konsistent bleiben.
        """
        self.sync_ntp_time()
        self.update_ntp_time_display()
        self.poll_serial_port()
        self.poll_monitor_port()

    def sync_ntp_time(self, force=False):
        """Aktualisiert die lokale NTP-Referenzzeit bei Bedarf oder auf Zwang.

        Standardmaessig wird nur nach Ablauf des Poll-Intervalls erneut
        synchronisiert. Bei Fehlern wird die Ausnahme abgefangen und in
        ``self.ntp_sync_error`` abgelegt, damit die GUI weiter reagieren kann.
        """
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
        """Fragt die aktuelle Zeit per UDP-NTP direkt von ``fritz.box`` ab.

        Die Methode erzeugt ein minimales NTP-Request-Paket, wertet den
        Transmit-Timestamp der Antwort aus und liefert daraus eine lokale
        ``datetime``-Instanz. Netzwerk- und Protokollfehler werden als
        ``OSError`` an den Aufrufer gemeldet.
        """
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
        """Leitet die aktuell angenommene NTP-Zeit aus letzter Sync-Zeit ab.

        Zwischen zwei echten NTP-Abfragen wird die Zeit aus dem letzten
        Referenzwert plus Monotonic-Delta fortgeschrieben. So bleibt die
        Anzeige fluessig, ohne den NTP-Server sekundenweise abzufragen.
        """
        if self.last_ntp_local_datetime is None:
            return None

        elapsed_seconds = time.monotonic() - self.last_ntp_sync_monotonic
        return self.last_ntp_local_datetime.timestamp() + elapsed_seconds

    def update_ntp_time_display(self):
        """Aktualisiert die NTP-Anzeige in der Hauptansicht.

        Fehlt eine gueltige NTP-Referenz, wird ein klarer Platzhaltertext
        angezeigt. Andernfalls wird die abgeleitete aktuelle Zeit formatiert
        und in das Hauptfenster geschrieben.
        """
        current_ntp_timestamp = self.get_current_ntp_time()
        if current_ntp_timestamp is None:
            self.ntp_time_label.setText("NTP unavailable")
            return

        current_ntp_time = datetime.fromtimestamp(current_ntp_timestamp)
        self.ntp_time_label.setText(current_ntp_time.strftime("%Y-%m-%d %H:%M:%S"))

    def poll_serial_port(self):
        """Liest neue Daten vom Teensy-Port und zerlegt sie in Zeilen.

        Teilzeilen bleiben in ``self.serial_read_buffer`` gepuffert, bis ein
        Zeilenende eintrifft. Lesefehler fuehren zum kontrollierten Schliessen
        des Ports, damit die GUI nicht mit einem defekten Handle weiterarbeitet.
        """
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

    def poll_monitor_port(self):
        """Liest neue Daten vom ESP32-Monitorport und zerlegt sie in Zeilen.

        Zeilenenden werden auf ``\\n`` normalisiert, damit die weitere
        Verarbeitung unabhaengig vom Betriebssystem bleibt. Im Unterschied zum
        Teensy-Pfad wird UTF-8 mit Ersatzzeichen verwendet, um Diagnoseausgaben
        robuster darzustellen.
        """
        if self.monitor_serial_port is None or not self.monitor_serial_port.is_open:
            return

        try:
            bytes_waiting = self.monitor_serial_port.in_waiting
        except (serial.SerialException, OSError) as e:
            QMessageBox.critical(self, "Serial Error", str(e))
            self.status_label.setText("ESP32 monitor read failed.")
            self.close_monitor_port()
            return

        if bytes_waiting <= 0:
            return

        try:
            incoming_data = self.monitor_serial_port.read(bytes_waiting).decode("utf-8", errors="replace")
        except (serial.SerialException, OSError) as e:
            QMessageBox.critical(self, "Serial Error", str(e))
            self.status_label.setText("ESP32 monitor read failed.")
            self.close_monitor_port()
            return

        self.monitor_read_buffer += incoming_data.replace("\r\n", "\n").replace("\r", "\n")

        while "\n" in self.monitor_read_buffer:
            raw_line, self.monitor_read_buffer = self.monitor_read_buffer.split("\n", 1)
            self.handle_monitor_line(raw_line)

    def handle_serial_line(self, line):
        """Verarbeitet eine einzelne Textzeile vom Teensy.

        Erwartet werden im Wesentlichen RTC-Livezeilen, UTC-Zeilen und die
        Bestaetigung nach erfolgreichem Setzen der RTC. Unbekannte Zeilen werden
        absichtlich ignoriert, damit neue Debug-Ausgaben die GUI nicht stoeren.
        """
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

    def handle_monitor_line(self, line):
        """Verarbeitet eine einzelne Textzeile vom ESP32-Monitor.

        Diese Methode verteilt Rohzeilen auf mehrere Anzeigen:
        TIME_TX, TIME_INFO, SPI-Statushistorie und Suite-Auswertung. Jede
        Zeile wird danach trotzdem noch unveraendert im Monitorlog archiviert.
        """
        if line.startswith("TIME_TX: "):
            self.time_tx_label.setText(line[9:])
            ack_match = re.search(r"\b(ACK=0x[0-9A-Fa-f]{2}\s+.+)$", line)
            if ack_match:
                self.update_esp32_spi_status(ack_match.group(1))
        elif line == "SPI test suite start":
            self.begin_test_suite()
            self.update_esp32_spi_status(line)
        elif line == "SPI test suite end":
            self.finish_test_suite()
            self.update_esp32_spi_status(line)
        elif line.startswith("TIME_INFO: "):
            self.time_info_label.setText(line[11:])
        elif line.startswith("SPI reply: "):
            self.update_esp32_spi_status(line[11:])
        elif line.startswith("SPI "):
            self.update_esp32_spi_status(line)
        elif " | SPI " in line:
            self.update_esp32_spi_status(line.split(" | SPI ", 1)[1])

        self.update_test_result_from_monitor_line(line)
        self.append_monitor_output(line)

    def append_monitor_output(self, line):
        """Haengt eine Zeile an die Monitoransicht an und scrollt nach unten.

        Das Vollprotokoll bleibt damit auch dann nachvollziehbar, wenn ein Teil
        der Informationen bereits strukturiert in anderen Widgets ausgewertet
        wurde.
        """
        self.monitor_output.appendPlainText(line)
        scrollbar = self.monitor_output.verticalScrollBar()
        scrollbar.setValue(scrollbar.maximum())

    def update_esp32_spi_status(self, text):
        """Haengt einen neuen SPI-Status an die Statushistorie an.

        Diese kompakte Historie soll die fuer SPI relevanten Ereignisse isoliert
        sichtbar machen, ohne dass das gesamte Monitorlog gelesen werden muss.
        """
        self.esp32_spi_status_history.appendPlainText(text)
        scrollbar = self.esp32_spi_status_history.verticalScrollBar()
        scrollbar.setValue(scrollbar.maximum())

    def apply_test_result_style(self, label, state):
        """Setzt Text und Farben einer Teststatuszelle entsprechend dem Zustand.

        Die Stildefinition ist hier zentralisiert, damit zukuenftige UI-
        Anpassungen nur an einer Stelle gepflegt werden muessen.
        """
        styles = {
            "pending": ("pending", "#5c6370", "#ffffff"),
            "running": ("running", "#d19a66", "#101010"),
            "pass": ("pass", "#98c379", "#101010"),
            "fail": ("fail", "#e06c75", "#ffffff"),
        }
        text, background, foreground = styles[state]
        label.setText(text)
        label.setStyleSheet(
            f"font-weight: bold; padding: 4px; border: 1px solid #444; background-color: {background}; color: {foreground};"
        )

    def reset_test_results(self):
        """Setzt Suite-Zustand, Statusanzeige und Einzeltestergebnisse zurueck.

        Die Methode wird vor jedem neuen Suite-Start und beim Schliessen des
        Monitorports verwendet, damit keine alten ACK-Ergebnisse in neue Laeufe
        hineinragen.
        """
        self.suite_active = False
        self.suite_seen_tests = set()
        self.test_suite_summary_label.setText("No suite run.")
        self.test_suite_summary_label.setStyleSheet("""
            font-size: 15px;
            font-weight: bold;
            border: 2px solid #444;
            padding: 8px;
            background-color: #1f2430;
            color: #d8dee9;
        """)

        for key in ESP32_SPI_TEST_EXPECTATIONS:
            self.test_actual_reply_labels[key].setText("not run")
            self.apply_test_result_style(self.test_result_labels[key], "pending")

    def begin_test_suite(self):
        """Startet eine neue Suite-Auswertung im GUI.

        Das eigentliche Senden der Tests uebernimmt der ESP32. Die GUI schaltet
        hier nur in den Beobachtungsmodus und initialisiert ihre Resultatanzeige.
        """
        self.reset_test_results()
        self.suite_active = True
        self.test_suite_summary_label.setText("Suite running: 0/9 complete")
        self.test_suite_summary_label.setStyleSheet("""
            font-size: 15px;
            font-weight: bold;
            border: 2px solid #444;
            padding: 8px;
            background-color: #3b4252;
            color: #f7d794;
        """)

    def update_suite_progress(self):
        """Aktualisiert die Fortschrittsanzeige der laufenden Suite.

        Gezaehlt werden nur Testfaelle, fuer die bereits eine passende ACK-Zeile
        gesehen wurde. Dadurch zeigt die GUI den realen Protokollfortschritt.
        """
        if not self.suite_active:
            return

        completed = len(self.suite_seen_tests)
        passed = sum(1 for key in self.suite_seen_tests if self.test_result_labels[key].text() == "pass")
        total = len(ESP32_SPI_TEST_EXPECTATIONS)
        self.test_suite_summary_label.setText(f"Suite running: {completed}/{total} complete, {passed} passed")

    def finish_test_suite(self):
        """Schliesst die Suite-Auswertung ab und zeigt das Endergebnis an.

        Die Methode unterscheidet zwischen unvollstaendigen Suites und inhaltlich
        fehlgeschlagenen Suites. Das erleichtert die Diagnose von Transport- und
        Parserproblemen.
        """
        total = len(ESP32_SPI_TEST_EXPECTATIONS)
        completed = len(self.suite_seen_tests)
        passed = sum(1 for key in ESP32_SPI_TEST_EXPECTATIONS if self.test_result_labels[key].text() == "pass")
        self.suite_active = False

        if completed < total:
            self.test_suite_summary_label.setText(f"Suite failed: incomplete ({completed}/{total} seen)")
            self.test_suite_summary_label.setStyleSheet("""
                font-size: 15px;
                font-weight: bold;
                border: 2px solid #444;
                padding: 8px;
                background-color: #7f1d1d;
                color: #ffffff;
            """)
            return

        if passed == total:
            self.test_suite_summary_label.setText(f"Suite passed: {passed}/{total} tests matched expected replies")
            self.test_suite_summary_label.setStyleSheet("""
                font-size: 15px;
                font-weight: bold;
                border: 2px solid #444;
                padding: 8px;
                background-color: #1f5f3b;
                color: #ffffff;
            """)
            return

        self.test_suite_summary_label.setText(f"Suite failed: {passed}/{total} tests matched expected replies")
        self.test_suite_summary_label.setStyleSheet("""
            font-size: 15px;
            font-weight: bold;
            border: 2px solid #444;
            padding: 8px;
            background-color: #7f1d1d;
            color: #ffffff;
        """)

    def update_test_result_from_monitor_line(self, line):
        """Ordnet eine ESP32-ACK-Zeile dem passenden Testfall zu.

        Grundlage ist das vom ESP32 ausgegebene Format ``SPI TX [key]: ... |
        ACK=...``. Nur bekannte Testschluessel werden ausgewertet, damit
        Debug-Zeilen die Suite-Statistik nicht verfaelschen.
        """
        match = re.match(r"^SPI TX \[([^\]]+)\]: .* \| ACK=(0x[0-9A-Fa-f]{2}\s+.+)$", line)
        if not match:
            return

        test_key = match.group(1).strip()
        if test_key not in ESP32_SPI_TEST_EXPECTATIONS:
            return

        actual_reply = match.group(2).strip()
        expected_reply = ESP32_SPI_TEST_EXPECTATIONS[test_key]

        self.test_actual_reply_labels[test_key].setText(actual_reply)
        self.suite_seen_tests.add(test_key)

        if actual_reply.lower() == expected_reply.lower():
            self.apply_test_result_style(self.test_result_labels[test_key], "pass")
        else:
            self.apply_test_result_style(self.test_result_labels[test_key], "fail")

        self.update_suite_progress()

    def update_monitor_command_controls(self):
        """Aktiviert oder deaktiviert die ESP32-Bedienelemente je nach Monitorstatus.

        Alle ESP32-Befehle laufen ueber den Monitorport. Deshalb ist dessen
        Status die einzige Quelle fuer die Freigabe der zugehoerigen Controls.
        """
        monitor_open = self.monitor_serial_port is not None and self.monitor_serial_port.is_open
        self.esp32_test_combo.setEnabled(monitor_open)
        self.esp32_run_selected_button.setEnabled(monitor_open)
        self.esp32_run_all_button.setEnabled(monitor_open)
        self.esp32_invalid_button.setEnabled(monitor_open)
        self.esp32_now_button.setEnabled(monitor_open)
        self.esp32_help_button.setEnabled(monitor_open)
        self.esp32_manual_send_button.setEnabled(monitor_open)
        self.esp32_displayed_send_button.setEnabled(monitor_open)
        self.esp32_command_edit.setEnabled(monitor_open)
        self.esp32_command_button.setEnabled(monitor_open)

    def refresh_ports(self):
        """Aktualisiert beide COM-Port-Auswahllisten aus der aktuellen Portliste.

        Bereits gewaehlt Ports werden nach Moeglichkeit wiederhergestellt. Das
        reduziert Bedienaufwand beim haeufigen Trennen, Flashen und Neuverbinden
        der Boards.
        """
        current_device_port = self.port_combo.currentData()
        current_monitor_port = self.monitor_port_combo.currentData()

        self.port_combo.clear()
        self.monitor_port_combo.clear()
        ports = list(serial.tools.list_ports.comports())

        selected_device_index = -1
        selected_monitor_index = -1

        for index, port in enumerate(ports):
            desc = port.description if port.description else "No description"
            text = f"{port.device} - {desc}"
            self.port_combo.addItem(text, port.device)
            self.monitor_port_combo.addItem(text, port.device)

            if port.device == current_device_port:
                selected_device_index = index

            if port.device == current_monitor_port:
                selected_monitor_index = index

        if selected_device_index >= 0:
            self.port_combo.setCurrentIndex(selected_device_index)

        if selected_monitor_index >= 0:
            self.monitor_port_combo.setCurrentIndex(selected_monitor_index)

        if ports:
            self.status_label.setText("COM ports updated.")
        else:
            self.status_label.setText("No COM ports found.")

    def update_time_mode(self):
        """Passt die GUI an den gewaehlten Zeitmodus an.

        Im NTP-Modus bleibt die manuelle Eingabe gesperrt. Im manuellen Modus
        werden Eingabefeld und Hilfsbutton aktiviert, damit Testzeiten bequem
        vorbereitet werden koennen.
        """
        manual_mode = self.radio_manual_time.isChecked()
        self.manual_time_edit.setEnabled(manual_mode)
        self.use_displayed_time_button.setEnabled(manual_mode)

        if manual_mode:
            self.status_label.setText("Manual time mode active.")
        else:
            self.status_label.setText("Fritz!Box NTP time mode active.")

    def copy_displayed_time_to_manual(self):
        """Uebernimmt die aktuell angezeigte NTP-Zeit in das manuelle Eingabefeld.

        Das ist als Komfortfunktion fuer Testfaelle gedacht, bei denen von der
        aktuellen Zeit ausgegangen und danach nur gezielt angepasst wird.
        """
        current_ntp_timestamp = self.get_current_ntp_time()
        if current_ntp_timestamp is None:
            self.manual_time_edit.clear()
            return

        current_ntp_time = datetime.fromtimestamp(current_ntp_timestamp)
        self.manual_time_edit.setText(current_ntp_time.strftime("%Y-%m-%d %H:%M:%S"))

    def send_monitor_command(self, command, success_message=None):
        """Sendet einen Textbefehl an den ESP32-Monitorport.

        Der Befehl wird immer mit Zeilenende abgeschlossen und gleichzeitig mit
        ``>>>`` in das Monitorlog gespiegelt. Schreibfehler fuehren zum
        kontrollierten Schliessen des Ports, um Folgefehler zu vermeiden.
        """
        if self.monitor_serial_port is None or not self.monitor_serial_port.is_open:
            QMessageBox.critical(self, "Error", "ESP32 monitor port is not open.")
            return False

        try:
            self.monitor_serial_port.write((command.strip() + "\n").encode("ascii"))
            self.append_monitor_output(f">>> {command.strip()}")
            self.status_label.setText(success_message or f"Sent ESP32 command: {command.strip()}")
            return True
        except serial.SerialException as e:
            QMessageBox.critical(self, "Serial Error", str(e))
            self.status_label.setText("ESP32 command send failed.")
            self.close_monitor_port()
            return False

    def run_selected_esp32_test(self):
        """Startet den aktuell ausgewaehlten vordefinierten ESP32-Test.

        Anzeige-Text und eigentlicher Kommando-String bleiben getrennt, damit
        die GUI menschenlesbar bleibt und trotzdem die exakten Befehle sendet.
        """
        command = self.esp32_test_combo.currentData()
        label = self.esp32_test_combo.currentText()
        if command:
            self.send_monitor_command(command, f"Started ESP32 test: {label}")

    def send_manual_time_to_esp32(self):
        """Validiert die manuelle Eingabe und sendet sie als ESP32-Befehl.

        Die Eingabe wird vorab streng geparst, damit offensichtliche Formatfehler
        direkt in der GUI gemeldet werden und nicht erst im ESP32 landen.
        """
        manual_text = self.manual_time_edit.text().strip()
        if not manual_text:
            QMessageBox.critical(self, "Error", "No manual time entered.")
            return

        try:
            parsed = datetime.strptime(manual_text, "%Y-%m-%d %H:%M:%S")
        except ValueError:
            QMessageBox.critical(self, "Error", "Invalid manual format. Use YYYY-MM-DD HH:MM:SS")
            return

        self.send_monitor_command(
            f"send {parsed.strftime('%Y-%m-%d %H:%M:%S')}",
            "Sent manual timestamp to ESP32.",
        )

    def send_displayed_time_to_esp32(self):
        """Sendet die aktuell aus NTP abgeleitete Zeit einmalig an den ESP32.

        Dieser Pfad ist fuer manuelle Soforttests gedacht und unabhaengig vom
        automatischen Minuten-Transfer des ESP32.
        """
        current_ntp_timestamp = self.get_current_ntp_time()
        if current_ntp_timestamp is None:
            QMessageBox.critical(self, "Error", "No displayed NTP time available.")
            return

        current_ntp_time = datetime.fromtimestamp(current_ntp_timestamp)
        self.send_monitor_command(
            f"send {current_ntp_time.strftime('%Y-%m-%d %H:%M:%S')}",
            "Sent displayed NTP timestamp to ESP32.",
        )

    def send_custom_esp32_command(self):
        """Sendet den frei eingegebenen ESP32-Befehl aus dem Textfeld.

        Nach erfolgreichem Senden wird das Eingabefeld geleert, damit alte
        Kommandos nicht versehentlich erneut abgesetzt werden.
        """
        command = self.esp32_command_edit.text().strip()
        if not command:
            QMessageBox.critical(self, "Error", "No ESP32 command entered.")
            return

        if self.send_monitor_command(command):
            self.esp32_command_edit.clear()

    def open_port(self):
        """Oeffnet den ausgewaehlten Teensy-Port fuer Senden und Empfangen.

        Bei Erfolg werden die zugehoerigen GUI-Elemente in einen konsistenten
        Betriebszustand gebracht und konkurrierende Portumschaltungen gesperrt.
        """
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
            self.monitor_refresh_button.setEnabled(False)

        except serial.SerialException as e:
            self.serial_port = None
            QMessageBox.critical(self, "Serial Error", str(e))
            self.status_label.setText("Failed to open port.")

    def close_port(self):
        """Schliesst den Teensy-Port und setzt die zugehoerige GUI zurueck.

        Die Methode ist absichtlich tolerant gegenueber bereits geschlossenen
        oder fehlerhaften Serial-Objekten, damit sie auch aus Fehlerpfaden
        sicher aufgerufen werden kann.
        """
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
        if self.monitor_serial_port is None or not self.monitor_serial_port.is_open:
            self.refresh_button.setEnabled(True)
            self.monitor_refresh_button.setEnabled(True)

    def open_monitor_port(self):
        """Oeffnet den ausgewaehlten ESP32-Monitorport und zeigt die Zusatzfenster an.

        Nach erfolgreichem Oeffnen werden Monitor- und Testfenster sichtbar
        gemacht und alle monitorabhaengigen Bedienelemente freigeschaltet.
        """
        if self.monitor_serial_port is not None and self.monitor_serial_port.is_open:
            self.status_label.setText("ESP32 monitor already open.")
            return

        if self.monitor_port_combo.count() == 0:
            QMessageBox.critical(self, "Error", "No monitor COM port available.")
            return

        port_name = self.monitor_port_combo.currentData()

        try:
            baudrate = int(self.monitor_baud_edit.text().strip())
        except ValueError:
            QMessageBox.critical(self, "Error", "Invalid monitor baudrate.")
            return

        try:
            self.monitor_serial_port = serial.Serial(port_name, baudrate, timeout=1)
            self.monitor_read_buffer = ""
            self.status_label.setText(f"ESP32 monitor opened: {port_name} @ {baudrate} baud")
            self.show_monitor_window()
            self.show_test_window()

            self.monitor_open_button.setEnabled(False)
            self.monitor_close_button.setEnabled(True)
            self.monitor_port_combo.setEnabled(False)
            self.monitor_baud_edit.setEnabled(False)
            self.refresh_button.setEnabled(False)
            self.monitor_refresh_button.setEnabled(False)
            self.update_monitor_command_controls()

        except serial.SerialException as e:
            self.monitor_serial_port = None
            QMessageBox.critical(self, "Serial Error", str(e))
            self.status_label.setText("Failed to open ESP32 monitor port.")
            self.update_monitor_command_controls()

    def close_monitor_port(self):
        """Schliesst den ESP32-Monitorport und setzt den Monitorzustand zurueck.

        Neben dem Port werden auch die davon gespeisten Anzeigen und
        Suite-Zustaende zurueckgesetzt, damit alte ESP32-Diagnosen nach einem
        Neuverbinden nicht als aktuell erscheinen.
        """
        if self.monitor_serial_port is not None:
            try:
                if self.monitor_serial_port.is_open:
                    self.monitor_serial_port.close()
            except Exception:
                pass

        self.monitor_serial_port = None
        self.monitor_read_buffer = ""
        self.time_tx_label.setText("No TIME_TX data")
        self.time_info_label.setText("No TIME_INFO data")
        self.esp32_spi_status_history.clear()
        self.reset_test_results()
        self.monitor_open_button.setEnabled(True)
        self.monitor_close_button.setEnabled(False)
        self.monitor_port_combo.setEnabled(True)
        self.monitor_baud_edit.setEnabled(True)
        self.update_monitor_command_controls()

        if self.serial_port is None or not self.serial_port.is_open:
            self.refresh_button.setEnabled(True)
            self.monitor_refresh_button.setEnabled(True)

    def get_time_to_send(self):
        """Liefert die aktuell zum Teensy zu sendende Zeit als formatierte Zeichenkette.

        Im NTP-Modus stammt die Zeit aus der fortgeschriebenen NTP-Referenz.
        Im manuellen Modus wird die Eingabe geparst und normiert. Fehler werden
        als ``ValueError`` an den Sendepfad gemeldet.
        """
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
        """Sendet die ausgewaehlte Zeit per USB an den Teensy.

        Die Methode kombiniert Quellauswahl, Formatpruefung und den eigentlichen
        seriellen Schreibvorgang. Gesendet wird mit ``CRLF``, weil die Firmware
        Eingaben zeilenbasiert verarbeitet.
        """
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
        """Schliesst offene Ports und Zusatzfenster beim Beenden der Anwendung.

        Das explizite Aufraeumen vermeidet haengende serielle Verbindungen und
        erleichtert einen unmittelbaren Neustart nach Test- oder Flashzyklen.
        """
        self.close_port()
        self.close_monitor_port()
        if self.monitor_window is not None:
            self.monitor_window.close()
        if self.test_window is not None:
            self.test_window.close()
        event.accept()


if __name__ == "__main__":
    app = QApplication(sys.argv)
    window = SerialClockSender()
    window.show()
    sys.exit(app.exec())
