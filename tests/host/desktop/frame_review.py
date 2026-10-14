"""Optional PNG review copies of host frames; never guest evidence."""
# SPDX-License-Identifier: MIT
import hashlib
import json
import os
from pathlib import Path


def capture(name, pixels, width=None, height=None):
    directory = os.environ.get("CIUKI_FONT_REVIEW")
    if not directory:
        return
    from PIL import Image
    import io
    folder = Path(directory)
    folder.mkdir(parents=True, exist_ok=True)
    if width is None:
        image = Image.open(io.BytesIO(pixels)).convert("RGB")
    else:
        image = Image.frombytes("RGB", (width, height), pixels, "raw", "BGRX")
    image.save(folder / (name + ".png"))
    record = folder / "frames.json"
    hashes = json.loads(record.read_text()) if record.exists() else {}
    hashes[name] = hashlib.sha256(pixels).hexdigest()
    record.write_text(json.dumps(hashes, indent=2, sort_keys=True) + "\n")
