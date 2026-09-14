"""Exercise the actual RA74 startup block with a disposable parameter file."""
from pathlib import Path
import shlex
import os
import subprocess
import tempfile

source = Path(__file__).resolve().parents[4]
shell = os.environ.get("NSS_TEST_SH", "sh")
init = source / 'package/nss/nss-tools/files/nss-dwmac.init'
text = init.read_text(encoding='utf-8')
subprocess.run([shell, '-n', str(init)], check=True)
block = text[text.index('\t# RA74 DMA descriptors:'):text.index('\t# Wi-Fi offload has to wait')]
assert text.index('modprobe qca-nss-drv') < text.index(block) < text.index('procd_open_instance nss-up')
parameter = '/sys/module/qca_nss_drv/parameters/meminfo_user_config'
expected = '<0, gmac_tx_desc_1, SDRAM>, <0, gmac_rx_desc_1, SDRAM>\n'
cases = [('xiaomi,redmi-ax5400', '', expected, 0),
         ('xiaomi,redmi-ax5400', 'explicit-override', 'explicit-override', 0),
         ('glinet,gl-b3000', '', '', 0),
         ('xiaomi,redmi-ax5400', None, None, 1)]
with tempfile.TemporaryDirectory(prefix='ra74-dma-check-') as directory:
    path = Path(directory) / 'parameter'
    for board, initial, result, code in cases:
        if initial is not None:
            path.write_text(initial, encoding='utf-8')
        elif path.exists():
            path.unlink()
        script = 'board_name() { echo ' + shlex.quote(board) + '; }\n'
        script += 'wifi_load() { echo "wifi-load:$1"; }\n'
        script += 'apply() {\n' + block.replace(parameter, shlex.quote(str(path))) + '\n}\napply\n'
        actual = subprocess.run([shell, '-c', script], capture_output=True, text=True)
        assert actual.returncode == code, (board, initial, actual.stderr)
        if result is not None:
            assert path.read_text(encoding='utf-8') == result
        else:
            assert 'wifi-load:0' in actual.stdout, 'Failure must preserve host Wi-Fi recovery'
print('PASS: RA74 default, explicit override, other board, missing parameter with Wi-Fi recovery')
