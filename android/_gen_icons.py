from pathlib import Path
import re
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
svg_text = (ROOT / "logo-stable.svg").read_text(encoding="utf-8")
fills = {
    "cls-1": "#d8d8d8",
    "cls-2": "#efefef",
    "cls-3": "#262626",
    "cls-4": "#515151",
    "cls-5": "#828282",
    "cls-6": "#a0a0a0",
    "cls-7": "#bcbcbc",
    "cls-8": "#6b6b6b",
    "cls-9": "#cccccc",
    "cls-10": "#ededed",
}
polys = re.findall(r'<polygon class="(cls-\d+)" points="([^"]+)"', svg_text)
if len(polys) != 10:
    raise SystemExit(f"expected 10 polygons, got {len(polys)}")
VB_W, VB_H = 163.21, 144.61
BG = (0, 0, 0, 0)
# Adaptive icon safe zone is the center 66dp of 108. Keep the dice inside
# that with extra margin so OEM squircles do not crop the vertices.
FIT = 0.50


def parse_pts(s):
    n = [float(x) for x in s.split()]
    return list(zip(n[0::2], n[1::2]))


def hex_rgb(h):
    h = h.lstrip("#")
    if len(h) == 3:
        h = "".join(c * 2 for c in h)
    return tuple(int(h[i : i + 2], 16) for i in (0, 2, 4))


def transform(x, y, size):
    scale = (size * FIT) / max(VB_W, VB_H)
    dx = (size - VB_W * scale) / 2
    dy = (size - VB_H * scale) / 2
    return x * scale + dx, y * scale + dy


def render(size, background=BG, fill_override=None):
    over = size * 8
    img = Image.new("RGBA", (over, over), background)
    draw = ImageDraw.Draw(img)
    for cls, pts in polys:
        rgb = hex_rgb(fill_override or fills[cls]) + (255,)
        xy = [transform(x, y, over) for x, y in parse_pts(pts)]
        draw.polygon(xy, fill=rgb)
    return img.resize((size, size), Image.Resampling.LANCZOS)


def round_icon(img):
    size = img.size[0]
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).ellipse((0, 0, size - 1, size - 1), fill=255)
    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    out.paste(img, mask=mask)
    return out


def vector(fill_fn, viewport, fit=FIT):
    s = viewport * fit / max(VB_W, VB_H)
    tx, ty = -VB_W / 2, -VB_H / 2
    paths = []
    for cls, pts in polys:
        nums = parse_pts(pts)
        d = "M" + " L".join(f"{x:.2f},{y:.2f}" for x, y in nums) + " Z"
        paths.append(
            f'                <path android:fillColor="{fill_fn(cls)}" android:pathData="{d}" />'
        )
    inner = "\n".join(paths)
    return f"""<?xml version="1.0" encoding="utf-8"?>
<vector xmlns:android="http://schemas.android.com/apk/res/android"
    android:width="{viewport}dp"
    android:height="{viewport}dp"
    android:viewportWidth="{viewport}"
    android:viewportHeight="{viewport}">
    <group android:translateX="{viewport/2:.0f}" android:translateY="{viewport/2:.0f}">
        <group android:scaleX="{s:.5f}" android:scaleY="{s:.5f}">
            <group android:translateX="{tx:.3f}" android:translateY="{ty:.3f}">
{inner}
            </group>
        </group>
    </group>
</vector>
"""


res = ROOT / "android" / "app" / "src" / "main" / "res"
sizes = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}
for dens, px in sizes.items():
    d = res / f"mipmap-{dens}"
    d.mkdir(parents=True, exist_ok=True)
    square = render(px)
    square.save(d / "ic_launcher.png", "PNG")
    round_icon(square).save(d / "ic_launcher_round.png", "PNG")
    print(dens, px, (d / "ic_launcher.png").stat().st_size)

(res / "drawable" / "ic_launcher_foreground.xml").write_text(
    vector(lambda cls: fills[cls], 108), encoding="utf-8"
)
(res / "drawable" / "ic_launcher_monochrome.xml").write_text(
    vector(lambda _cls: "#FF000000", 108), encoding="utf-8"
)
(res / "drawable" / "ic_notification.xml").write_text(
    vector(lambda _cls: "#FFFFFFFF", 24, fit=0.82), encoding="utf-8"
)

s = 108 * FIT / max(VB_W, VB_H)
tx, ty = -VB_W / 2, -VB_H / 2
lines = [
    '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 108 108" role="img" aria-labelledby="title">',
    '  <title id="title">Dice!Next</title>',
    '  <rect width="108" height="108" fill="none"/>',
    f'  <g transform="translate(54 54) scale({s:.5f}) translate({tx:.3f} {ty:.3f})">',
]
for cls, pts in polys:
    points = " ".join(f"{x:.2f},{y:.2f}" for x, y in parse_pts(pts))
    lines.append(f'    <polygon fill="{fills[cls]}" points="{points}"/>')
lines += ["  </g>", "</svg>", ""]
(ROOT / "android" / "ic_launcher.svg").write_text("\n".join(lines), encoding="utf-8")
print("wrote vectors and svg")
