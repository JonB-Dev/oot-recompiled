"""Regression: out-of-order Metal argument IDs must not grow without bound."""

import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path


class SpirvCrossPaddingTests(unittest.TestCase):
    def test_out_of_order_ids_finish_with_bounded_output(self):
        source_path = (Path(__file__).resolve().parent.parent /
                       "build-cmake/rt64-src/src/tools/spirv_cross_msl/spirv_cross_msl.cpp")
        source = source_path.read_text()
        functions = source.split("int extract_id(", 1)[1].split("int main(", 1)[0]
        program = r'''
#include <algorithm>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
int extract_id(''' + functions + r'''
int main() {
    const std::string input =
        "struct spvDescriptorSetBuffer0\n{\n"
        "constant float* first [[id(2)]];\n"
        "constant float* earlier [[id(1)]];\n"
        "constant float* last [[id(4)]];\n};\n";
    const std::string output = pad_descriptor_sets(input);
    if (output.size() > 1000 ||
        output.find("_PadResource0 [[id(0)]]") == std::string::npos ||
        output.find("_PadResource1 [[id(1)]]") == std::string::npos ||
        output.find("_PadResource3 [[id(3)]]") == std::string::npos ||
        output.find("earlier [[id(1)]]") == std::string::npos)
        return 1;
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            test_source = Path(temporary) / "padding.cpp"
            executable = Path(temporary) / "padding"
            test_source.write_text(program)
            subprocess.run(["clang++", "-std=c++20", str(test_source), "-o", str(executable)],
                           check=True, capture_output=True, text=True, timeout=30)

            process = subprocess.Popen([str(executable)], stdout=subprocess.DEVNULL,
                                       stderr=subprocess.DEVNULL)
            try:
                deadline = time.monotonic() + 5
                while process.poll() is None:
                    self.assertLess(time.monotonic(), deadline, "padding converter timed out")
                    usage = subprocess.run(["ps", "-o", "rss=", "-p", str(process.pid)],
                                           capture_output=True, text=True, check=False)
                    if usage.stdout.strip():
                        self.assertLess(int(usage.stdout.strip()), 65536,
                                        "padding converter exceeded 64 MiB")
                    time.sleep(0.025)
                self.assertEqual(process.returncode, 0)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
