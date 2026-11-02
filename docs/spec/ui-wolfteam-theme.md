# WolfTeam look-alike theme for Panorama

`data/panorama/styles/wt/wolfteam.css` gives Panorama widgets the look of
WolfTeam's (Softnyx, LithTech) lobby, room and option screens, so new menus,
dialogs and HUD panels can be built in that style without copying any of
its screens. Put `class="WolfTeam"` on a container and every widget inside
takes the look. `exec panorama/wt-gallery.cfg` opens the gallery
(`layout/wt/gallery.xml`): every piece once, plus a message box and a
"create room" dialog built only from the parts.

It follows the Qt themes ([ui-qt-themes.md](ui-qt-themes.md)): one CSS file
per look, custom properties at the top, the engine's own widget types
(`Button`, `ToggleButton`, `RadioButton`, `TextEntry`, `DropDown`,
`Slider`, `ProgressBar`, scroll bars) plus a few class-based composites.

## Where the look comes from

WolfTeam's UI is described by text scripts (`WT_Interface/attributes/
interface/*.txt`, UTF-16, `[Start]`/`[End]` blocks: `Type "Button"`,
`Size`, `Position`, `Texture`, `ButtonDefaultRect`/`OverRect`/`ClickRect`/
`SelectRect`/`DisableRect`, `CaptionColor`, `FontSize`, `FontAlign`) and
DTX textures, both inside `wolf.xfs`. Everything below was measured from
them (`wolf.xfs` is a run of zlib blocks with an 8-byte header each; the
last block is the directory). The textures are the game's and never go in
git.

Every button, header, tab and thumb face in WolfTeam is a plain vertical
gradient, identical across its width, so the theme draws them with CSS
gradients whose stops `tools/wtui/measure-wt.py` reads from the textures
(the block between `BEGIN/END measure-wt` in `wolfteam.css`):

| widget | texture | size | states |
| --- | --- | --- | --- |
| button | `All/a_button_*.dtx` | 90x28 | idle 101→2 grey, hover 215→5 (silver), pressed reversed, disabled = idle with #656565 text |
| tab | `Options/op_tab.dtx` | 82x26, 1px apart | black, #323232 top line; hover top line #686868; chosen: 9px #494949 band; text #6b6b6b / #ffcc00 bold |
| glass tab | `Lobby/lb_ch_tab.dtx`, `inven_shop/is_inven_tab.dtx` | 18 tall | hover: lit black glass; chosen: grey glass; white text |
| check box | `All/a_check.dtx` | 18x18 | #0c0c0c, #282828 rim; hover and checked #1a1a1a with the grey check mark (WolfTeam shows the mark on hover too) |
| radio | `MessageBox/mb_popup_bt.dtx` (round, 10px); option pages use the square check | | dark dot, orange when lit or on |
| edit box | `MessageBox/mb_popup_inputback.dtx` | 18–20 tall | #0b0b0b, black rim, white 12px text, no focus ring |
| combo box | `combobox/mr_combobg_01.dtx` + `cb_button_down.dtx` + `cb_popup.dtx` | 18 tall | #0f0f0f, #444 rim, white triangle; list rows 18px #383838, #7c7c7c under the mouse; text 182,200,204 |
| slider | `Options/op_slider_gage*.dtx`, `op_slider_cursor.dtx` | 285x9 | dark line above, grey line below, #d6d6d6 fill, 13x7 grey cursor under the groove; value box `op_slider_numback` 38x18, text 147,146,152 |
| gauge | `inven_shop/is_gage.dtx` | 12 tall | #ffcc00 42px segments, 3px gaps; `lb_userinfo_gage` plain grey 10px |
| scroll bar | `Lobby/a_scroll.dtx` | 13 wide | 97→46 grey thumb, bevelled triangle buttons (grey / light / dark) |
| column header | `Lobby/lb_cate_*.dtx` | 22 tall | grey glass, white bold text; hover: white glass, black text |
| list row | `Lobby/lb_room_bar.dtx` | 21 tall every 25 | black 65%; lit: white 57% |
| item card | `inven_shop/is_inven_weponbar.dtx` | 208x114 | #2b2b2b with a dark top shade, 18,18,18 @70% foot; hover white rim; chosen gold rim on near-black |
| dialog | `MessageBox/mb_popup_bg.dtx` (471x286), `gamemenu/gm_popup_bg`, `makeroom/mr_bg` | | 5,5,5 @95% body, black rim, soft shadow right/below; 24px black-glass title bar, #363636 line, gold (255,192,3) bold 12px title 5px in, 14px close cross top right |
| icon button | `All/a_icon_close.dtx` | 32x32 | white bevelled glass, red when lit |

Text colours come from the scripts' `CaptionColor`s: labels 199,209,217;
values white; titles 255,192,3; map/room names 255,204,102; idle tab text
107,107,107; item buttons 173,174,179 / hover 194,227,255 / disabled
88,88,88.

## Fonts

- Display text (buttons, big numbers) is **Square721 BT Bold** in WolfTeam
  (`wt_interface/fonts/sqr721b.ttf` inside `wolf.xfs`; the HUD digit strips
  were rendered from it). The buttons bake it at 16px (11px cap height, 8px
  down in the 28px face). It is a Bitstream font, so the theme ships
  **Russo One** (SIL OFL, `fonts-wt/`) as the stand-in: same weight and
  squarish Eurostile-like shapes, slightly narrower.
- Text is 12px Gulim/Arial in WolfTeam; the theme ships **Liberation Sans**
  Regular and Bold (SIL OFL, `fonts-wt/`).
- `--wt-display-font` and `--wt-text-font` list the originals first, so
  registering them makes the theme use them:
  `ui_font "Square721 BT" panorama/fonts-wt/wt-private/sqr721b.ttf 700`
  and `ui_font "Gulim" panorama/fonts-wt/wt-private/gulim.ttc`.
  `wt-gallery.cfg` does both (and only warns when the files are missing).

## Original art (private)

`styles/wt/wolfteam-art.css` is the same theme with WolfTeam's own glyph
sprites: add `WolfTeamArt` next to `WolfTeam` and the check mark, scroll
arrows, combo arrow, close cross, slider cursor and radio dot come from
`images/wt/wt-private/*.png` (crops of `a_check`, `a_scroll`,
`cb_button_down`, `a_close`, `op_slider_cursor`, `mb_popup_bt`). That folder
and `fonts-wt/wt-private/` are gitignored (`**/wt-private/`); the pack is
`bulwark-ui/wt-theme/wt-theme-private.tgz` in the project files. Without
it, leave the class off: the shipped SVG redraws in `images/wt/` stand in.

## Building UI with it

```xml
<Panel class="WolfTeam">
  <Panel class="WtWindow" style="width:417px;height:186px;">
    <Panel class="WtTitleBar"><Label class="WtTitle" text="CREATE" /><Button class="WtClose" onactivate="..." /></Panel>
    <Panel class="WtBody">
      <Panel class="WtField" style="x:15px;y:10px;"><Label text="Title" /><TextEntry style="width:168px;" /></Panel>
      <Panel class="WtField" style="x:15px;y:40px;"><Label text="Mode" /><DropDown selected="m1" style="width:159px;"><Label id="m1" text="Deathmatch" /></DropDown></Panel>
      <ToggleButton text="Observe" style="x:15px;y:100px;" />
      <Panel class="WtButtonRow" style="y:124px;"><Button><Label text="OK" /></Button><Button><Label text="CANCEL" /></Button></Panel>
    </Panel>
  </Panel>
</Panel>
```

Classes: `WtWindow` / `WtTitleBar` / `WtTitle` / `WtClose` / `WtBody` /
`WtButtonRow` / `WtField` (dialogs), `WtTabs` + `WtTab` RadioButtons (add
`Glass` to the bar for the short tabs), `WtHeaderRow` + `WtHeader` Buttons
and `WtList` + `WtRow` (`Selected`) for lists, `WtPlainRow`, `WtCard` +
`WtCardBody` + `WtCardFoot` (`Hover`, `Selected`), `WtGroup` + `WtGroupTitle`
+ `WtGroupFrame`, `WtSliderValue`, `WtStepper Minus|Plus`, `WtIconButton`,
`WtTextButton` (text-only button), `RadioButton.Square`, `DropDown.Light`,
`ProgressBar.Plain`, text styles `WtTitle` `WtLabel` `WtValue` `WtHint`
`WtMuted` `WtGold` `WtDisplay` `WtNumber` (`Red`/`Blue`/`Gold`), `WtRule`.

Sizes are WolfTeam's at its 1024x768 scale; give the view
`bwdesignheight="768"` to keep them, or any other height to scale.

## Known differences

- Hover and pressed faces are the measured gradients, but WolfTeam's
  baked button text has a soft embossed glow we only approximate with
  `text-shadow`.
- The check box shows its mark on hover because WolfTeam's "over" sprite
  does; the disabled check box is blank even when checked (its disable
  sprite is), the same as the game.
- The open combo box keeps its header row (Panorama shows the selected
  option there); WolfTeam draws only the list.
- Only the glyphs have an "original art" variant; faces are CSS in both.

## Test recipe

A game-less basedir with an empty game dir and the gallery cfg:
`bulwark -basedir <dir> -game wt +exec wttest.cfg` where `wttest.cfg` is
`exec panorama/wt-gallery.cfg`; `ui_eval 1 WtGallery.SetArt(true)` switches
to the private sprites; `screenshot name`.
