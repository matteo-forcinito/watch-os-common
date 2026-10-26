import importlib.util
import json
import sqlite3
import tempfile
import time
import unittest
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

spec = importlib.util.spec_from_file_location('monitor', Path(__file__).with_name('monitor.py'))
m = importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

class MonitorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
    def tearDown(self): self.temp.cleanup()
    def transcript(self, events):
        path = self.root/'rollout.jsonl'
        from datetime import datetime, timezone
        date = datetime.now(timezone.utc).isoformat()
        path.write_text('\n'.join(json.dumps(dict(timestamp=date, **e)) for e in events), encoding='utf-8')
        return path
    def test_codex_work_wait_done_and_stale(self):
        path = self.transcript([dict(type='response_item', payload=dict(type='function_call', name='request_user_input',call_id='x'))])
        self.assertEqual(m.codex_activity(path)['status'],'needs_input')
        path = self.transcript([dict(type='event_msg',payload=dict(type='task_started'))])
        self.assertEqual(m.codex_activity(path)['status'],'working')
        self.assertEqual(m.codex_activity(path,time.time()+240)['status'],'unknown')
        path = self.transcript([dict(type='event_msg',payload=dict(type='task_complete'))])
        self.assertEqual(m.codex_activity(path)['status'],'done')
    def test_context_is_not_cumulative_tokens(self):
        path=self.transcript([dict(type='event_msg',payload=dict(type='token_count',info=dict(model_context_window=1000,last_token_usage=dict(total_tokens=100),total_token_usage=dict(total_tokens=200000)),rate_limits=dict(primary=dict(used_percent=25,resets_at=2000000000))))])
        result=m.codex_activity(path)
        self.assertEqual(result['context_percent'],10)
        self.assertEqual(result['tokens'],200000)
        self.assertEqual(result['quota'][0]['remaining_percent'],75)
        self.assertEqual(m.quota_windows({'primary':{'used_percent':120}}),[])
    def test_readonly_database_and_path_boundary(self):
        path=self.transcript([dict(type='event_msg',payload=dict(type='task_complete'))])
        db=self.root/'state_5.sqlite'
        with sqlite3.connect(db) as c:
            c.execute('create table threads(id text, rollout_path text, updated_at integer, title text, cwd text)')
            c.execute('insert into threads values(?,?,?,?,?)',('session-1',str(path),int(time.time()),'Test','project'))
            c.execute('insert into threads values(?,?,?,?,?)',('session-2',str(self.root.parent/'outside.jsonl'),int(time.time()),'Other','project'))
        c.close()
        before=db.read_bytes()
        sessions,error=m.codex_sessions(self.root)
        self.assertIsNone(error);self.assertEqual(len(sessions),1)
        self.assertEqual(db.read_bytes(),before)
    def test_jobs_persist_without_claiming_success(self):
        store=m.Store(self.root/'data')
        store.put(dict(id='build-1',status='working',pid=None))
        self.assertEqual(store.get('build-1')['status'],'working')
        store.put(dict(id='build-1',status='error',exit_code=2))
        self.assertEqual(store.get('build-1')['exit_code'],2)
    def test_bounded_device_snapshot(self):
        args=SimpleNamespace(data=self.root/'data',workspace=self.root)
        monitor=m.Monitor(args)
        full=dict(generated_at=time.time(),agents=[dict(id=str(i),agent='codex',name='Agent',status='working',activity='x'*240,observed_at=1,repo='repo',branch='main') for i in range(60)],jobs=[])
        raw=monitor.compact(full);result=json.loads(raw)
        self.assertLessEqual(len(result['cards']),16);self.assertLessEqual(len(raw),24000)
        self.assertEqual(result['total'],60)
    def test_redaction(self):
        self.assertNotIn('example-private-value',m.clean('token=example-private-value'))
        self.assertEqual(m.clean('a\x00b'),'ab')
        self.assertLessEqual(len(m.clean('è'*100,100).encode('utf-8')),100)
    def test_pairing_url_boundary(self):
        self.assertEqual(m.device_url('http://192.168.10.5/'),'http://192.168.10.5')
        for url in ['http://127.0.0.1','https://192.168.10.5','http://169.254.169.254','http://example.org','http://192.168.10.5/?token=x','http://name:secret@192.168.10.5']:
            with self.assertRaises(ValueError):m.device_url(url)
    def test_missing_claude_is_not_a_fake_agent(self):
        sessions,error=m.claude_sessions(self.root)
        self.assertEqual(sessions,[]);self.assertIn('nessuna sessione',error)
    def test_process_probe_never_stops_the_process(self):
        child=subprocess.Popen([sys.executable,'-c','import time;time.sleep(10)'])
        try:
            for _ in range(3): self.assertTrue(m.process_alive(child.pid))
            time.sleep(.15)
            self.assertIsNone(child.poll())
        finally: child.terminate();child.wait()
        self.assertFalse(m.process_alive(child.pid))
    def test_real_exit_codes_are_recorded(self):
        for code in (0,7):
            args=SimpleNamespace(command=[sys.executable,'-c',f'import sys;sys.exit({code})'],data=self.root/'jobs',cwd=self.root,title='Validation',agent='codex',kind='test')
            self.assertEqual(m.tracked_run(args),code)
            job=m.Store(args.data).all()[0]
            self.assertEqual(job['exit_code'],code)
            self.assertEqual(job['status'],'done' if code==0 else 'error')

if __name__=='__main__': unittest.main()
