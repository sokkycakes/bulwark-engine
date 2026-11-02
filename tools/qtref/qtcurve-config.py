# Opens QtCurve's own settings editor (the KStyle config plugin that KDE's
# System Settings used to host) as a standalone window. Save writes
# ~/.config/qtcurve/stylerc. Needs PyQt5, QtCurve's Qt 5 style
# (kde-style-qtcurve-qt5 on Debian/Ubuntu) and a D-Bus session: without one
# the editor shows "Already Open" instead of its pages.
#   python3 qtcurve-config.py            edit the settings
#   python3 qtcurve-config.py out.png    screenshot after 1.5 s and quit (testing)
#   dbus-run-session -- python3 qtcurve-config.py   when no session bus runs
import ctypes, glob, sys
from PyQt5 import sip
from PyQt5.QtCore import QMetaObject, QTimer
from PyQt5.QtWidgets import QApplication, QDialogButtonBox, QVBoxLayout, QWidget

app = QApplication(sys.argv[:1])
paths = glob.glob('/usr/lib*/**/kstyle_qtcurve5_config.so', recursive=True)
if not paths:
    sys.exit('kstyle_qtcurve5_config.so not found: install kde-style-qtcurve-qt5')
lib = ctypes.CDLL(paths[0])
lib.allocate_kstyle_config.restype = ctypes.c_void_p
lib.allocate_kstyle_config.argtypes = [ctypes.c_void_p]

win = QWidget()
win.setWindowTitle('QtCurve settings')
box = QVBoxLayout(win)
holder = QWidget()
box.addWidget(holder)
cfg = sip.wrapinstance(lib.allocate_kstyle_config(sip.unwrapinstance(holder)), QWidget)
QVBoxLayout(holder).addWidget(cfg)
buttons = QDialogButtonBox(QDialogButtonBox.Save | QDialogButtonBox.RestoreDefaults | QDialogButtonBox.Close)
box.addWidget(buttons)
buttons.button(QDialogButtonBox.Save).clicked.connect(lambda: QMetaObject.invokeMethod(cfg, 'save'))
buttons.button(QDialogButtonBox.RestoreDefaults).clicked.connect(lambda: QMetaObject.invokeMethod(cfg, 'defaults'))
buttons.rejected.connect(win.close)
win.resize(1100, 760)
win.show()
if len(sys.argv) > 1:
    QTimer.singleShot(1500, lambda: (win.grab().save(sys.argv[1]), app.quit()))
app.exec_()
