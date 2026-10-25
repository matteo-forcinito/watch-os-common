"""Select an OTA slot without booting applications between flash operations.

ESP-IDF 6.1's OtatoolTarget passes obsolete positional arguments into the newer
ParttoolTarget constructor. Initialize the small target adapter with keywords;
use Espressif's unchanged sequence/CRC implementation for the actual selection.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

parser=argparse.ArgumentParser()
parser.add_argument('port')
parser.add_argument('--slot',type=int,choices=(0,1),default=1)
args=parser.parse_args()
components=Path(os.environ['IDF_PATH'])/'components'
sys.path.insert(0,str(components/'partition_table'))
sys.path.insert(0,str(components/'app_update'))
from otatool import OtatoolTarget, SPI_FLASH_SEC_SIZE
from parttool import ParttoolTarget

class KeywordOtaTarget(OtatoolTarget):
    def __init__(self,port):
        self.target=ParttoolTarget(port=port,esptool_args=['after=no-reset','no-stub'])
        self.spi_flash_sec_size=SPI_FLASH_SEC_SIZE
        with tempfile.NamedTemporaryFile(delete=False) as f:
            saved=Path(f.name)
        try:
            self.target.read_partition(self.OTADATA_PARTITION,str(saved))
            self.otadata=saved.read_bytes()
        finally:
            saved.unlink(missing_ok=True)

KeywordOtaTarget(args.port).switch_ota_partition(args.slot)
# The only application boot happens after slot selection is complete.
subprocess.run([sys.executable,'-m','esptool','--port',args.port,'--no-stub',
                '--after','hard-reset','read-mac'],check=True)
