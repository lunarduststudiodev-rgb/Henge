# Makes tools/henge-tools.DS_Store, the Finder view of the Henge Tools disk, with the ds_store package
# (pip install ds_store): python3 tools/henge-tools-dsstore.py tools/henge-tools.DS_Store. Run it again only to change the view.
import sys
from ds_store import DSStore
out = sys.argv[1]
win_w, win_h = 520, 320
left, top = 200, 160
with DSStore.open(out, 'w+') as d:
    d['.']['bwsp'] = {
        'ShowStatusBar': False, 'ShowToolbar': False, 'ShowTabView': False, 'ShowSidebar': False,
        'ShowPathbar': False, 'ContainerShowSidebar': False,
        'WindowBounds': '{{%d, %d}, {%d, %d}}' % (left, top, win_w, win_h),
        'PreviewPaneVisibility': False,
    }
    d['.']['icvp'] = {
        'viewOptionsVersion': 1, 'backgroundType': 0, 'backgroundColorRed': 1.0, 'backgroundColorGreen': 1.0,
        'backgroundColorBlue': 1.0, 'gridOffsetX': 0.0, 'gridOffsetY': 0.0, 'gridSpacing': 100.0,
        'arrangeBy': 'none', 'showIconPreview': True, 'showItemInfo': False, 'labelOnBottom': True,
        'textSize': 13.0, 'iconSize': 128.0, 'scrollPositionX': 0.0, 'scrollPositionY': 0.0,
    }
    d['.']['vSrn'] = ('long', 1)
    d['Install Henge Tools.app']['Iloc'] = (win_w // 2, win_h // 2 - 20)
