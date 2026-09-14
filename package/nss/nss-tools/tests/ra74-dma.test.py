"""Exercise the actual GMAC descriptor-ring startup block with a disposable parameter file."""
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
block = text[text.index('\t# GMAC descriptor rings:'):text.index('\t# Wi-Fi offload has to wait')]
assert text.index('modprobe qca-nss-drv') < text.index(block) < text.index('procd_open_instance nss-up')
parameter = '/sys/module/qca_nss_drv/parameters/meminfo_user_config'
sdram = '<0, gmac_tx_desc_1, SDRAM>, <0, gmac_rx_desc_1, SDRAM>\n'
custom = '<0, gmac_rx_desc_1, SDRAM>'
# board, nss.general.meminfo (None = unset), parameter before (None = missing), parameter after, exit code
cases = [('xiaomi,redmi-ax5400', None, '', sdram, 0),
         ('xiaomi,redmi-ax5400', None, 'explicit-override', 'explicit-override', 0),
         ('glinet,gl-b3000', None, '', '', 0),
         ('xiaomi,redmi-ax5400', None, None, None, 1),
         ('linksys,mr5500', custom, '', custom + '\n', 0),
         ('xiaomi,redmi-ax5400', 'default', '', '', 0)]
with tempfile.TemporaryDirectory(prefix='ra74-dma-check-') as directory:
    path = Path(directory) / 'parameter'
    for board, uci, initial, result, code in cases:
        if initial is not None:
            path.write_text(initial, encoding='utf-8')
        elif path.exists():
            path.unlink()
        script = 'board_name() { echo ' + shlex.quote(board) + '; }\n'
        if uci is None:
            script += 'uci() { return 1; }\n'
        else:
            script += 'uci() { echo ' + shlex.quote(uci) + '; }\n'
        script += 'logger() { :; }\n'
        script += 'wifi_load() { echo "wifi-load:$1"; }\n'
        script += 'apply() {\n' + block.replace(parameter, shlex.quote(str(path))) + '\n}\napply\n'
        actual = subprocess.run([shell, '-c', script], capture_output=True, text=True)
        assert actual.returncode == code, (board, uci, initial, actual.stderr)
        if result is not None:
            assert path.read_text(encoding='utf-8') == result, (board, uci, initial)
        else:
            assert 'wifi-load:0' in actual.stdout, 'Failure must preserve host Wi-Fi recovery'
print('PASS: RA74 default, explicit override, other board, missing parameter with Wi-Fi recovery, '
      'uci value on another board, uci default on RA74')
