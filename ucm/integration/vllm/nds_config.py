import os
import re
import tempfile

from ucm.logger import init_logger

logger = init_logger(__name__)

_START_CORE_PATTERN = re.compile(r"^(\s*start_core\s*=\s*)(\d+)(.*)$")


def materialize_per_rank_nds_config(base_path: str, core_offset: int) -> str:
    """Write a per-process NDS config with ``start_core`` shifted.

    The derived file stays beside the base config so the NDS wrapper config in
    that directory remains discoverable. Existing derived files are always
    replaced atomically: the base config is the source of truth, and readers
    must never observe a partially written file.
    """
    if core_offset == 0:
        return base_path
    if core_offset < 0:
        raise ValueError(f"NDS core offset must be non-negative, got {core_offset}")

    with open(base_path, "r", encoding="utf-8") as f:
        lines = f.readlines()

    base_core = None
    for index, line in enumerate(lines):
        match = _START_CORE_PATTERN.match(line.rstrip("\n"))
        if match:
            base_core = int(match.group(2))
            lines[index] = (
                f"{match.group(1)}{base_core + core_offset}{match.group(3)}\n"
            )
            break
    if base_core is None:
        raise ValueError(
            f"start_core not found in {base_path}; "
            f"cannot apply NDS core offset {core_offset}"
        )

    new_core = base_core + core_offset
    root, ext = os.path.splitext(base_path)
    new_path = f"{root}.core{new_core}{ext}"
    fd, tmp_path = tempfile.mkstemp(
        prefix=f".{os.path.basename(new_path)}.",
        dir=os.path.dirname(new_path) or ".",
    )
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write("".join(lines))
        os.replace(tmp_path, new_path)
    except BaseException:
        try:
            os.unlink(tmp_path)
        except OSError:
            pass
        raise

    logger.info(
        "Wrote per-rank NDS config %s (start_core %d -> %d, offset %d)",
        new_path,
        base_core,
        new_core,
        core_offset,
    )
    return new_path
