# Renders a fixed-layout widget gallery in a given Qt style (black-box reference).
import sys
from PyQt5.QtWidgets import *
from PyQt5.QtCore import Qt
from PyQt5.QtGui import QFont
style, out = sys.argv[1], sys.argv[2]
app = QApplication(['x', '-platform', 'offscreen'])
app.setStyle(QStyleFactory.create(style))
app.setPalette(app.style().standardPalette())
f = QFont('DejaVu Sans'); f.setPixelSize(12); app.setFont(f)
w = QWidget(); w.resize(640, 400)
def place(widget, x, y, ww, hh): widget.setParent(w); widget.setGeometry(x, y, ww, hh); return widget
def hover(widget): widget.setAttribute(Qt.WA_UnderMouse, True); return widget
# row 1 buttons
place(QPushButton('Button'), 10, 10, 90, 28)
hover(place(QPushButton('Hover'), 110, 10, 90, 28))
b = place(QPushButton('Pressed'), 210, 10, 90, 28); b.setDown(True)
b = place(QPushButton('Default'), 310, 10, 90, 28); b.setDefault(True)
b = place(QPushButton('Disabled'), 410, 10, 90, 28); b.setEnabled(False)
b = place(QPushButton('Checked'), 510, 10, 90, 28); b.setCheckable(True); b.setChecked(True)
# row 2 checks / radios
place(QCheckBox('Check'), 10, 50, 90, 22)
c = place(QCheckBox('Checked'), 110, 50, 90, 22); c.setChecked(True)
hover(place(QCheckBox('Hover'), 210, 50, 90, 22))
c = place(QCheckBox('Disabled'), 310, 50, 90, 22); c.setChecked(True); c.setEnabled(False)
place(QRadioButton('Radio'), 410, 50, 90, 22)
r = place(QRadioButton('On'), 510, 50, 90, 22); r.setChecked(True)
# row 3 text / combo
place(QLineEdit('Text entry'), 10, 90, 140, 26)
e = place(QLineEdit('Focused'), 160, 90, 140, 26); e.setFocus()
cb = place(QComboBox(), 310, 90, 140, 26); cb.addItems(['Combo box', 'Two'])
e = place(QLineEdit('Disabled'), 460, 90, 140, 26); e.setEnabled(False)
# row 4 slider / progress
s = place(QSlider(Qt.Horizontal), 10, 130, 200, 22); s.setRange(0, 100); s.setValue(40)
p = place(QProgressBar(), 230, 130, 200, 22); p.setValue(40); p.setTextVisible(False)
p = place(QProgressBar(), 450, 130, 170, 22); p.setValue(70)
# row 5 scrollbars / group / tabs
sb = place(QScrollBar(Qt.Vertical), 10, 170, 16, 140); sb.setRange(0, 100); sb.setPageStep(30); sb.setValue(0)
sb = place(QScrollBar(Qt.Horizontal), 40, 170, 180, 16); sb.setRange(0, 100); sb.setPageStep(30); sb.setValue(0)
g = place(QGroupBox('Group box'), 40, 200, 180, 110)
c = QCheckBox('Inside', g); c.setGeometry(10, 25, 100, 22)
t = place(QTabWidget(), 240, 170, 380, 140)
for n in ('First', 'Second', 'Third'): t.addTab(QWidget(), n)
w.show(); app.processEvents(); e.setFocus(); app.processEvents()
w.grab().save(out)
