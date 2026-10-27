import importlib.util
from pathlib import Path
import unittest
spec=importlib.util.spec_from_file_location('qa',Path(__file__).with_name('device_qa.py'))
qa=importlib.util.module_from_spec(spec);spec.loader.exec_module(qa)

class DeviceQATests(unittest.TestCase):
    def test_requires_matching_version_integrity_and_telemetry(self):
        line='@QA version=1.17.0-dev.6 free=90000 largest=60000 minimum=80000 psram=7000000 integrity=1'
        self.assertEqual(qa.sample([line],'1.17.0-dev.6')['largest'],60000)
        for lines,version in [([], '1.17.0-dev.6'),([line],'wrong'),([line.replace('integrity=1','integrity=0')],'1.17.0-dev.6'),([line,'Guru Meditation Error'],'1.17.0-dev.6')]:
            with self.assertRaises(RuntimeError):qa.sample(lines,version)
    def test_leak_pressure_and_incomplete_are_distinct(self):
        def s(f,b=40000):return dict(free=f,largest=b)
        self.assertEqual(qa.assess([],30000,16000,8192),'incomplete')
        self.assertEqual(qa.assess([s(90000),s(70000)],30000,16000,8192),'needs_review')
        self.assertEqual(qa.assess([s(90000),s(89000,1000)],30000,16000,8192),'failed')
        self.assertEqual(qa.assess([s(90000),s(89000)],30000,16000,8192),'passed')
    def test_cycles_sample_open_and_closed_states(self):
        commands=[]
        def cmd(text):
            commands.append(text)
            if text=='apps':return ['@CURRENT '+('agentdeck' if commands[-2]=='open agentdeck' else 'home')]
            return ['@QA version=1.0.0 free=90000 largest=50000 minimum=80000 psram=0 integrity=1']
        results=qa.exercise(cmd,2,1,'1.0.0',pause=lambda _:None)
        self.assertEqual(len(results),5)
        self.assertEqual(commands.count('open agentdeck'),2)
        self.assertEqual(commands.count('home'),3)

if __name__=='__main__':unittest.main()
