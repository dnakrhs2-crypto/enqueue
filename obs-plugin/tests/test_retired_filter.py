"""Source contract for the legacy scene id; requires no libobs or OBS process."""
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


class RetiredFilterTests(unittest.TestCase):
    def test_legacy_id_is_registered_but_hidden_and_passes_parent_audio(self):
        source = (ROOT / "src/livemix-filter.c").read_text(encoding="utf-8")
        flags = re.search(r"\.output_flags\s*=\s*([^,]+),", source).group(1)
        self.assertEqual(set(re.findall(r"OBS_SOURCE_\w+", flags)),
                         {"OBS_SOURCE_AUDIO", "OBS_SOURCE_DEPRECATED"})
        self.assertIn('.id = "livemix_master_filter"', source)
        self.assertNotIn(".filter_audio", source)
        self.assertNotIn("lm_receiver", source)
        self.assertNotIn(".update", source)
        self.assertIn("OBS_TEXT_INFO_WARNING", source)
        self.assertIn('obs_module_text("Filter.Retired")', source)
        main = (ROOT / "src/plugin-main.c").read_text(encoding="utf-8")
        self.assertIn("obs_register_source(&livemix_master_filter)", main)

    def test_both_locales_explain_retirement_without_modes(self):
        messages = {
            "ko-KR": "이 필터는 더 이상 쓰지 않습니다. 지우고 OBS 소스(+)에서 'LiveMix'를 추가하세요.",
            "en-US": "This filter is no longer used. Remove it and add the 'LiveMix' source (+) in OBS instead.",
        }
        for locale, message in messages.items():
            text = (ROOT / f"data/locale/{locale}.ini").read_text(encoding="utf-8")
            self.assertIn(f'Filter.Retired="{message}"', text)
            for key in ("Filter.Mode", "Filter.Usage", "Filter.IdleWarning"):
                self.assertNotIn(key, text)


if __name__ == "__main__":
    unittest.main()
