"""Validate native captures and build the visual-review deliverables."""
from pathlib import Path
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parent
build = root / "build"
output = root.parent / "artifacts"
output.mkdir(exist_ok=True)

PAPER = (198, 198, 198)
INK = (48, 48, 48)
PALETTE = {PAPER, INK, (153, 153, 153)}
PAGE_NAMES = ("OVERVIEW", "CO2", "PARTICLES", "GASES", "CLIMATE")


def load(path: Path) -> Image.Image:
    image = Image.open(path).convert("RGB")
    if image.size != (400, 300):
        raise SystemExit(f"{path.name}: expected 400x300, got {image.size}")
    colors = set(image.getdata())
    if not colors <= PALETTE:
        raise SystemExit(f"{path.name}: unsupported colors {sorted(colors - PALETTE)}")
    return image


pages = [load(build / f"scenario-0-scene-{scene}.bmp") for scene in range(5)]
for scene, page in enumerate(pages):
    page.save(output / f"final-page-{scene}.png")
for scene, image in enumerate(pages[1:], 1):
    if image.getpixel((0, 205)) != INK:
        raise SystemExit(f"{PAGE_NAMES[scene]}: history panel does not start at y=205")
    for point in ((60, 217), (60, 267), (382, 267)):
        if image.getpixel(point) != PAPER:
            raise SystemExit(f"{PAGE_NAMES[scene]}: canonical plot boundary {point} is missing")
    for point in ((260, 10), (387, 10), (260, 38), (387, 38)):
        if image.getpixel(point) != INK:
            raise SystemExit(f"{PAGE_NAMES[scene]}: fixed 128x29 header badge boundary is missing at {point}")


def page_sheet(scale: int) -> Image.Image:
    label_height = 24 * scale
    sheet = Image.new("RGB", (400 * 5 * scale, (300 + 24) * scale), INK)
    pen = ImageDraw.Draw(sheet)
    for column, (name, page) in enumerate(zip(PAGE_NAMES, pages)):
        x = column * 400 * scale
        pen.text((x + 8 * scale, 5 * scale), name, fill=PAPER)
        rendered = page if scale == 1 else page.resize((400 * scale, 300 * scale), Image.Resampling.NEAREST)
        sheet.paste(rendered, (x, label_height))
    return sheet


native = page_sheet(1)
native.save(output / "final-pages-native.png")
native.save(output / "lvgl-review.png")
page_sheet(2).save(output / "final-pages-2x.png")

selection_paths = (
    build / "gases-voc.bmp", build / "gases-nox.bmp",
    build / "climate-temperature.bmp", build / "climate-humidity.bmp",
)
selection_sheet = Image.new("RGB", (800, 600), PAPER)
for index, path in enumerate(selection_paths):
    selection_sheet.paste(load(path), ((index % 2) * 400, (index // 2) * 300))
selection_sheet.save(output / "secondary-history-selections.png")

transition_names = ("Return through status", "Status portal", "CO2 bubble portal", "Airflow sweep", "Ribbon sweep")
for scenario in (0, 2):
    sheet = Image.new("RGB", (400 * 5, 324 * 5), (25, 25, 25))
    pen = ImageDraw.Draw(sheet)
    for destination in range(5):
        paths = sorted(build.glob(f"transition-{scenario}-to-{destination}-frame-*.bmp"))
        if len(paths) < 8:
            raise SystemExit("Run atmos --capture-transitions from desktop/build first")
        frames = [load(path) for path in paths]
        frames[0].save(output / f"lvgl-transition-{scenario}-{destination}.gif",
                       save_all=True, append_images=frames[1:], duration=200, loop=0)
        for column, index in enumerate((0, 2, 3, 5, 7)):
            sheet.paste(frames[index], (column * 400, destination * 324 + 24))
        pen.text((8, destination * 324 + 5), transition_names[destination], fill="white")
    sheet.save(output / f"lvgl-transitions-{scenario}.png")

motion_sources = {
    "co2-bubble-frames.png": sorted(build.glob("co2-motion-*.bmp")),
    "particle-airflow-frames.png": sorted(build.glob("pm-select-1-frame-*.bmp")),
    "gas-ribbon-frames.png": sorted(build.glob("gases-motion-*.bmp"))[::8],
    "climate-animation-frames.png": sorted(build.glob("climate-motion-*.bmp"))[::5],
}
for name, paths in motion_sources.items():
    frames = [load(path) for path in paths[:5]]
    if not frames:
        raise SystemExit(f"No frames found for {name}")
    sheet = Image.new("RGB", (400 * len(frames), 300), PAPER)
    for index, frame in enumerate(frames):
        sheet.paste(frame, (index * 400, 0))
    sheet.save(output / name)

print(f"Validated {len(pages)} pages and wrote current review artifacts to {output}")

# Six requested visual states, each shown at native resolution.
six = Image.new("RGB", (1200, 600), PAPER)
for i, (scenario, scene) in enumerate(((0,0),(2,0),(0,2),(0,1),(0,3),(0,4))):
    six.paste(load(build / f"scenario-{scenario}-scene-{scene}.bmp"), ((i%3)*400,(i//3)*300))
six.save(output / "redesign-after.png")
