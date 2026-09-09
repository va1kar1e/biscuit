#!/usr/bin/env python3
"""Convert one or more manga PDFs into Xteink X4-sized image quadrants.

Each PDF page is rendered, split into four quadrants, and written in reading
order. Output images are always exactly 480x800 pixels by default. One output
directory is created per PDF using the PDF filename without its extension.

Examples:
  python pdf_manga_to_x4.py comic.pdf
  python pdf_manga_to_x4.py volume1.pdf volume2.pdf -o X4-Manga
  python pdf_manga_to_x4.py PDFs/ --recursive --order manga
  python pdf_manga_to_x4.py comic.pdf --order ltr --format png --tone mono
"""

from __future__ import annotations

import argparse
import glob
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Iterable

from PIL import Image, ImageChops, ImageOps


QUADRANT_ORDERS = {
    # Japanese manga: top-right, top-left, bottom-right, bottom-left.
    "manga": ("tr", "tl", "br", "bl"),
    # Western/Thai left-to-right: top-left, top-right, bottom-left, bottom-right.
    "ltr": ("tl", "tr", "bl", "br"),
}

COMMON_TOOL_DIRS = (Path("/opt/homebrew/bin"), Path("/usr/local/bin"))


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed < 1:
        raise argparse.ArgumentTypeError("must be at least 1")
    return parsed


def percentage(value: str) -> float:
    parsed = float(value)
    if not 0.0 <= parsed <= 20.0:
        raise argparse.ArgumentTypeError("must be between 0 and 20")
    return parsed


def find_tool(name: str, override: str | None) -> str:
    if override:
        candidate = Path(override).expanduser()
        if not candidate.is_file():
            raise FileNotFoundError(f"Tool not found: {candidate}")
        return str(candidate)

    found = shutil.which(name)
    if found:
        return found

    for directory in COMMON_TOOL_DIRS:
        candidate = directory / name
        if candidate.is_file():
            return str(candidate)

    raise FileNotFoundError(
        f"'{name}' was not found. Install Poppler with 'brew install poppler', "
        f"or pass --{name.replace('pdftoppm', 'pdftoppm').replace('pdfinfo', 'pdfinfo')}."
    )


def expand_inputs(values: Iterable[str], recursive: bool) -> list[Path]:
    pdfs: list[Path] = []
    for raw in values:
        expanded = Path(raw).expanduser()
        if expanded.is_file():
            if expanded.suffix.lower() != ".pdf":
                raise ValueError(f"Not a PDF: {expanded}")
            pdfs.append(expanded.resolve())
            continue

        if expanded.is_dir():
            iterator = expanded.rglob("*") if recursive else expanded.glob("*")
            pdfs.extend(path.resolve() for path in iterator if path.is_file() and path.suffix.lower() == ".pdf")
            continue

        # Also accept quoted glob patterns, e.g. 'Manga/*.pdf'.
        matches = [Path(match) for match in glob.glob(raw, recursive=recursive)]
        matched_pdfs = [path.resolve() for path in matches if path.is_file() and path.suffix.lower() == ".pdf"]
        if not matched_pdfs:
            raise FileNotFoundError(f"Input not found or contains no PDF files: {raw}")
        pdfs.extend(matched_pdfs)

    unique: list[Path] = []
    seen: set[Path] = set()
    for pdf in sorted(pdfs, key=lambda item: str(item).casefold()):
        if pdf not in seen:
            seen.add(pdf)
            unique.append(pdf)
    if not unique:
        raise ValueError("No PDF files were found")
    return unique


def pdf_page_count(pdfinfo: str, pdf: Path) -> int:
    result = subprocess.run(
        [pdfinfo, str(pdf)],
        check=True,
        capture_output=True,
        text=True,
        errors="replace",
    )
    match = re.search(r"^Pages:\s+(\d+)\s*$", result.stdout, flags=re.MULTILINE)
    if not match:
        raise RuntimeError(f"Could not determine page count for {pdf}")
    return int(match.group(1))


def render_pdf_page(pdftoppm: str, pdf: Path, page: int, dpi: int, temp_dir: Path) -> Image.Image:
    prefix = temp_dir / f"page-{page:06d}"
    subprocess.run(
        [
            pdftoppm,
            "-f",
            str(page),
            "-l",
            str(page),
            "-singlefile",
            "-r",
            str(dpi),
            "-gray",
            "-png",
            str(pdf),
            str(prefix),
        ],
        check=True,
        capture_output=True,
    )
    rendered = prefix.with_suffix(".png")
    if not rendered.is_file():
        raise RuntimeError(f"Poppler did not produce an image for page {page} of {pdf}")
    with Image.open(rendered) as source:
        image = source.convert("L")
        image.load()
    rendered.unlink()
    return image


def trim_white_margin(image: Image.Image, threshold: int = 248) -> Image.Image:
    white = Image.new("L", image.size, 255)
    difference = ImageChops.difference(image, white)
    mask = difference.point(lambda pixel: 255 if pixel > 255 - threshold else 0)
    box = mask.getbbox()
    return image.crop(box) if box else image


def split_quadrants(image: Image.Image, overlap_percent: float) -> dict[str, Image.Image]:
    width, height = image.size
    middle_x = width // 2
    middle_y = height // 2
    overlap_x = round(width * overlap_percent / 100.0)
    overlap_y = round(height * overlap_percent / 100.0)

    left_end = min(width, middle_x + overlap_x)
    right_start = max(0, middle_x - overlap_x)
    top_end = min(height, middle_y + overlap_y)
    bottom_start = max(0, middle_y - overlap_y)

    return {
        "tl": image.crop((0, 0, left_end, top_end)),
        "tr": image.crop((right_start, 0, width, top_end)),
        "bl": image.crop((0, bottom_start, left_end, height)),
        "br": image.crop((right_start, bottom_start, width, height)),
    }


def fit_to_screen(image: Image.Image, size: tuple[int, int], fit: str) -> Image.Image:
    if fit == "stretch":
        return image.resize(size, Image.Resampling.LANCZOS)
    if fit == "cover":
        return ImageOps.fit(image, size, Image.Resampling.LANCZOS, centering=(0.5, 0.5))

    resized = ImageOps.contain(image, size, Image.Resampling.LANCZOS)
    canvas = Image.new("L", size, 255)
    position = ((size[0] - resized.width) // 2, (size[1] - resized.height) // 2)
    canvas.paste(resized, position)
    return canvas


def save_image(image: Image.Image, destination: Path, image_format: str, tone: str, quality: int) -> None:
    if tone == "mono":
        output = image.convert("1", dither=Image.Dither.FLOYDSTEINBERG)
    else:
        output = image.convert("L")

    if image_format == "jpg":
        # JPEG does not support Pillow's 1-bit mode.
        output.convert("L").save(destination, "JPEG", quality=quality, optimize=True)
    elif image_format == "png":
        output.save(destination, "PNG", optimize=True)
    else:
        output.save(destination, "BMP")


def convert_pdf(
    pdf: Path,
    output_root: Path,
    pdftoppm: str,
    pdfinfo: str,
    args: argparse.Namespace,
) -> tuple[Path, int]:
    total_pages = pdf_page_count(pdfinfo, pdf)
    first_page = args.first_page
    last_page = min(args.last_page or total_pages, total_pages)
    if first_page > total_pages:
        raise ValueError(f"{pdf.name} has only {total_pages} page(s); --first-page is {first_page}")
    if last_page < first_page:
        raise ValueError("--last-page must be greater than or equal to --first-page")

    destination = output_root / pdf.stem
    if destination.exists():
        raise FileExistsError(
            f"Output folder already exists: {destination}\n"
            "Move or rename it before running again so old and new pages cannot be mixed."
        )

    page_numbers = range(first_page, last_page + 1)
    image_count = len(page_numbers) * 4
    sequence_digits = max(6, len(str(image_count)))
    temp_output = Path(tempfile.mkdtemp(prefix=f".{pdf.stem}.processing-", dir=output_root))
    extension = "jpg" if args.format == "jpg" else args.format

    try:
        with tempfile.TemporaryDirectory(prefix="pdf-manga-render-") as render_dir_name:
            render_dir = Path(render_dir_name)
            sequence = 0
            for page in page_numbers:
                print(f"  [{page - first_page + 1}/{len(page_numbers)}] Rendering PDF page {page}...", flush=True)
                rendered = render_pdf_page(pdftoppm, pdf, page, args.dpi, render_dir)
                if args.trim:
                    rendered = trim_white_margin(rendered)
                if args.autocontrast:
                    rendered = ImageOps.autocontrast(rendered, cutoff=1)

                quadrants = split_quadrants(rendered, args.overlap_percent)
                for quadrant_name in QUADRANT_ORDERS[args.order]:
                    sequence += 1
                    screen_image = fit_to_screen(quadrants[quadrant_name], (args.width, args.height), args.fit)
                    filename = (
                        f"{sequence:0{sequence_digits}d}_p{page:05d}_{quadrant_name}.{extension}"
                    )
                    save_image(screen_image, temp_output / filename, args.format, args.tone, args.jpeg_quality)

        temp_output.rename(destination)
        return destination, image_count
    except Exception:
        shutil.rmtree(temp_output, ignore_errors=True)
        raise


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Split manga PDF pages into four ordered 480x800 images for Xteink X4.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("inputs", nargs="+", help="PDF file(s), directory/directories, or quoted glob pattern(s)")
    parser.add_argument("-o", "--output-root", type=Path, default=Path.cwd(), help="Parent directory for output folders")
    parser.add_argument("--recursive", action="store_true", help="Search PDF input directories recursively")
    parser.add_argument("--order", choices=tuple(QUADRANT_ORDERS), default="manga", help="Quadrant reading order")
    parser.add_argument("--width", type=positive_int, default=480, help="Output image width")
    parser.add_argument("--height", type=positive_int, default=800, help="Output image height")
    parser.add_argument("--dpi", type=positive_int, default=200, help="PDF render resolution before splitting")
    parser.add_argument("--format", choices=("bmp", "png", "jpg"), default="bmp", help="Output image format")
    parser.add_argument("--tone", choices=("gray", "mono"), default="gray", help="Grayscale or dithered 1-bit output")
    parser.add_argument("--fit", choices=("contain", "cover", "stretch"), default="contain", help="How each crop fills the screen")
    parser.add_argument("--overlap-percent", type=percentage, default=2.0, help="Context overlap at each split edge")
    parser.add_argument("--trim", action="store_true", help="Remove near-white outer page margins before splitting")
    parser.add_argument("--no-autocontrast", dest="autocontrast", action="store_false", help="Keep original contrast")
    parser.set_defaults(autocontrast=True)
    parser.add_argument("--first-page", type=positive_int, default=1, help="First PDF page to process")
    parser.add_argument("--last-page", type=positive_int, help="Last PDF page to process (inclusive)")
    parser.add_argument("--jpeg-quality", type=int, choices=range(1, 101), default=92, metavar="1-100")
    parser.add_argument("--pdftoppm", help="Path to pdftoppm when it is not on PATH")
    parser.add_argument("--pdfinfo", help="Path to pdfinfo when it is not on PATH")
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    try:
        pdftoppm = find_tool("pdftoppm", args.pdftoppm)
        pdfinfo = find_tool("pdfinfo", args.pdfinfo)
        pdfs = expand_inputs(args.inputs, args.recursive)
        output_root = args.output_root.expanduser().resolve()
        output_root.mkdir(parents=True, exist_ok=True)

        duplicate_stems = {pdf.stem for pdf in pdfs if sum(other.stem == pdf.stem for other in pdfs) > 1}
        if duplicate_stems:
            names = ", ".join(sorted(duplicate_stems))
            raise ValueError(f"PDF filename collision (same output folder name): {names}")

        print(f"Found {len(pdfs)} PDF file(s). Output: {output_root}")
        completed: list[tuple[Path, int]] = []
        failures: list[tuple[Path, Exception]] = []
        for index, pdf in enumerate(pdfs, start=1):
            print(f"[{index}/{len(pdfs)}] {pdf}")
            try:
                completed.append(convert_pdf(pdf, output_root, pdftoppm, pdfinfo, args))
            except Exception as exc:  # Continue so one bad PDF does not block the rest of a batch.
                failures.append((pdf, exc))
                print(f"  ERROR: {exc}", file=sys.stderr)

        for folder, count in completed:
            print(f"OK: {folder} ({count} images, {args.width}x{args.height})")
        for pdf, exc in failures:
            print(f"FAILED: {pdf}: {exc}", file=sys.stderr)

        return 1 if failures else 0
    except (FileNotFoundError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
