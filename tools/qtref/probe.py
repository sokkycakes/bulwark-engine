import sys
from PIL import Image
im=Image.open(sys.argv[1]).convert('RGB')
def h(c): return '%02x%02x%02x'%c
def col(x,y0,y1): return ' '.join(h(im.getpixel((x,y))) for y in range(y0,y1))
def row(y,x0,x1): return ' '.join(h(im.getpixel((x,y))) for x in range(x0,x1))
probes={
 'bg':row(395,0,3),
 'btn col x=30':col(30,8,40),
 'btn row y=24 left':row(24,8,16),
 'btn top-left corner rows':'\n   '.join(row(y,9,15) for y in range(9,14)),
 'hover col':col(130,8,40),
 'pressed col':col(230,8,40),
 'default col':col(330,8,40),
 'disabled col':col(430,8,40),
 'checked btn col':col(530,8,40),
 'check col x=17':col(17,52,72),
 'check row y=61':row(61,8,26),
 'checked col x=117':col(117,52,72),
 'hover check col':col(217,52,72),
 'radio row y=61':row(61,408,426),
 'radio on row':row(61,508,526),
 'edit col':col(100,88,118),
 'focused edit col':col(250,88,118),
 'focused edit row':row(103,158,164),
 'combo col':col(330,88,118),
 'combo row arrow':row(103,425,452),
 'disabled edit col':col(500,88,118),
 'slider col x=50':col(50,130,152),
 'slider col x=150':col(150,130,152),
 'slider thumb col':col(91,128,154),
 'slider thumb row':row(141,80,104),
 'progress col x=260':col(260,128,154),
 'progress col x=400':col(400,128,154),
 'progress row y=141':row(141,228,236)+' | '+row(141,305,315),
 'vscroll row y=240':row(240,8,28),
 'vscroll col':col(18,168,315),
 'hscroll col x=100':col(100,168,188),
 'hscroll row':row(178,38,222),
 'group col x=100':col(100,198,312),
 'tab col first x=260':col(260,166,200),
 'tab col second x=320':col(320,166,200),
 'tab row y=185':row(185,238,300),
 'pane col x=400':col(400,190,312),
}
for k,v in probes.items(): print(k+':\n   '+v)
