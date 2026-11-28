import importlib.util
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from types import ModuleType, SimpleNamespace

import pytest

REPO_ROOT = Path(__file__).resolve().parents[1]
ucm_module = ModuleType("ucm")
ucm_module.__path__ = [str(REPO_ROOT / "ucm")]
sys.modules.setdefault("ucm", ucm_module)
logger_module = ModuleType("ucm.logger")
logger_module.init_logger = lambda name: SimpleNamespace(info=lambda *args: None)
sys.modules.setdefault("ucm.logger", logger_module)
spec = importlib.util.spec_from_file_location(
    "nds_config_under_test", REPO_ROOT / "ucm/integration/vllm/nds_config.py"
)
nds_config = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nds_config)
materialize_per_rank_nds_config = nds_config.materialize_per_rank_nds_config


def test_zero_offset_preserves_base_config(tmp_path):
    base = tmp_path / "nds_config.conf"
    original = "start_core = 2\ncore_num = 1\n"
    base.write_text(original, encoding="utf-8")

    assert materialize_per_rank_nds_config(str(base), 0) == str(base)
    assert base.read_text(encoding="utf-8") == original
    assert not (tmp_path / "nds_config.core2.conf").exists()


def test_materializes_sibling_config_and_preserves_base(tmp_path):
    base = tmp_path / "nds_config.conf"
    original = "# reserved cores\nstart_core = 2  # aux start\ncore_num = 1\n"
    base.write_text(original, encoding="utf-8")
    (tmp_path / "nds_wrapper.conf").write_text("value_size_kb = 1024\n")

    derived = materialize_per_rank_nds_config(str(base), 6)

    assert derived == str(tmp_path / "nds_config.core8.conf")
    assert base.read_text(encoding="utf-8") == original
    assert (tmp_path / "nds_config.core8.conf").read_text(encoding="utf-8") == (
        "# reserved cores\nstart_core = 8  # aux start\ncore_num = 1\n"
    )
    assert (tmp_path / "nds_wrapper.conf").is_file()


def test_derived_config_is_overwritten_when_base_changes(tmp_path):
    base = tmp_path / "nds_config.conf"
    base.write_text("start_core = 2\ncore_num = 1\n", encoding="utf-8")
    derived = materialize_per_rank_nds_config(str(base), 2)
    assert "start_core = 4" in (tmp_path / "nds_config.core4.conf").read_text()

    base.write_text("start_core = 10\ncore_num = 1\n", encoding="utf-8")
    materialize_per_rank_nds_config(str(base), 2)

    assert "start_core = 12" in (tmp_path / "nds_config.core12.conf").read_text()
    assert base.read_text(encoding="utf-8").startswith("start_core = 10")


def test_missing_start_core_fails_loudly(tmp_path):
    base = tmp_path / "nds_config.conf"
    base.write_text("core_num = 1\n", encoding="utf-8")

    with pytest.raises(ValueError, match="start_core not found"):
        materialize_per_rank_nds_config(str(base), 2)


def test_negative_offset_is_rejected(tmp_path):
    with pytest.raises(ValueError, match="non-negative"):
        materialize_per_rank_nds_config("unused.conf", -1)


def test_concurrent_writers_never_leave_torn_config(tmp_path):
    base = tmp_path / "nds_config.conf"
    base.write_text("start_core = 2\ncore_num = 1\n", encoding="utf-8")

    with ThreadPoolExecutor(max_workers=4) as executor:
        paths = list(
            executor.map(lambda _: materialize_per_rank_nds_config(str(base), 6), range(8))
        )

    assert set(paths) == {str(tmp_path / "nds_config.core8.conf")}
    assert "start_core = 8" in (tmp_path / "nds_config.core8.conf").read_text()
    assert not list(tmp_path.glob(".nds_config.core8.conf.*"))
